#pragma once
// DS3VR2 LENS (M4): the shape each eye is drawn with, from the headset's own per-eye angles (VR_ESSENTIALS §3; guide 08 §3;
// playbook FAIL-PERF-020 / FAIL-STR-009). Both engine views share ONE picture size and ONE engine fov (the views share
// their final targets, DS3VR2 PICSPLIT), so every mode gives both eyes the same centred lens size:
//   mode 0  SYMMETRIC: one centred lens wide enough for both eyes' outer edges (the old behaviour; ~49 % more pixels
//           on a Quest 3 than the eye really shows).
//   mode 1  OFF-CENTRE - the default (2026-10-30, JJ: "do the final fix for asymmetrical field of view"): each eye drawn with its own
//           true off-centre window (the runtime's four tangents), the eye camera straight ahead, declared exactly. The
//           engine builds that projection with its own off-centre builder 0x14004BB40 (OFFAXIS, offaxis.cpp); this
//           shape is the window's half extents (the picture = the runtime's recommended size).
//   mode 2  TURNED (AKN TURNEDEYE, VR_ESSENTIALS §3, JJ "98 % better" in AK): a centred lens just covering one eye, the
//           eye camera turned to the middle of that eye's view (Quest 3: ~7 deg out, ~5.5 deg down) and DECLARED turned.
//           Dark Souls III's lens is centred (one fov value, v1 FOVLAW), so this is how it draws each eye's own field.
// Angles follow OpenXR: radians, left / down negative; quaternions x, y, z, w in the eye's own frame (-Z forward).
#include <cmath>

namespace bvr::ds3::lens {

struct Shape {
    float hx = 0, hy = 0;        // half tangents of the centred lens (horizontal, vertical)
    float q[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}}; // per-eye turn
};

inline void quat_axis(float ax, float ay, float az, float a, float out[4]) {
    const float s = std::sin(a * 0.5f);
    out[0] = ax * s; out[1] = ay * s; out[2] = az * s; out[3] = std::cos(a * 0.5f);
}
inline void quat_mul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
// v rotated by q (inv = rotate by the inverse)
inline void quat_rot(const float q[4], const float v[3], float o[3], bool inv = false) {
    const float x = inv ? -q[0] : q[0], y = inv ? -q[1] : q[1], z = inv ? -q[2] : q[2], w = q[3];
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}

// The turn that points an eye at the middle of its own view: yaw to the horizontal centre, then pitch to the vertical
// one. A rotation of +a about +Y swings -Z to the left, so the yaw is -centreX; +b about +X tilts -Z up, so the pitch is
// +centreY.
inline void eye_turn(const float ang[4], float q[4]) {
    float qy[4], qp[4];
    quat_axis(0, 1, 0, -(ang[0] + ang[1]) * 0.5f, qy);
    quat_axis(1, 0, 0, (ang[2] + ang[3]) * 0.5f, qp);
    quat_mul(qy, qp, q);
}

// Half tangents a centred lens (turned by q) needs to contain the eye's own frustum: its four corner rays, seen from the
// turned frame (straight frustum edges map to straight lines, so the corners bound them).
inline void cover(const float ang[4], const float q[4], float& hx, float& hy) {
    hx = hy = 0;
    const float xs[2] = {std::tan(ang[0]), std::tan(ang[1])}, ys[2] = {std::tan(ang[2]), std::tan(ang[3])};
    for (float x : xs)
        for (float y : ys) {
            const float v[3] = {x, y, -1.0f};
            float t[3];
            quat_rot(q, v, t, true);
            if (t[2] >= -1e-4f) continue;
            hx = std::fmax(hx, std::fabs(t[0] / -t[2]));
            hy = std::fmax(hy, std::fabs(t[1] / -t[2]));
        }
}

// Both eyes' shape for a mode. `ang` = the runtime's angles of the left [0] and right [1] eye.
inline Shape shape(int mode, const float ang[2][4]) {
    Shape s;
    for (int e = 0; e < 2; ++e) {
        float hx = 0, hy = 0;
        if (mode == 2) {
            eye_turn(ang[e], s.q[e]);
            cover(ang[e], s.q[e], hx, hy);
        } else if (mode == 1) {
            hx = 0.5f * (std::tan(ang[e][1]) - std::tan(ang[e][0]));
            hy = 0.5f * (std::tan(ang[e][2]) - std::tan(ang[e][3]));
        } else {
            hx = std::fmax(std::tan(-ang[e][0]), std::tan(ang[e][1]));
            hy = std::fmax(std::tan(ang[e][2]), std::tan(-ang[e][3]));
        }
        s.hx = std::fmax(s.hx, hx);
        s.hy = std::fmax(s.hy, hy);
    }
    return s;
}

// The picture size for a shape: the runtime's recommended size gives its pixels per tangent unit over the eye's TRUE
// span; the centred lens spans 2 hx by 2 hy at that density, times the player's resolution slider. Multiples of 8.
inline void picture_size(const Shape& s, const float ang[4], unsigned recW, unsigned recH, float scale, unsigned& w,
                         unsigned& h) {
    const float spanX = std::tan(ang[1]) - std::tan(ang[0]), spanY = std::tan(ang[2]) - std::tan(ang[3]);
    const float dx = recW / spanX, dy = recH / spanY;
    auto r8 = [](float v) {
        unsigned u = (static_cast<unsigned>(v) + 4u) & ~7u;
        return u < 640u ? 640u : (u > 8192u ? 8192u : u);
    };
    w = r8(2.0f * s.hx * dx * scale);
    h = r8(2.0f * s.hy * dy * scale);
}

} // namespace bvr::ds3::lens

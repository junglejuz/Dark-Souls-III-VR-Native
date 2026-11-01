// DS3VR2 OFFAXIS - see offaxis.h.
#include "offaxis.h"

#include "addresses.h"
#include "stereo.h"
#include "core/util/log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <tuple>

namespace bvr::ds3::offaxis {
namespace {

constexpr uintptr_t kCenteredBuilder = 0x14004BA40;   // void* (out, fov, aspect, near, far)
constexpr uintptr_t kBuilderWrapper = 0x14004B860;    // void* (out, kind, fov, aspect, near, far) - culling (probe only)
constexpr uintptr_t kViewSetup = 0x14004B070;         // void* (cam, out) - the view's own projection (tiled path inside)
constexpr uintptr_t kPassSetup = 0x14004B3B0;         // void* (cam, out) - the same for other passes (+ TAA jitter)
constexpr uintptr_t kOffCentreBuilder = 0x14004BB40;
constexpr uintptr_t kFinaliser2 = 0x1400C52E0;         // void* (obj, view, proj) - culling planes at +0x150 (CULLPLANES2)
constexpr uintptr_t kFinaliser = 0x14006EB60;          // void* (renderCam, view, proj) - culling planes at +0x160 (CULLPLANES)  // void* (out, left, right, bottom, top, near, far)

using ProjFn = void*(__fastcall*)(void*, float, float, float, float);
using ProjWrapFn = void*(__fastcall*)(void*, int, float, float, float, float);
using CamFn = void*(__fastcall*)(void*, void*);
using OffFn = void*(__fastcall*)(void*, float, float, float, float, float, float);
ProjFn oProj = nullptr; ProjWrapFn oProjWrap = nullptr; CamFn oCamA = nullptr, oCamB = nullptr; OffFn oOff = nullptr;

std::atomic<bool> g_on{false};
std::atomic<float> g_win[2][4];                 // per eye: left, right, bottom, top tangents
std::atomic<float> g_tag[2] = {};               // each eye's link fov (0 = unknown); right = left + 0.001
std::atomic<uint32_t> g_made[2] = {}, g_untagged{0};
// MIRRORFLIP (V6, JJ: "all the reflections move around when you move your head"): DS3's reflections are a planar mirror.
// The engine renders the scene from an UPRIGHT camera mirrored about the reflecting plane and every material reads that
// picture back at (x, 1 - y) (g_ReflectionTexture t54, e.g. PS 002783eb). The flip lines up only for a lens symmetric top
// to bottom; the eye windows are not (top 0.82, bottom -1.21), so the reflection sat at the wrong height and slid with the
// head, and its edge showed as a light/dark line on the water. A mirror camera gets the eye's window flipped vertically
// (bottom' = -top, top' = -bottom). It is told from the eye's own camera by geometry: same x / z position (within 5 cm),
// a different height, and the forward row's vertical part negated.
std::atomic<int> g_mirrorFlip{1};
std::atomic<uint32_t> g_mirrorSeen[2] = {};
float g_eyeLink[2][16] = {};                     // each eye's link (rows + position), stereo.cpp, render thread
std::atomic<bool> g_eyeLinkOk[2] = {};
bool is_mirror(const void* cam, int e) {
    if (!g_eyeLinkOk[e].load(std::memory_order_relaxed)) return false;
    float c[16];
    __try { memcpy(c, cam, sizeof(c)); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    const float* L = g_eyeLink[e];
    const float dx = c[12] - L[12], dy = c[13] - L[13], dz = c[14] - L[14];
    // MIRRORFLIP2 (JJ 2026-10-31: the left eye sheared when the head moved): the game view's camera copies lag the head a
    // little, so "another height" at 2 cm caught them. A reflecting plane lies BELOW the eye and the mirror camera is as far
    // below the plane as the eye is above it: at least 0.3 m under the eye (world y is up).
    if (fabsf(dx) > 0.05f || fabsf(dz) > 0.05f || dy > -0.3f) return false;
    // forward row (+0x20): horizontal parts equal, vertical part negated (either sign convention of the row) - only a
    // real test when the eye looks up or down; level, the height alone decides
    for (float s : {1.0f, -1.0f})
        if (fabsf(c[8] - s * L[8]) < 0.02f && fabsf(c[10] - s * L[10]) < 0.02f &&
            (fabsf(L[9]) < 0.05f || (fabsf(c[9] + s * L[9]) < 0.02f && fabsf(c[9] - s * L[9]) > 0.04f)))
            return true;
    return false;
}
thread_local const void* t_cam = nullptr;
// CULLUNION (V13e, the gravestones popping at the right eye's outer edge): every caller of the view setup 0x14004B070 with an
// eye camera got that eye's EXACT window - including the culling path 0x1400522B0 -> 0x140138280 -> 0x1400BEEE0 ->
// 0x140203E90 -> 0x14004B070 (call 0x140203EE2), which CULLHUNT saw reading both views' links. So a culler fed by the main
// (left) view culled to the left window, and every fov widening was replaced by the exact window. Callers in g_unionRet
// now get the UNION of both eyes' windows x g_unionK instead.
thread_local uintptr_t t_camRet = 0;
thread_local int t_lastEye = -1;   // CULLPLANES: the eye whose window the last view / pass setup on this thread produced   // the return address of the view-setup call being served (rva, 0x140000000-based)
std::atomic<int> g_unionOn{0};   // OFF: on 0x1400CAF08 it culled in the MIDDLE too (JJ 22:14), x1.0 as well
std::atomic<float> g_unionK{1.10f};
// V13l: 0x1400CADD0 copies the MAIN link into a stack camera used ONLY for a per-frame scenery visibility pass (view, then
// 0x14004B070 -> return 0x1400CAF08, planes via 0x1400C52E0, then per-object visibility tests); offcallers: eye 0 only.
// It gets both eyes' window; 0x140203EE7 (the RENDER projection, V13e warped the picture) is NOT in the list any more.
uintptr_t g_unionRet[8] = {0x1400CAF08ull}; int g_nUnionRet = 1;
std::atomic<uint32_t> g_unionMade{0};
struct CallerRow { uintptr_t ret; int eye; uint32_t n; };
CallerRow g_callers[64]; int g_nCallers = 0;
SRWLOCK g_callerLock = SRWLOCK_INIT;       // the camera object whose projection is being built on this thread

uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
int eye_of(const void* cam) {
    if (!cam) return -1;
    float fov = 0;
    __try { fov = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(cam) + 0x50); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    const float l = g_tag[0].load(std::memory_order_relaxed), r = g_tag[1].load(std::memory_order_relaxed);
    if (l <= 0.0f) return -1;
    if (fabsf(fov - r) < 0.0004f) return 1;
    if (fabsf(fov - l) < 0.0004f) return 0;
    return -1;
}
int tile_count(const void* cam) {
    __try { return *reinterpret_cast<const int*>(static_cast<const uint8_t*>(cam) + 0x70); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void* build_eye(void* out, int e, float n, float f) {
    g_made[e].fetch_add(1, std::memory_order_relaxed);
    if (t_camRet) {   // the per-caller tally (`ds offcallers`)
        AcquireSRWLockExclusive(&g_callerLock);
        int i = 0;
        for (; i < g_nCallers; ++i) if (g_callers[i].ret == t_camRet && g_callers[i].eye == e) { ++g_callers[i].n; break; }
        if (i == g_nCallers && g_nCallers < 64) g_callers[g_nCallers++] = {t_camRet, e, 1};
        ReleaseSRWLockExclusive(&g_callerLock);
    }
    t_lastEye = (t_cam && is_mirror(t_cam, e)) ? -1 : e;   // CULLPLANES: the finaliser after this call widens this eye's planes
    if (g_unionOn.load(std::memory_order_relaxed) && t_camRet) {
        bool cull = false;
        for (int i = 0; i < g_nUnionRet; ++i) if (g_unionRet[i] == t_camRet) cull = true;
        if (cull) {   // CULLUNION: both eyes' windows, widened
            const float k = g_unionK.load(std::memory_order_relaxed);
            const float l = fminf(g_win[0][0].load(), g_win[1][0].load()) * k, r = fmaxf(g_win[0][1].load(), g_win[1][1].load()) * k;
            const float b = fminf(g_win[0][2].load(), g_win[1][2].load()) * k, t = fmaxf(g_win[0][3].load(), g_win[1][3].load()) * k;
            g_unionMade.fetch_add(1, std::memory_order_relaxed);
            return oOff(out, l * n, r * n, b * n, t * n, n, f);
        }
    }
    float b = g_win[e][2].load(std::memory_order_relaxed), t = g_win[e][3].load(std::memory_order_relaxed);
    if (g_mirrorFlip.load(std::memory_order_relaxed) && t_cam && is_mirror(t_cam, e)) {   // MIRRORFLIP
        if (g_mirrorSeen[e].fetch_add(1, std::memory_order_relaxed) < 3) {
            const float* c = static_cast<const float*>(t_cam);
            BVR_LOG("offaxis: mirror camera for eye %d at %.2f %.2f %.2f fwd %.3f %.3f %.3f (eye at %.2f %.2f %.2f fwd %.3f %.3f %.3f)", e,
                    c[12], c[13], c[14], c[8], c[9], c[10], g_eyeLink[e][12], g_eyeLink[e][13], g_eyeLink[e][14], g_eyeLink[e][8],
                    g_eyeLink[e][9], g_eyeLink[e][10]);
        }
        const float nb = -t; t = -b; b = nb;
    }
    return oOff(out, g_win[e][0].load(std::memory_order_relaxed) * n, g_win[e][1].load(std::memory_order_relaxed) * n, b * n, t * n, n, f);
}

// ---- PROJPROBE / CAMPROBE (logging only) ----
std::atomic<int> g_projLog{0}, g_camLog{0};
struct ProjRow { uintptr_t ret; float fov, aspect; uint32_t n; int kind; };
ProjRow g_projRows[32]; int g_projN = 0;
struct CamRow { const void* cam; float pos[3]; float fwd[3]; float fov, aspect; int tile; uint32_t nA, nB; bool mirror; };
CamRow g_camRows[24]; int g_camN = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
void proj_note(uintptr_t ret, float fov, float aspect, int kind) {
    AcquireSRWLockExclusive(&g_lock);
    const uintptr_t r = ret - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x140000000ull;
    int i = 0;
    for (; i < g_projN; ++i) if (g_projRows[i].ret == r && g_projRows[i].fov == fov && g_projRows[i].aspect == aspect) { ++g_projRows[i].n; break; }
    if (i == g_projN && g_projN < 32) g_projRows[g_projN++] = {r, fov, aspect, 1, kind};
    ReleaseSRWLockExclusive(&g_lock);
}
void cam_note(const void* cam, bool b) {
    const auto* c = static_cast<const uint8_t*>(cam);
    AcquireSRWLockExclusive(&g_lock);
    int i = 0;
    for (; i < g_camN; ++i) if (g_camRows[i].cam == cam) break;
    if (i == g_camN && g_camN < 24) {
        CamRow& r = g_camRows[g_camN++];
        r = {};
        r.cam = cam;
        __try {
            memcpy(r.pos, c + 0x30, 12);
            memcpy(r.fwd, c + 0x20, 12);
            const int e = eye_of(cam);
            r.mirror = e >= 0 && is_mirror(cam, e);
            r.fov = *reinterpret_cast<const float*>(c + 0x50); r.aspect = *reinterpret_cast<const float*>(c + 0x64);
            r.tile = *reinterpret_cast<const int*>(c + 0x70);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (i < 24) { if (b) ++g_camRows[i].nB; else ++g_camRows[i].nA; }
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- CULLPLANES (V13f) ----
// The render camera's finaliser 0x14006EB60(renderCam, view, proj) (called by 0x14006E9E0 right after the pass setup
// 0x14004B3B0) turns the projection into six WORLD-space culling planes at renderCam+0x160..+0x1BF (0x14006F690 transforms
// them). Only culling reads them - the picture uses the matrices. CULLUNION at the projection warped the picture (that
// projection is the RENDER one); here, after the finaliser, the four side planes of an eye camera are replaced by planes
// of the UNION of both eyes' windows x g_unionK, in the game's own plane convention (found by matching its six planes
// against planes computed for the eye's window). renderCam+0x240.. holds the camera link's rows (copied by 0x14006E9E0).
using FinalFn = void*(__fastcall*)(void*, void*, void*);
FinalFn oFinal = nullptr;
std::atomic<int> g_planesOn{0};   // OFF - did not stop the popping (x3 test)
std::atomic<uint32_t> g_planesDone{0}, g_planesNoMatch{0};
std::atomic<int> g_planesLogged{0};
void cand_plane(const float* R, const float* U, const float* F, const float* C, float nx, float ny, float nz, float out[4]) {
    float n[3];
    for (int i = 0; i < 3; ++i) n[i] = nx * R[i] + ny * U[i] + nz * F[i];
    const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-6f) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    for (int i = 0; i < 3; ++i) n[i] /= l;
    out[0] = n[0]; out[1] = n[1]; out[2] = n[2]; out[3] = -(n[0] * C[0] + n[1] * C[1] + n[2] * C[2]);
}
void widen_planes(float* P, const uint8_t* rowsAt, int e) {
    float rows[16];
    __try { memcpy(rows, rowsAt, sizeof(rows)); (void)P[0]; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    const float R[3] = {rows[0], rows[1], rows[2]}, U[3] = {rows[4], rows[5], rows[6]}, C[3] = {rows[12], rows[13], rows[14]};
    const float el = g_win[e][0].load(), er = g_win[e][1].load(), eb = g_win[e][2].load(), et = g_win[e][3].load();
    const float k = g_unionK.load(std::memory_order_relaxed);
    const float ul = fminf(g_win[0][0].load(), g_win[1][0].load()) * k, ur = fmaxf(g_win[0][1].load(), g_win[1][1].load()) * k;
    const float ub = fminf(g_win[0][2].load(), g_win[1][2].load()) * k, ut = fmaxf(g_win[0][3].load(), g_win[1][3].load()) * k;
    // the four side planes in view space (x right, y up, z forward): inside = n.x + d >= 0, through the camera position
    const float side[4][3] = {{1, 0, -el}, {-1, 0, er}, {0, 1, -eb}, {0, -1, et}};
    const float sideU[4][3] = {{1, 0, -ul}, {-1, 0, ur}, {0, 1, -ub}, {0, -1, ut}};
    // forward = +row2 or -row2, whichever matches the game's planes better
    float best = -1e9f; int bestF = 1;
    int map[4]; float sgn[4];
    for (int fs = -1; fs <= 1; fs += 2) {
        const float F[3] = {rows[8] * fs, rows[9] * fs, rows[10] * fs};
        float total = 0; int m[4]; float sg[4];
        for (int j = 0; j < 4; ++j) {
            float c[4]; cand_plane(R, U, F, C, side[j][0], side[j][1], side[j][2], c);
            float bj = -2; int bi = -1; float bs = 1;
            for (int i = 0; i < 6; ++i) {
                const float* q = P + 4 * i;
                const float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (ql < 1e-6f) continue;
                const float cs = (q[0] * c[0] + q[1] * c[1] + q[2] * c[2]) / ql;
                if (cs > bj) { bj = cs; bi = i; bs = 1; }
                if (-cs > bj) { bj = -cs; bi = i; bs = -1; }
            }
            total += bj; m[j] = bi; sg[j] = bs;
        }
        if (total > best) { best = total; bestF = fs; for (int j = 0; j < 4; ++j) { map[j] = m[j]; sgn[j] = sg[j]; } }
    }
    if (best < 3.99f) {   // every side plane must match its game plane to within ~3 degrees
        g_planesNoMatch.fetch_add(1, std::memory_order_relaxed);
        if (g_planesLogged.fetch_add(1) < 3)
            BVR_LOG("cullplanes: eye %d - the game's planes did not match (score %.4f of 4) - left alone", e, best);
        return;
    }
    const float F[3] = {rows[8] * bestF, rows[9] * bestF, rows[10] * bestF};
    for (int j = 0; j < 4; ++j) {
        float* q = P + 4 * map[j];
        const float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        float c[4]; cand_plane(R, U, F, C, side[j][0], side[j][1], side[j][2], c);
        // the game's d convention: q.w = s * ql * c.d, or the opposite sign
        const float dA = sgn[j] * ql * c[3];
        const float dsign = fabsf(q[3] - dA) <= fabsf(q[3] + dA) ? 1.0f : -1.0f;
        float u[4]; cand_plane(R, U, F, C, sideU[j][0], sideU[j][1], sideU[j][2], u);
        q[0] = sgn[j] * ql * u[0]; q[1] = sgn[j] * ql * u[1]; q[2] = sgn[j] * ql * u[2]; q[3] = dsign * sgn[j] * ql * u[3];
    }
    g_planesDone.fetch_add(1, std::memory_order_relaxed);
    if (g_planesLogged.fetch_add(1) < 3)
        BVR_LOG("cullplanes: eye %d - side planes %d %d %d %d replaced by both eyes' window x%.2f (match %.4f of 4, forward %c row 2)", e,
                map[0], map[1], map[2], map[3], k, best, bestF > 0 ? '+' : '-');
}
void* __fastcall hkFinal(void* rc, void* view, void* proj) {
    const int e = t_lastEye;
    t_lastEye = -1;
    void* r = oFinal(rc, view, proj);
    // only the eye camera itself: its copied link (renderCam+0x240, fov at +0x290) must carry this eye's fov tag, and no mirror
    if (e >= 0 && g_planesOn.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed)) {
        const void* link = static_cast<const uint8_t*>(rc) + 0x240;
        if (eye_of(link) == e && !is_mirror(link, e))
            widen_planes(reinterpret_cast<float*>(static_cast<uint8_t*>(rc) + 0x160), static_cast<const uint8_t*>(link), e);
    }
    return r;
}
// CULLPLANES2 (V13g, JJ: the gravestones still popped right after loading in, then not): a second plane set - 0x1400C5060
// (~150 calls a frame per eye) builds view + projection from a camera link stored at obj+0x210 and calls 0x1400C52E0(obj,
// view, proj), which stores six world planes (0x14006F690) at obj+0x150..+0x1AF. Same widening, rows from the link itself.
FinalFn oFinal2 = nullptr;
std::atomic<uint32_t> g_planes2Done{0};
const void* link_of(void* obj) {
    __try { return *reinterpret_cast<void* const*>(static_cast<uint8_t*>(obj) + 0x210); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* __fastcall hkFinal2(void* obj, void* view, void* proj) {
    const int e = t_lastEye;
    t_lastEye = -1;
    void* r = oFinal2(obj, view, proj);
    if (e >= 0 && g_planesOn.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed)) {
        const void* link = link_of(obj);
        if (link && eye_of(link) == e && !is_mirror(link, e)) {
            widen_planes(reinterpret_cast<float*>(static_cast<uint8_t*>(obj) + 0x150), static_cast<const uint8_t*>(link), e);
            g_planes2Done.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return r;
}

// ---- hooks ----
void* __fastcall hkProj(void* out, float fov, float aspect, float n, float f) {
    if (g_projLog.load(std::memory_order_relaxed) > 0) proj_note(reinterpret_cast<uintptr_t>(_ReturnAddress()), fov, aspect, 0);
    if (g_on.load(std::memory_order_relaxed) && t_cam) {
        const int e = eye_of(t_cam);
        if (e >= 0) return build_eye(out, e, n, f);
        // MIRRORFLIP: a reflection camera that did not inherit the eye's fov tag, told by geometry
        if (g_mirrorFlip.load(std::memory_order_relaxed))
            for (int k = 0; k < 2; ++k) if (is_mirror(t_cam, k)) return build_eye(out, k, n, f);
        g_untagged.fetch_add(1, std::memory_order_relaxed);
    }
    return oProj(out, fov, aspect, n, f);
}
void* __fastcall hkOff(void* out, float l, float r, float b, float t, float n, float f) {
    // the view setup's tiled path: one tile = the whole frustum, which is where the eye's window goes instead
    if (g_on.load(std::memory_order_relaxed) && t_cam && tile_count(t_cam) == 1) {
        const int e = eye_of(t_cam);
        if (e >= 0) return build_eye(out, e, n, f);
        if (g_mirrorFlip.load(std::memory_order_relaxed))   // MIRRORFLIP (untagged reflection camera)
            for (int k = 0; k < 2; ++k) if (is_mirror(t_cam, k)) return build_eye(out, k, n, f);
    }
    return oOff(out, l, r, b, t, n, f);
}
void* __fastcall hkProjWrap(void* out, int kind, float fov, float aspect, float n, float f) {
    if (g_projLog.load(std::memory_order_relaxed) > 0) proj_note(reinterpret_cast<uintptr_t>(_ReturnAddress()), fov, aspect, 1 + kind);
    return oProjWrap(out, kind, fov, aspect, n, f);
}
void* __fastcall hkCamA(void* cam, void* out) {
    if (g_camLog.load(std::memory_order_relaxed) > 0) cam_note(cam, false);
    const void* prev = t_cam; t_cam = cam;
    const uintptr_t prevRet = t_camRet;
    t_camRet = reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x140000000ull;
    void* r = oCamA(cam, out);
    t_camRet = prevRet;
    t_cam = prev;
    return r;
}
void* __fastcall hkCamB(void* cam, void* out) {
    if (g_camLog.load(std::memory_order_relaxed) > 0) cam_note(cam, true);
    const void* prev = t_cam; t_cam = cam;
    const uintptr_t prevRet = t_camRet;
    t_camRet = reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x140000000ull;
    void* r = oCamB(cam, out);
    t_camRet = prevRet;
    t_cam = prev;
    return r;
}

char g_status[200] = "offaxis: not installed";

} // namespace

void install() {
    int ok = 0;
    for (auto [va, det, orig] : {std::tuple<uintptr_t, void*, void**>{kCenteredBuilder, reinterpret_cast<void*>(&hkProj), reinterpret_cast<void**>(&oProj)},
                                 {kBuilderWrapper, reinterpret_cast<void*>(&hkProjWrap), reinterpret_cast<void**>(&oProjWrap)},
                                 {kViewSetup, reinterpret_cast<void*>(&hkCamA), reinterpret_cast<void**>(&oCamA)},
                                 {kPassSetup, reinterpret_cast<void*>(&hkCamB), reinterpret_cast<void**>(&oCamB)},
                                 {kOffCentreBuilder, reinterpret_cast<void*>(&hkOff), reinterpret_cast<void**>(&oOff)},
                                 {kFinaliser, reinterpret_cast<void*>(&hkFinal), reinterpret_cast<void**>(&oFinal)},
                                 {kFinaliser2, reinterpret_cast<void*>(&hkFinal2), reinterpret_cast<void**>(&oFinal2)}}) {
        void* f = reinterpret_cast<void*>(addr::rebase(va));
        const MH_STATUS a = MH_CreateHook(f, det, orig);
        const MH_STATUS b = a == MH_OK ? MH_EnableHook(f) : a;
        if (b == MH_OK) ++ok;
        BVR_LOG("offaxis: %llX hooked: %s / %s", static_cast<unsigned long long>(va), MH_StatusToString(a), MH_StatusToString(b));
    }
    // Self-check: the engine's off-centre builder with a centred window must give exactly what its centred builder gives
    // (same convention, so the swap changes nothing but the window).
    if (oProj && oOff) {
        alignas(16) float a[16] = {}, b[16] = {};
        const float fov = 1.7904f, asp = 0.9219f, n = 0.1f, f = 1000.0f;
        oProj(a, fov, asp, n, f);
        const float t = n * tanf(fov * 0.5f), r = t * asp;
        oOff(b, -r, r, -t, t, n, f);
        float worst = 0;
        for (int i = 0; i < 16; ++i) worst = fmaxf(worst, fabsf(a[i] - b[i]) / fmaxf(1.0f, fabsf(a[i])));
        BVR_LOG("offaxis: self-check centred vs off-centre builder, same window: largest difference %.2e (%s) | %.4f %.4f %.4f %.4f",
                worst, worst < 1e-4f ? "same convention" : "DIFFERENT - off-centre lens stays off", a[0], a[5], a[10], a[14]);
        if (worst >= 1e-4f) ok = -99;
    }
    snprintf(g_status, sizeof(g_status), "offaxis: %d of 5 hooks%s", ok < 0 ? 0 : ok, ok < 0 ? ", builders disagree - not used" : "");
    if (ok < 0) { oOff = nullptr; }
}
void set_on(bool on) {
    const bool was = g_on.exchange(on && oOff != nullptr);
    if (was != g_on.load()) BVR_LOG("offaxis: true off-centre lens %s", g_on.load() ? "ON" : "off");
}
bool on() { return g_on.load(); }
void set_window(int eye, float l, float r, float b, float t) {
    if (eye < 0 || eye > 1) return;
    g_win[eye][0].store(l); g_win[eye][1].store(r); g_win[eye][2].store(b); g_win[eye][3].store(t);
}
void set_tags(float leftFov, float rightFov) { g_tag[0].store(leftFov); g_tag[1].store(rightFov); }
int eye_of_cam(const void* cam) { return eye_of(cam); }
bool mirror_cam(const void* cam, int eye) { return eye >= 0 && eye < 2 && g_mirrorFlip.load(std::memory_order_relaxed) && cam && is_mirror(cam, eye); }
void set_eye_link(int eye, const float rec[16]) {
    if (eye < 0 || eye > 1 || !rec) return;
    memcpy(g_eyeLink[eye], rec, sizeof(g_eyeLink[eye]));
    g_eyeLinkOk[eye].store(true, std::memory_order_relaxed);
}
void set_mirror_flip(int on) {
    g_mirrorFlip.store(on ? 1 : 0);
    BVR_LOG("offaxis: mirror cameras (reflections) %s", on ? "get the eye's window flipped top to bottom" : "get the eye's window unchanged (old)");
}
void window(int eye, float out[4]) {
    for (int k = 0; k < 4; ++k) out[k] = eye == 0 || eye == 1 ? g_win[eye][k].load(std::memory_order_relaxed) : 0.0f;
}
const char* status() {
    static char s[260];
    snprintf(s, sizeof(s), "%s | on %d | eye projections built L %u R %u, untagged eye-shaped builds %u | mirror flip %d: mirror builds L %u R %u",
             g_status, g_on.load() ? 1 : 0, g_made[0].load(), g_made[1].load(), g_untagged.load(), g_mirrorFlip.load(),
             g_mirrorSeen[0].load(), g_mirrorSeen[1].load());
    return s;
}

void proj_probe() {
    if (g_projLog.exchange(1) == 1) return;
    std::thread([] {
        Sleep(1000);
        g_projLog.store(0);
        AcquireSRWLockExclusive(&g_lock);
        BVR_LOG("projprobe: %d distinct (caller, fov, aspect) in 1 s", g_projN);
        for (int i = 0; i < g_projN; ++i)
            BVR_LOG("projprobe   %s caller %llX fov %.5f (%.2f deg) aspect %.5f x%u", g_projRows[i].kind ? "wrapper" : "builder",
                    (unsigned long long)g_projRows[i].ret, g_projRows[i].fov, g_projRows[i].fov * 57.29578f, g_projRows[i].aspect, g_projRows[i].n);
        g_projN = 0;
        ReleaseSRWLockExclusive(&g_lock);
    }).detach();
}
void cam_probe() {
    if (g_camLog.exchange(1) == 1) return;
    std::thread([] {
        Sleep(1000);
        g_camLog.store(0);
        void* ml = nullptr; void* ol = nullptr;
        stereo::links(ml, ol);
        float mp[3] = {}, op[3] = {};
        if (ml) memcpy(mp, static_cast<uint8_t*>(ml) + 0x30, 12);
        if (ol) memcpy(op, static_cast<uint8_t*>(ol) + 0x30, 12);
        AcquireSRWLockExclusive(&g_lock);
        BVR_LOG("camprobe: main link %p pos %.3f %.3f %.3f | own link %p pos %.3f %.3f %.3f | tags L %.7f R %.7f | %d camera objects in 1 s",
                ml, mp[0], mp[1], mp[2], ol, op[0], op[1], op[2], g_tag[0].load(), g_tag[1].load(), g_camN);
        for (int i = 0; i < g_camN; ++i) {
            const CamRow& r = g_camRows[i];
            BVR_LOG("camprobe   cam %p eye %d pos %.3f %.3f %.3f fwd %.3f %.3f %.3f fov %.7f aspect %.4f tile %d | setup A x%u, B x%u%s", r.cam,
                    eye_of(r.cam), r.pos[0], r.pos[1], r.pos[2], r.fwd[0], r.fwd[1], r.fwd[2], r.fov, r.aspect, r.tile, r.nA, r.nB,
                    r.mirror ? " | MIRROR camera (window flipped)" : "");
        }
        g_camN = 0;
        ReleaseSRWLockExclusive(&g_lock);
    }).detach();
}

// CULLUNION commands: "offunion 0|1 [k]" and "offcallers" (which callers of the view setup got which eye's window)
void union_cmd(const char* args) {
    int on = 1; float k = 0.0f;
    const int n = sscanf_s(args, "%d %f", &on, &k);
    if (n >= 1) g_unionOn.store(on);
    if (n >= 2 && k >= 1.0f && k <= 3.0f) g_unionK.store(k);
    BVR_LOG("offunion %d x%.2f: culling callers given both eyes' window %u times", g_unionOn.load(), g_unionK.load(), g_unionMade.load());
}
void union_two(uintptr_t a, uintptr_t b, float k);
void union_only(uintptr_t ret, float k) { union_two(ret, 0, k); }
void union_two(uintptr_t a, uintptr_t b, float k) {   // POPSWEEP3: CULLFIX's caller plus one candidate
    g_unionOn.store(0);
    g_nUnionRet = 0;
    if (a) g_unionRet[g_nUnionRet++] = a;
    if (b && b != a) g_unionRet[g_nUnionRet++] = b;
    const uintptr_t ret = g_nUnionRet ? g_unionRet[0] : 0;
    if (k >= 1.0f && k <= 3.0f) g_unionK.store(k);
    if (ret) g_unionOn.store(1);
}
int callers_list(uintptr_t* out, int max) {   // POPSWEEP2: every distinct caller that got an eye window so far
    AcquireSRWLockExclusive(&g_callerLock);
    int n = 0;
    for (int i = 0; i < g_nCallers; ++i) {
        bool seen = false;
        for (int j = 0; j < n; ++j) if (out[j] == g_callers[i].ret) seen = true;
        if (!seen && n < max) out[n++] = g_callers[i].ret;
    }
    ReleaseSRWLockExclusive(&g_callerLock);
    return n;
}
void planes_cmd(const char* args) {
    int on = 1; float k = 0.0f;
    const int n = sscanf_s(args, "%d %f", &on, &k);
    if (n >= 1) g_planesOn.store(on);
    if (n >= 2 && k >= 1.0f && k <= 3.0f) g_unionK.store(k);
    BVR_LOG("cullplanes %d x%.2f: plane sets widened %u (second set %u), not matched %u", g_planesOn.load(), g_unionK.load(),
            g_planesDone.load(), g_planes2Done.load(), g_planesNoMatch.load());
}
void callers_log() {
    AcquireSRWLockExclusive(&g_callerLock);
    for (int i = 0; i < g_nCallers; ++i)
        BVR_LOG("offcallers: view setup called from %llX for eye %d - %u times%s", static_cast<unsigned long long>(g_callers[i].ret),
                g_callers[i].eye, g_callers[i].n, [&] { for (int j = 0; j < g_nUnionRet; ++j) if (g_unionRet[j] == g_callers[i].ret) return " (union)"; return ""; }());
    ReleaseSRWLockExclusive(&g_callerLock);
}

} // namespace bvr::ds3::offaxis

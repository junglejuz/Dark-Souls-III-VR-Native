// DS3VR2 MARKERS (M8) - see markers.h for the method and the evidence it rests on.
#include "markers.h"

#include "addresses.h"
#include "camera.h"
#include "picsplit.h"
#include "settings.h"
#include "shaderid.h"

#include "core/util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bvr::ds3::markers {
namespace {

// M8 DS3 (STATIC 2026-10-31): Dark Souls III 1.15.2's marker projection = 0x1406C73B0 (holder, out, viewPoint): [holder]
// is the HUD's camera (ChrCam layout: fov +0x50, aspect +0x54, near +0x58, far +0x5C); 0x14062D4F0 builds its centred
// lens from those, the camera-space point (x right, y up, z ahead, w 1) goes through it, / w, x 0.5 + 0.5, then onto
// the 1920x1080 stage (or x 1280 on the other branch). Its callers (0x1406C6028, 0x1406C69CB: the lock-on / enemy-bar
// updates, 0x1408FAA5D / 0x1408FAA85) take world -> camera space with the camera's view rows and check z > near first.
// 0x1406C74D0 is the matching on-stage test (0..1920, 0..1080, z > 0). Sekiro's was 0x140C69B30 (out, viewPoint, flag).
constexpr uintptr_t kToStage = 0x1406C73B0;
// v1 checked these bytes before hooking (skvr/src/hooks.cpp MARKERZ); same build, same check.
// mov [rsp+8], rbx / push rdi / sub rsp, 60h / mov rcx, [rcx] / mov rdi, rdx
constexpr uint8_t kToStageBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0xFA};

using ToStageFn = void*(__fastcall*)(void* holder, float* out, const float* viewPoint);
ToStageFn oToStage = nullptr;
std::atomic<bool> g_installed{false};

// ---- settings / switches -------------------------------------------------------------------------------------
std::atomic<int> g_on{1};           // markers: 0 = everything stays on the HUD panel (the control)
std::atomic<int> g_base{1};         // MARKERBASE: lay the marker out with the head-free camera (guide §4)
std::atomic<int> g_facePos{1};      // §5c: the card faces the game camera's POSITION, not its heading
std::atomic<float> g_reach{0.12f};  // how far a HUD piece may sit from a marker point and still belong to it (NDC)
std::atomic<float> g_smooth{1.0f};  // v1 MARKERSMOOTH, JJ "that actually worked": a moving target's own wobble
std::atomic<int> g_probe{0};        // `ds mkdump N`: log the next N frames' points and pieces
std::atomic<int> g_allProbe{0};     // ... the next N HUD pieces' own placement constants ...
std::atomic<int> g_pieceProbe{0};   // ... and where the next N MATCHED pieces' cards land in each eye
std::atomic<float> g_testPt[3] = {};   // MKTEST: HUD x, y and the distance (0 = off)
std::atomic<int> g_matchProbe{0};   // `ds mkdump N`: the next N x 40 pieces' match (V11)
std::atomic<int> g_stencil{1};      // MKSTENCIL (V12): the marker layers get their own depth-stencil, so clipped pieces (a bar's
                                    // fill) are clipped there too - Batman City HUDDSV, the same lesson for the card layers
std::atomic<int> g_maskLayer{1};    // MKMASK (V12): a clip mask that belongs to a marker is ALSO drawn (colour off) into its layer
std::atomic<int> g_follow{0};       // MKFOLLOW (V12): a piece drawn right after a marker's piece, within mkfollowreach of that
std::atomic<float> g_followReach{0.40f};   // marker, belongs to it (a bar's fill sits away from the bar's anchor)
std::atomic<uint32_t> g_followTaken{0}, g_maskTaken{0};
struct Last { float w[3]; uint32_t seq; bool ok; };
Last g_last{};                      // the last marker a HUD piece went to, and that piece's draw number (render thread only)
uint32_t g_drawSeq = 0;
std::atomic<int> g_region{1};       // MKREGION (V12g): HUD triangles are sorted onto markers ON THE GPU by where they are, not by
                                    // a piece's anchor (DS3 draws many pieces - the bar's fill - in absolute stage coordinates,
                                    // anchor = stage centre: the CPU match could never place them = flat fill, duplicates)
std::atomic<float> g_regionX{0.12f}, g_regionY{0.08f};
std::atomic<int> g_regionMasks{1};   // V12h: masks into the layers (crashed - off; `ds mkregionmasks 1` only for a test)   // the region around a marker point (HUD NDC half-sizes)
std::atomic<int> g_framesLog{0};
std::atomic<int> g_farLog{60};   // V12p
std::atomic<int> g_trackOn{1};
std::atomic<int> g_keepSpecial{1};
std::atomic<int> g_maskWhole{1};
uint64_t g_hide[8] = {0xfa3d4612573b6d52ull}; int g_nHide = 1;   // V12z HALOHIDE: shaders whose pieces near a marker are hidden
std::atomic<uint32_t> g_hidden{0};
// DRAWDIFF (V12y, the halo pulses on the card once a second; blend / stencil / mask ideas all failed): every region draw of
// the frame is recorded (shader, vertex range, blend, write mask, depth-stencil, stencil ref, mask, first anchor); while
// `ds mkframes` runs, a frame whose list differs from the one before logs what came and went.
struct DrawRec { uint64_t vs; unsigned count, start; int base; uint32_t blend, dss; unsigned ref; bool mask; float ax, ay; uint64_t cbHash; float cb[12]; };
thread_local uint64_t t_cbHash = 0;
thread_local float t_cb[12] = {};
std::vector<DrawRec> g_drawsNow, g_drawsPrev;   // V12x `ds mkmaskwhole 0|1`   // V12w `ds mkblend 0|1`
std::atomic<uint32_t> g_specialDraws{0};   // V12t MKTRACK, `ds mktrack 0|1`
uint32_t g_catchCardsAcc = 0, g_catchRegionAcc = 0;   // V12q: this frame's, render thread
std::atomic<int> g_layerHold{4};   // V12n `ds mkhold N`
std::atomic<uint32_t> g_cardsF{0}, g_regionF{0};
std::atomic<uint32_t> g_regionDraws{0}, g_regionCards{0}, g_regionHeld{0}, g_regionFar{0}, g_regionNotTri{0};
thread_local ID3D11GeometryShader* t_flatGs = nullptr;
thread_local ID3D11Buffer* t_flatCb = nullptr;
std::atomic<int> g_vpNow{1};         // MKVPNOW (V12e)
std::atomic<uint32_t> g_vpNowUsed{0};
std::atomic<int> g_world{3};        // MKWORLD (V12c): 0 head camera now, 1 head-free game camera, 2 head camera one frame
                                    // back, 3 auto = the one whose world points move least between frames (JJ: the bar
                                    // and lock-on jitter with head movement - the point is rebuilt with the wrong camera)
std::atomic<int> g_worldPick{0};
double g_worldMove[3] = {}; uint32_t g_worldN = 0; uint64_t g_worldAt = 0;
struct WTrack { uint32_t caller; float w[3][3]; uint64_t at; };
WTrack g_wt[32]; int g_wtN = 0;
camera::Cam g_headNow{}, g_headPrev{}; uint32_t g_headGen = 0xFFFFFFFFu;
std::mutex g_worldMutex;
std::atomic<int> g_holderCam{1};    // MKHOLDER (V12): rebuild the world point with the camera the projection itself used
std::atomic<uint64_t> g_holderUsed{0}, g_holderMiss{0};
std::atomic<float> g_holderGap{0.0f};  // last |holder camera - our published head camera| (m), for the log

// ---- the marker points of this game frame --------------------------------------------------------------------
struct Point {
    float w[3];          // world
    float stage[2];      // the game's own 1920x1080 stage position (head-free when MARKERBASE is on)
    float ndc[2];        // stage mapped into the HUD sheet's own coordinates (fitted map)
    float z;             // distance in front of the head-free camera
    uint32_t caller;      // RVA of the placement, for the log
    bool ok;
};
constexpr int kMaxPoints = 64;
Point g_pts[2][kMaxPoints];
// MKTRACK (V12t, fillcatch 18:19: the lock-on dot flat on the sheet in frames that drew 38 / 8 card pieces instead of 46 -
// the Present switched buckets while the game thread was half way through projecting, so one marker had no point that
// frame): every marker point also updates a TRACK (same caller, within 0.3 m), kept 120 ms without an update. The region
// path uses the live tracks - one point per marker, every draw.
struct TrackPt { Point p; uint64_t at; bool used; };
TrackPt g_trk[16];          // [0] this game frame, [1] the one before (the render thread may lag by one)
int g_nPts[2] = {};
int g_cur = 0;
std::mutex g_ptMutex;

// ---- the two measured maps ----------------------------------------------------------------------------------
// (a) camera space -> stage, asked of the game's own function once a frame with three known points.
//     stage.x = cx + sx * (vx / vz),  stage.y = cy + sy * (vy / vz)
struct Cal { float cx, cy, sx, sy; bool ok; };
Cal g_cal{960.0f, 540.0f, 960.0f, -960.0f, false};
// (b) stage -> the HUD sheet's own coordinates:
//     ndc.x = ax * stage.x + bx,  ndc.y = ay * stage.y + by. DERIVED, not fitted: run 03:15 showed a least-squares
//     fit being polluted (one marker point on screen against 30,000 HUD pieces a run, so almost any piece counted as
//     a pair and the map came out at half-angles 3.6 x 3.96 against the engine's 1.15 x 1.25). Two independent
//     derivations agree instead - Scaleform ShowAll (the 1920x1080 stage scaled uniformly to the sheet's WIDTH) and
//     our own calibration chained with the sheet's width (960 / 834.5 = 1.1504, exactly the engine's own horizontal
//     half-angle) - so the map is computed, and the sums below only MEASURE how far the pieces really sit from it.
struct Fit {
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxn = 0, syn = 0, sn = 0;   // sums for x and y
    double nx = 0, ny = 0;                                              // sums of the observed ndc
    long n = 0;
    float ax = 1.0f / 960.0f, bx = -1.0f, ay = -1.0f / 960.0f, by = 540.0f / 960.0f;
    double resid = 0; long residN = 0;      // mean distance of a matched piece from where the map puts its marker
};
Fit g_fit;
std::mutex g_fitMutex;

// ---- what reached the eyes ------------------------------------------------------------------------------------
std::atomic<uint32_t> g_placed{0}, g_matched{0}, g_unmatched{0}, g_gsBuilt{0}, g_gsFailed{0}, g_noCode{0}, g_drawn{0};
std::atomic<uint32_t> g_pointsSeen{0};
char g_lastErr[256] = "";

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// ---------------------------------------------------------------------------------------------------------------
// MARKERSMOOTH (v1 run 133, JJ "that actually worked"): an enemy bar rides a point on a body that bobs with every
// step. The point is filtered in WORLD space (One Euro), so head and stick turns stay exact and only the target's own
// wobble is damped. Slider 0 = off.
// ---------------------------------------------------------------------------------------------------------------
struct Track { uint32_t caller; float raw[3], f[3], dx[3]; uint64_t qpc; bool used; };
Track g_tracks[96];
int g_nTracks = 0;
std::mutex g_trackMutex;
float oe_alpha(float cutoff, float dt) { const float tau = 1.0f / (6.2831853f * cutoff); return 1.0f / (1.0f + tau / dt); }

bool smooth_world(uint32_t caller, float* W) {
    const float strength = g_smooth.load(std::memory_order_relaxed);
    if (strength <= 0.001f) return false;
    LARGE_INTEGER q, fq;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&fq);
    std::lock_guard<std::mutex> lk(g_trackMutex);
    int best = -1;
    float bd = 1.5f;
    for (int i = 0; i < g_nTracks; ++i) {
        Track& t = g_tracks[i];
        if (t.caller != caller || static_cast<double>(q.QuadPart - t.qpc) / static_cast<double>(fq.QuadPart) > 0.25) continue;
        const float d = fabsf(t.raw[0] - W[0]) + fabsf(t.raw[1] - W[1]) + fabsf(t.raw[2] - W[2]);
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) {
        int k = g_nTracks < 96 ? g_nTracks++ : 0;
        if (g_nTracks >= 96) for (int i = 1; i < 96; ++i) if (g_tracks[i].qpc < g_tracks[k].qpc) k = i;
        Track& t = g_tracks[k];
        t.caller = caller; t.qpc = q.QuadPart; t.used = true;
        for (int i = 0; i < 3; ++i) { t.raw[i] = W[i]; t.f[i] = W[i]; t.dx[i] = 0; }
        return false;
    }
    Track& t = g_tracks[best];
    const float dt = static_cast<float>(static_cast<double>(q.QuadPart - t.qpc) / static_cast<double>(fq.QuadPart));
    if (dt <= 1e-5f) return false;
    const float minCut = 4.0f * powf(0.15f, strength), beta = 0.8f, dCut = 1.0f;
    for (int i = 0; i < 3; ++i) {
        const float raw = W[i];
        const float dxRaw = (raw - t.raw[i]) / dt;
        const float ad = oe_alpha(dCut, dt);
        t.dx[i] += ad * (dxRaw - t.dx[i]);
        const float cut = minCut + beta * fabsf(t.dx[i]);
        const float a = oe_alpha(cut, dt);
        t.f[i] += a * (raw - t.f[i]);
        t.raw[i] = raw;
        W[i] = t.f[i];
    }
    t.qpc = q.QuadPart;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The projection hook. Three jobs, in this order:
//   1. turn the camera-space point into a WORLD point (the camera the game is placing with = the head camera we wrote);
//   2. smooth it, then hand the game the HEAD-FREE camera's version of it, so the HUD lays the marker out at a place
//      that does not move with the head (MARKERBASE, guide §4 - the reference view must not contain the head);
//   3. keep (world point, stage point) for this frame.
// ---------------------------------------------------------------------------------------------------------------
void calibrate_with(void* holder);
std::atomic<uint64_t> g_calAt{0};
bool read_holder_cam(void* holder, float (*hr)[4]) {   // no C++ objects here: __try needs a frame without unwinding
    __try {
        const uint8_t* cam = *static_cast<uint8_t* const*>(holder);
        if (!cam) return false;
        memcpy(hr, cam + 0x10, 16 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* __fastcall hkToStage(void* holder, float* out, const float* vp) {
    if (!g_on.load(std::memory_order_relaxed) || !vp) return oToStage(holder, out, vp);
    {   // the game's own lens through the game's own holder, on the game's thread - at most every 100 ms
        const uint64_t now = GetTickCount64();
        if (now - g_calAt.load(std::memory_order_relaxed) > 100) { g_calAt.store(now, std::memory_order_relaxed); calibrate_with(holder); }
    }
    camera::Cam head{}, game{};
    bool haveHead = camera::latest(-1, head) && head.valid;
    if (g_holderCam.load(std::memory_order_relaxed)) {
        // MKHOLDER (V12): the camera-space point was made with [holder]'s own camera block (+0x10 rows, the ChrCam layout).
        // Our published head camera can be a frame apart from it - the marker then shakes with every head move.
        float hr[4][4];
        bool ok = read_holder_cam(holder, hr);
        if (ok) {
            const float n0 = dot3(hr[0], hr[0]), n1 = dot3(hr[1], hr[1]), n2 = dot3(hr[2], hr[2]);
            ok = fabsf(n0 - 1) < 0.02f && fabsf(n1 - 1) < 0.02f && fabsf(n2 - 1) < 0.02f && fabsf(dot3(hr[0], hr[2])) < 0.02f
                 && fabsf(hr[3][0]) < 1e5f && fabsf(hr[3][1]) < 1e5f && fabsf(hr[3][2]) < 1e5f;
        }
        if (ok) {
            if (haveHead) {
                const float g[3] = {hr[3][0] - head.r[3][0], hr[3][1] - head.r[3][1], hr[3][2] - head.r[3][2]};
                g_holderGap.store(sqrtf(dot3(g, g)), std::memory_order_relaxed);
            }
            memcpy(head.r, hr, sizeof(hr)); head.valid = true; haveHead = true;
            g_holderUsed.fetch_add(1, std::memory_order_relaxed);
        } else g_holderMiss.fetch_add(1, std::memory_order_relaxed);
    }
    const bool haveGame = camera::game_camera(game) && game.valid;
    if (!haveHead || !haveGame || vp[2] < 0.05f || vp[2] > 4000.0f) return oToStage(holder, out, vp);

    // 1. camera space -> world, with the camera that placed it (MKWORLD: three candidates, the steadiest wins)
    const uint32_t caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - 0x140000000ull);
    float Wc[3][3];
    {
        std::lock_guard<std::mutex> wl(g_worldMutex);
        uint32_t gen = 0; camera::Cam h{};
        if (camera::latest(-1, h, &gen) && gen != g_headGen) { g_headPrev = g_headNow.valid ? g_headNow : h; g_headNow = h; g_headGen = gen; }
        const camera::Cam* cs[3] = {&head, &game, g_headPrev.valid ? &g_headPrev : &head};
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 3; ++i)
                Wc[c][i] = cs[c]->r[3][i] + vp[0] * cs[c]->r[0][i] + vp[1] * cs[c]->r[1][i] + vp[2] * cs[c]->r[2][i];
        // the same marker a frame later: how far did each candidate's world point move?
        LARGE_INTEGER q, fq; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&fq);
        const uint64_t nowUs = static_cast<uint64_t>(static_cast<double>(q.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart));
        int best = -1; float bd = 0.5f;
        for (int k = 0; k < g_wtN; ++k) {
            if (g_wt[k].caller != caller) continue;
            const float dx = g_wt[k].w[1][0] - Wc[1][0], dy = g_wt[k].w[1][1] - Wc[1][1], dz = g_wt[k].w[1][2] - Wc[1][2];
            const float d = fabsf(dx) + fabsf(dy) + fabsf(dz);
            if (d < bd) { bd = d; best = k; }
        }
        if (best >= 0 && nowUs - g_wt[best].at > 4000 && nowUs - g_wt[best].at < 100000) {
            for (int c = 0; c < 3; ++c) {
                float d2 = 0;
                for (int i = 0; i < 3; ++i) { const float d = Wc[c][i] - g_wt[best].w[c][i]; d2 += d * d; }
                g_worldMove[c] += sqrt(static_cast<double>(d2));
            }
            ++g_worldN;
        }
        if (best < 0 || nowUs - g_wt[best].at > 4000) {
            if (best < 0) best = g_wtN < 32 ? g_wtN++ : static_cast<int>(nowUs % 32);
            g_wt[best].caller = caller; g_wt[best].at = nowUs; memcpy(g_wt[best].w, Wc, sizeof(Wc));
        }
        const uint64_t ms = GetTickCount64();
        if (ms - g_worldAt > 5000 && g_worldN >= 60) {
            int pick = 0;
            for (int c = 1; c < 3; ++c) if (g_worldMove[c] < g_worldMove[pick]) pick = c;
            if (g_world.load(std::memory_order_relaxed) == 3) g_worldPick.store(pick);
            BVR_LOG("markers: world point steadiness over %u frame pairs - head camera %.3f m, head-free camera %.3f m, head "
                    "camera a frame back %.3f m (mean move) -> using %s", g_worldN, g_worldMove[0] / g_worldN, g_worldMove[1] / g_worldN,
                    g_worldMove[2] / g_worldN, (g_world.load() == 3 ? pick : g_world.load()) == 0 ? "head camera" :
                    (g_world.load() == 3 ? pick : g_world.load()) == 1 ? "head-free camera" : "head camera a frame back");
            g_worldMove[0] = g_worldMove[1] = g_worldMove[2] = 0; g_worldN = 0; g_worldAt = ms;
        }
    }
    const int wsel = g_world.load(std::memory_order_relaxed) == 3 ? g_worldPick.load(std::memory_order_relaxed)
                                                                  : (g_world.load(std::memory_order_relaxed) % 3);
    float W[3] = {Wc[wsel][0], Wc[wsel][1], Wc[wsel][2]};
    smooth_world(caller, W);

    // 2. the head-free camera's version of the same world point
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = W[i] - game.r[3][i];
    const float gz = dot3(d, game.r[2]);
    alignas(16) float base[4] = {dot3(d, game.r[0]), dot3(d, game.r[1]), gz, vp[3]};
    const bool useBase = g_base.load(std::memory_order_relaxed) != 0 && gz > 0.05f;
    void* r = oToStage(holder, out, useBase ? base : vp);

    // 3. remember it for this frame's HUD pieces
    if (r) {
        const float* s = static_cast<const float*>(r);
        if (s[0] > -8000.0f && s[0] < 8000.0f && s[1] > -8000.0f && s[1] < 8000.0f) {
            std::lock_guard<std::mutex> lk(g_ptMutex);
            if (g_nPts[g_cur] < kMaxPoints) {
                Point& p = g_pts[g_cur][g_nPts[g_cur]++];
                for (int i = 0; i < 3; ++i) p.w[i] = W[i];
                p.stage[0] = s[0]; p.stage[1] = s[1];
                p.z = useBase ? gz : vp[2];
                p.caller = caller;
                p.ok = true;
                {
                    std::lock_guard<std::mutex> fl(g_fitMutex);
                    p.ndc[0] = g_fit.ax * s[0] + g_fit.bx;
                    p.ndc[1] = g_fit.ay * s[1] + g_fit.by;
                }
                g_pointsSeen.fetch_add(1, std::memory_order_relaxed);
                {   // MKTRACK (under g_ptMutex)
                    const uint64_t now = GetTickCount64();
                    int best = -1, oldest = 0; float bd = 0.3f * 0.3f;
                    for (int t = 0; t < 16; ++t) {
                        if (!g_trk[t].used || now - g_trk[t].at > 120) { if (!g_trk[t].used || g_trk[t].at < g_trk[oldest].at) oldest = t; continue; }
                        if (g_trk[t].p.caller != p.caller) continue;
                        const float dx = g_trk[t].p.w[0] - p.w[0], dy = g_trk[t].p.w[1] - p.w[1], dz = g_trk[t].p.w[2] - p.w[2];
                        const float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < bd) { bd = d2; best = t; }
                    }
                    if (best < 0) {   // a free / expired slot, else the oldest
                        best = oldest;
                        for (int t = 0; t < 16; ++t) if (!g_trk[t].used || now - g_trk[t].at > 120) { best = t; break; }
                    }
                    g_trk[best].p = p; g_trk[best].at = now; g_trk[best].used = true;
                }
            }
        }
    }
    return r;
}

// Ask the game's own projection about three known camera-space points, so the camera space -> stage map is measured
// rather than assumed (it carries the engine's FOV law and whatever aspect the HUD stage uses).
void calibrate_with(void* holder) {
    if (!oToStage || !holder) return;
    alignas(16) float a[4] = {0, 0, 1, 1}, b[4] = {0.5f, 0, 1, 1}, c[4] = {0, 0.5f, 1, 1};
    alignas(16) float o0[4] = {}, o1[4] = {}, o2[4] = {};
    const float* r0 = static_cast<const float*>(oToStage(holder, o0, a));
    const float* r1 = static_cast<const float*>(oToStage(holder, o1, b));
    const float* r2 = static_cast<const float*>(oToStage(holder, o2, c));
    if (!r0 || !r1 || !r2) return;
    const float dx = (r1[0] - r0[0]) / 0.5f, dy = (r2[1] - r0[1]) / 0.5f;
    if (fabsf(dx) < 1.0f || fabsf(dy) < 1.0f) return;
    Cal n{r0[0], r0[1], dx, dy, true};
    if (!g_cal.ok || fabsf(n.sx - g_cal.sx) > 0.5f || fabsf(n.sy - g_cal.sy) > 0.5f)
        BVR_LOG("markers: the game's own HUD projection measured - centre (%.1f, %.1f) stage units per unit tangent "
                "(%.1f, %.1f)", n.cx, n.cy, n.sx, n.sy);
    g_cal = n;
}

// ---------------------------------------------------------------------------------------------------------------
// A geometry shader generated from each HUD vertex shader's output signature (guide §5b step 4). It passes every
// output through untouched and replaces only SV_Position: the HUD's own 2D position becomes this eye's clip position
// of the matching point on the card. The perspective is then exact per vertex, which an affine cb0 patch can never be
// (Scaleform writes w = 1).
// ---------------------------------------------------------------------------------------------------------------
struct SigEl { std::string name; unsigned index, reg, mask, type, sysValue; };

bool parse_osgn(const void* code, size_t len, std::vector<SigEl>& out) {
    const uint8_t* p = static_cast<const uint8_t*>(code);
    if (len < 32 || memcmp(p, "DXBC", 4) != 0) return false;
    const uint32_t chunks = *reinterpret_cast<const uint32_t*>(p + 28);
    if (32 + 4ull * chunks > len) return false;
    for (uint32_t i = 0; i < chunks; ++i) {
        const uint32_t off = *reinterpret_cast<const uint32_t*>(p + 32 + 4 * i);
        if (off + 8 > len) continue;
        const char* fourcc = reinterpret_cast<const char*>(p + off);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(p + off + 4);
        const bool osgn = memcmp(fourcc, "OSGN", 4) == 0, osg1 = memcmp(fourcc, "OSG1", 4) == 0,
                   osg5 = memcmp(fourcc, "OSG5", 4) == 0;
        if (!osgn && !osg1 && !osg5) continue;
        const uint8_t* d = p + off + 8;
        if (off + 8 + size > len || size < 8) return false;
        const uint32_t n = *reinterpret_cast<const uint32_t*>(d);
        const uint32_t stride = osgn ? 24u : 32u;   // OSG1 / OSG5 carry a stream index first
        const uint32_t skip = osgn ? 0u : 4u;
        for (uint32_t k = 0; k < n; ++k) {
            const uint8_t* e = d + 8 + static_cast<size_t>(k) * stride + skip;
            if (e + 24 > p + off + 8 + size) return false;
            SigEl s;
            const uint32_t nameOff = *reinterpret_cast<const uint32_t*>(e);
            if (nameOff >= size) return false;
            s.name = reinterpret_cast<const char*>(d + nameOff);
            s.index = *reinterpret_cast<const uint32_t*>(e + 4);
            s.sysValue = *reinterpret_cast<const uint32_t*>(e + 8);
            s.type = *reinterpret_cast<const uint32_t*>(e + 12);
            s.reg = *reinterpret_cast<const uint32_t*>(e + 16);
            s.mask = e[20];
            out.push_back(s);
        }
        return !out.empty();
    }
    return false;
}

const char* type_name(unsigned t, unsigned mask) {
    int n = 0;
    for (int i = 0; i < 4; ++i) if (mask & (1 << i)) ++n;
    static const char* f[5] = {"float", "float", "float2", "float3", "float4"};
    static const char* u[5] = {"uint", "uint", "uint2", "uint3", "uint4"};
    static const char* s[5] = {"int", "int", "int2", "int3", "int4"};
    if (n < 1 || n > 4) n = 4;
    return t == 1 ? u[n] : t == 2 ? s[n] : f[n];
}

// Build the GS source for one signature. Returns "" when the signature is not one we can mirror (two outputs packed in
// one register, or an unwritten one) - the piece then keeps the old path rather than being drawn wrongly.
std::string gs_source(const std::vector<SigEl>& sig, int variant) {
    std::string decl = "struct V {\n";
    int posIdx = -1;
    unsigned used = 0;
    for (size_t i = 0; i < sig.size(); ++i) {
        const SigEl& e = sig[i];
        if (!e.mask) continue;
        if (used & (1u << e.reg)) return "";          // two outputs sharing a register: not mirrored
        used |= 1u << e.reg;
        const bool isPos = _stricmp(e.name.c_str(), "SV_Position") == 0 || _stricmp(e.name.c_str(), "SV_POSITION") == 0 ||
                           e.sysValue == 1;
        char line[160];
        snprintf(line, sizeof(line), "  %s v%zu : %s%u;\n", isPos ? "float4" : type_name(e.type, e.mask), i,
                 isPos ? "SV_Position" : e.name.c_str(), isPos ? 0u : e.index);
        decl += line;
        if (isPos) posIdx = static_cast<int>(i);
    }
    decl += "};\n";
    if (posIdx < 0) return "";
    char body[1600];
    if (variant == 1) {
        // MKREGION flat side (V12g): the HUD draw goes to the sheet as the game drew it, minus every triangle that lies in a
        // marker's region (those are on the marker's card)
        snprintf(body, sizeof(body),
                 "cbuffer MF : register(b13) { float4 R[8]; float4 N; }\n"
                 "[maxvertexcount(3)]\n"
                 "void main(triangle V i[3], inout TriangleStream<V> s) {\n"
                 "  float2 c = 0;\n"
                 "  [unroll] for (int k = 0; k < 3; ++k) c += (i[k].v%d.xy / (abs(i[k].v%d.w) > 1e-6 ? i[k].v%d.w : 1.0)) / 3.0;\n"
                 "  bool drop = false;\n"
                 "  [unroll] for (int m = 0; m < 8; ++m)\n"
                 "    if (m < (int)N.x && abs(c.x - R[m].x) <= R[m].z && abs(c.y - R[m].y) <= R[m].w) drop = true;\n"
                 "  if (!drop) { s.Append(i[0]); s.Append(i[1]); s.Append(i[2]); }\n"
                 "  s.RestartStrip();\n"
                 "}\n",
                 posIdx, posIdx, posIdx);
        return decl + body;
    }
    snprintf(body, sizeof(body),
             "cbuffer MK : register(b13) { float4 K0; float4 Kx; float4 Ky; float4 P; float4 R[8]; float4 N; }\n"
             "[maxvertexcount(3)]\n"
             "void main(triangle V i[3], inout TriangleStream<V> s) {\n"
             "  V o[3]; bool ok = true; float2 c = 0;\n"
             "  [unroll] for (int k = 0; k < 3; ++k) {\n"
             "    o[k] = i[k];\n"
             "    float2 p = i[k].v%d.xy / (abs(i[k].v%d.w) > 1e-6 ? i[k].v%d.w : 1.0);\n"
             "    c += p / 3.0;\n"
             "    float4 q = K0 + p.x * Kx + p.y * Ky;\n"
             // V12c: never a streak. (An early `return` here failed to compile - X4580, every card shader lost, JJ 15:09.)
             "    ok = ok && q.w > 0.05 && abs(q.x) < 3.0 * q.w && abs(q.y) < 3.0 * q.w;\n"
             "    o[k].v%d = float4(q.x, q.y, 0.5 * q.w, q.w);\n"
             "  }\n"
             // MKREGION (V12g): with P.z > 0 only the triangles in this marker's region (the HUD draw comes whole)
             "  if (P.z > 0 && (abs(c.x - P.x) > P.z || abs(c.y - P.y) > P.w)) ok = false;\n"
             // V12l (JJ: the lock-on glow brightens / dims): an enemy's lock-on point and bar point are close, so a piece
             // sat in both regions and was drawn on both cards - each triangle now goes only to its NEAREST marker
             "  float dOwn = dot(c - P.xy, c - P.xy);\n"
             "  [unroll] for (int m = 0; m < 8; ++m)\n"
             "    if (m < (int)N.x && m != (int)N.y && dot(c - R[m].xy, c - R[m].xy) < dOwn) ok = false;\n"
             "  if (ok) { s.Append(o[0]); s.Append(o[1]); s.Append(o[2]); }\n"
             "  s.RestartStrip();\n"
             "}\n",
             posIdx, posIdx, posIdx, posIdx);
    return decl + body;
}

// vertex shader -> its generated geometry shader. Unbounded map (AKN MKGS's first try used a fixed 2048-slot table and
// the HUD's shaders came last, so the fix never ran and looked like a failed method - guide §5a).
struct Gen { ID3D11GeometryShader* gs[2] = {}; bool tried[2] = {}; };   // [0] the card, [1] MKREGION's flat side
std::map<const void*, Gen> g_gen;
std::mutex g_genMutex;

ID3D11GeometryShader* gs_for(ID3D11Device* dev, ID3D11VertexShader* vs, int variant) {
    if (!dev || !vs) return nullptr;
    std::lock_guard<std::mutex> lk(g_genMutex);
    Gen& gen = g_gen[vs];
    if (gen.tried[variant]) return gen.gs[variant];
    gen.tried[variant] = true;
    struct One { ID3D11GeometryShader* gs = nullptr; } g;
    const void* code = nullptr;
    size_t len = 0;
    if (!shaderid::vs_code(vs, &code, &len) || !code || !len) { g_noCode.fetch_add(1); return nullptr; }
    std::vector<SigEl> sig;
    if (!parse_osgn(code, len, sig)) { g_gsFailed.fetch_add(1); snprintf(g_lastErr, sizeof(g_lastErr), "output signature not readable"); return nullptr; }
    const std::string src = gs_source(sig, variant);
    if (src.empty()) { g_gsFailed.fetch_add(1); snprintf(g_lastErr, sizeof(g_lastErr), "output signature not mirrorable"); return nullptr; }
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    static CompileFn compile = nullptr;
    if (!compile) {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        compile = m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }
    if (!compile) { g_gsFailed.fetch_add(1); snprintf(g_lastErr, sizeof(g_lastErr), "no shader compiler"); return nullptr; }
    ID3DBlob* bin = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(compile(src.c_str(), src.size(), "mkgs", nullptr, nullptr, "main", "gs_5_0", 0, 0, &bin, &err)) || !bin) {
        snprintf(g_lastErr, sizeof(g_lastErr), "%s", err ? static_cast<const char*>(err->GetBufferPointer()) : "compile failed");
        BVR_LOG("markers: the card geometry shader did not compile: %s\n%s", g_lastErr, src.c_str());
        if (err) err->Release();
        g_gsFailed.fetch_add(1);
        return nullptr;
    }
    if (err) err->Release();
    const HRESULT hr = dev->CreateGeometryShader(bin->GetBufferPointer(), bin->GetBufferSize(), nullptr, &g.gs);
    bin->Release();
    if (FAILED(hr) || !g.gs) { g.gs = nullptr; g_gsFailed.fetch_add(1); snprintf(g_lastErr, sizeof(g_lastErr), "CreateGeometryShader 0x%08x", static_cast<unsigned>(hr)); return nullptr; }
    g_gsBuilt.fetch_add(1);
    BVR_LOG("markers: %s geometry shader %u built for HUD vertex shader %p (%zu outputs)", variant ? "flat-side" : "card",
            g_gsBuilt.load(), (void*)vs, sig.size());
    gen.gs[variant] = g.gs;
    return g.gs;
}

// ---- per-eye layers and the card constants ---------------------------------------------------------------------
struct Layer {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* ds = nullptr;          // MKSTENCIL
    ID3D11DepthStencilView* dsv = nullptr;
    unsigned w = 0, h = 0;
} g_layer[2];
constexpr int kKRing = 24;
ID3D11Buffer* g_kcb[kKRing] = {};
constexpr int kFRing = 64;
ID3D11Buffer* g_fcb[kFRing] = {};
int g_fNext = 0;
int g_kNext = 0;
ID3D11BlendState* g_blend = nullptr;
ID3D11Device* g_dev = nullptr;
std::atomic<bool> g_layersOk{false};
std::atomic<uint32_t> g_frameNo{0}, g_clearedFrame{0xFFFFFFFFu}, g_pieceFrame{0xFFFFFFFFu};

// This frame's eye data: the full world -> clip matrix (row-vector) and the head-free camera's frame.
struct EyeFrame { float vp[16]; bool ok; };
EyeFrame g_eye[2];
struct GameFrame { float pos[3], right[3], up[3], fwd[3]; float hx, hy; bool ok; } g_game;
std::mutex g_frameMutex;

std::mutex g_layerMutex;
bool ensure_layers(ID3D11Device* dev, unsigned w, unsigned h) {
    if (!dev || !w || !h) return false;
    std::lock_guard<std::mutex> lk(g_layerMutex);
    for (int e = 0; e < 2; ++e) {
        Layer& L = g_layer[e];
        if (L.tex && L.w == w && L.h == h) continue;
        if (L.srv) { L.srv->Release(); L.srv = nullptr; }
        if (L.rtv) { L.rtv->Release(); L.rtv = nullptr; }
        if (L.tex) { L.tex->Release(); L.tex = nullptr; }
        if (L.dsv) { L.dsv->Release(); L.dsv = nullptr; }
        if (L.ds) { L.ds->Release(); L.ds = nullptr; }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &L.tex)) || !L.tex) return false;
        if (FAILED(dev->CreateRenderTargetView(L.tex, nullptr, &L.rtv)) ||
            FAILED(dev->CreateShaderResourceView(L.tex, nullptr, &L.srv))) return false;
        {   // MKSTENCIL: the layer's own depth-stencil (a failure only costs the clipping)
            D3D11_TEXTURE2D_DESC z = d;
            z.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; z.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            if (SUCCEEDED(dev->CreateTexture2D(&z, nullptr, &L.ds)) && L.ds) dev->CreateDepthStencilView(L.ds, nullptr, &L.dsv);
        }
        L.w = w; L.h = h;
        BVR_LOG("markers: eye %d marker layer %ux%u", e, w, h);
    }
    if (!g_kcb[0]) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 208; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        for (auto& b : g_kcb) if (FAILED(dev->CreateBuffer(&bd, nullptr, &b))) return false;
        bd.ByteWidth = 144;   // MKREGION flat side: 8 regions + count
        for (auto& b : g_fcb) if (FAILED(dev->CreateBuffer(&bd, nullptr, &b))) return false;
    }
    if (!g_blend) {
        // "over" that accumulates coverage: colour src*a + dst*(1-a), alpha a + dst*(1-a). The layer then holds
        // premultiplied colour and real coverage, which blit::alpha_premul composites correctly (AKN HUDINEYE).
        D3D11_BLEND_DESC b{};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&b, &g_blend))) return false;
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------------------------------------------------
bool install() {
    if (g_installed.load()) return true;
    g_on.store(settings::i("markers", 1));
    g_base.store(settings::i("marker_base", 1));
    g_facePos.store(settings::i("marker_face_pos", 1));
    g_reach.store(settings::f("marker_reach", 0.12f));
    g_smooth.store(settings::f("marker_smooth", 1.0f));
    g_holderCam.store(settings::i("marker_holder", 1));
    g_world.store(settings::i("marker_world", 3));
    g_vpNow.store(settings::i("marker_vp_now", 1));
    g_region.store(settings::i("marker_region", 1));
    g_regionX.store(settings::f("marker_region_x", 0.12f));
    g_regionY.store(settings::f("marker_region_y", 0.08f));
    g_stencil.store(settings::i("marker_stencil", 1));
    g_maskLayer.store(settings::i("marker_masks", 1));
    g_follow.store(settings::i("marker_follow", 0));
    g_followReach.store(settings::f("marker_follow_reach", 0.40f));
    // The byte check keeps this module out of the game's code on any other build - it must never hook a guessed address.
    uint8_t* at = addr::ptr<uint8_t>(kToStage);
    bool match = false;
    __try { match = memcmp(at, kToStageBytes, sizeof(kToStageBytes)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!match) {
        BVR_LOG("markers: off - the marker projection's bytes differ on this build (0x1406C73B0); nothing hooked");
        return false;
    }
    if (MH_CreateHook(at, reinterpret_cast<void*>(&hkToStage), reinterpret_cast<void**>(&oToStage)) != MH_OK ||
        MH_QueueEnableHook(at) != MH_OK) {
        BVR_LOG("markers: the marker projection hook FAILED");
        return false;
    }
    g_installed.store(true);
    BVR_LOG("markers: marker projection hooked at %p (world markers %s)", at, g_on.load() ? "on" : "off");
    return true;
}

bool installed() { return g_installed.load(); }

float g_vpOver[2][16] = {};
bool g_vpOverOn[2] = {};
void set_eye_vp(int eye, const float vp[16]) {
    if (eye < 0) { g_vpOverOn[0] = g_vpOverOn[1] = false; return; }
    if (eye > 1 || !vp) return;
    memcpy(g_vpOver[eye], vp, sizeof(g_vpOver[eye]));
    g_vpOverOn[eye] = true;
}

// FILLCATCH (V12q, JJ: the red fill jumps to the 2D sheet now and then - too rare for a capture): at each Present the
// sheet around the first marker point is copied into a small staging ring; the copy from two Presents ago is read back
// without waiting, and red pixels there (the fill drawn flat) are logged with that frame's marker counters.
struct CatchSlot { ID3D11Texture2D* tex = nullptr; bool full = false; uint32_t frame = 0, cards = 0, regionDraws = 0, held = 0; int pts = 0; };
CatchSlot g_catch[4];
int g_catchNext = 0;
std::atomic<int> g_catchOn{0}, g_catchLogs{40};   // V12z: off by default (every-frame readback; diagnosis done) - `ds mkcatch 1`
std::atomic<uint32_t> g_catchHits{0}, g_catchChecked{0};
uint32_t g_catchCardsF = 0, g_catchRegionF = 0;
void fill_catch(ID3D11Device* dev, ID3D11DeviceContext* imm) {
    if (!g_catchOn.load(std::memory_order_relaxed) || !dev || !imm) return;
    ID3D11Texture2D* sheet = picsplit::hud_sheet_ensure(dev);
    if (!sheet) return;
    D3D11_TEXTURE2D_DESC sd{}; sheet->GetDesc(&sd);
    if (sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM && sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return;
    constexpr UINT kW = 256, kH = 256;   // bar AND lock-on dot (they sit ~70 px apart on the sheet)
    // 1. read back the slot written two Presents ago (DO_NOT_WAIT: never stalls the frame)
    CatchSlot& old = g_catch[(g_catchNext + 1) % 4];   // V12u: three Presents old, read every frame (done by then)
    if (old.full && old.tex) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(imm->Map(old.tex, 0, D3D11_MAP_READ, 0, &m))) {
            unsigned red = 0, white = 0;   // V12s: white too - the lock-on dot / glow drawn flat (JJ: the glow flash is 2D)
            for (UINT y = 0; y < kH; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
                for (UINT x = 0; x < kW; ++x) {
                    if (row[4 * x] > 120 && row[4 * x + 1] < 60 && row[4 * x + 2] < 60) ++red;
                    if (row[4 * x] > 170 && row[4 * x + 1] > 170 && row[4 * x + 2] > 170) ++white;
                }
            }
            imm->Unmap(old.tex, 0);
            old.full = false;
            g_catchChecked.fetch_add(1, std::memory_order_relaxed);
            if (red > 20 || white > 30) {
                g_catchHits.fetch_add(1, std::memory_order_relaxed);
                if (g_catchLogs.load(std::memory_order_relaxed) > 0 && g_catchLogs.fetch_sub(1) > 0)
                    BVR_LOG("fillcatch: frame %u - %u red / %u white pixels on the SHEET around the marker | that frame: cards %u, region "
                            "draws %u, held-point draws %u, points %d", old.frame, red, white, old.cards, old.regionDraws, old.held, old.pts);
            }
        }
    }
    // 2. this Present: copy the box around the first complete point (if any) into the next slot
    Point p{}; bool have = false; int npts = 0;
    {   // V12u: the live tracks (a frame with a missing bucket point is exactly the frame to catch)
        std::lock_guard<std::mutex> lk(g_ptMutex);
        const uint64_t tnow = GetTickCount64();
        npts = g_nPts[g_cur ^ 1];
        for (int t = 0; t < 16 && !have; ++t)
            if (g_trk[t].used && tnow - g_trk[t].at <= 120 && fabsf(g_trk[t].p.ndc[0]) < 1.0f && fabsf(g_trk[t].p.ndc[1]) < 1.0f) {
                p = g_trk[t].p; have = true;
            }
    }
    CatchSlot& cur = g_catch[g_catchNext];
    g_catchNext = (g_catchNext + 1) % 4;
    if (!have) return;
    if (!cur.tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kW; d.Height = kH; d.MipLevels = 1; d.ArraySize = 1; d.Format = sd.Format; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &cur.tex))) { cur.tex = nullptr; return; }
    }
    const int cx = static_cast<int>((p.ndc[0] + 1.0f) * 0.5f * sd.Width), cy = static_cast<int>((1.0f - p.ndc[1]) * 0.5f * sd.Height);
    D3D11_BOX box{};
    box.left = static_cast<UINT>(cx - static_cast<int>(kW / 2) < 0 ? 0 : cx - static_cast<int>(kW / 2));
    box.top = static_cast<UINT>(cy - static_cast<int>(kH / 2) < 0 ? 0 : cy - static_cast<int>(kH / 2));
    if (box.left + kW > sd.Width) box.left = sd.Width - kW;
    if (box.top + kH > sd.Height) box.top = sd.Height - kH;
    box.right = box.left + kW; box.bottom = box.top + kH; box.front = 0; box.back = 1;
    imm->CopySubresourceRegion(cur.tex, 0, 0, 0, 0, sheet, 0, &box);
    cur.full = true; cur.frame = g_frameNo.load(); cur.cards = g_catchCardsF; cur.regionDraws = g_catchRegionF;
    cur.held = g_regionHeld.load(); cur.pts = npts;
}

// GLOWMETER (V12r, JJ: the lock-on glow brightens about once a second in VR, never in the flat game; every frame draws the
// same 46 pieces): the left marker layer around each marker point, read back two Presents late, mean colour and alpha per
// point - logged per Present while `ds mkframes` runs, so a pulse shows as numbers.
struct GlowSlot { ID3D11Texture2D* tex[2] = {}; bool full[2] = {}; uint32_t frame = 0; };
GlowSlot g_glow[3];
int g_glowNext = 0;
void glow_meter(ID3D11Device* dev, ID3D11DeviceContext* imm) {
    if (g_framesLog.load(std::memory_order_relaxed) <= 0 || !dev || !imm || !g_layer[0].tex) return;
    constexpr UINT kS = 256;   // big enough for a larger copy of the dot (JJ: "maybe a larger version once a second")
    GlowSlot& old = g_glow[(g_glowNext + 1) % 3];
    char line[300]; int n = snprintf(line, sizeof(line), "glowmeter: frame %u", old.frame);
    bool any = false;
    for (int k = 0; k < 2; ++k) {
        if (!old.full[k] || !old.tex[k]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(imm->Map(old.tex[k], 0, D3D11_MAP_READ, 0, &m))) continue;
        double rgb = 0, a = 0; unsigned bright = 0, covered = 0;
        for (UINT y = 0; y < kS; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
            for (UINT x = 0; x < kS; ++x) {
                rgb += row[4 * x] + row[4 * x + 1] + row[4 * x + 2]; a += row[4 * x + 3];
                if (row[4 * x] + row[4 * x + 1] + row[4 * x + 2] > 600) ++bright;
                if (row[4 * x + 3] > 8) ++covered;
            }
        }
        imm->Unmap(old.tex[k], 0);
        old.full[k] = false;
        n += snprintf(line + n, sizeof(line) - n, " | point %d colour %.2f alpha %.2f bright px %u covered px %u", k,
                      rgb / (kS * kS * 3.0), a / (kS * kS), bright, covered);
        any = true;
    }
    if (any) BVR_LOG("%s", line);
    // this Present: the boxes around this frame's points in the left layer
    GlowSlot& cur = g_glow[g_glowNext];
    g_glowNext = (g_glowNext + 1) % 3;
    cur.frame = g_frameNo.load();
    Point pts[2]; int np = 0;
    {
        std::lock_guard<std::mutex> lk(g_ptMutex);
        const int b = g_nPts[g_cur ^ 1] ? (g_cur ^ 1) : g_cur;
        for (int i = 0; i < g_nPts[b] && np < 2; ++i) pts[np++] = g_pts[b][i];
    }
    float M[16];
    if (!picsplit::view_proj(0, M, nullptr)) return;
    D3D11_TEXTURE2D_DESC ld{}; g_layer[0].tex->GetDesc(&ld);
    for (int k = 0; k < np; ++k) {
        float c[4];
        for (int j = 0; j < 4; ++j) c[j] = pts[k].w[0] * M[j] + pts[k].w[1] * M[4 + j] + pts[k].w[2] * M[8 + j] + M[12 + j];
        if (c[3] <= 0.05f) continue;
        const int cx = static_cast<int>((c[0] / c[3] + 1.0f) * 0.5f * ld.Width), cy = static_cast<int>((1.0f - c[1] / c[3]) * 0.5f * ld.Height);
        if (cx < static_cast<int>(kS) || cy < static_cast<int>(kS) || cx + static_cast<int>(kS) > static_cast<int>(ld.Width) ||
            cy + static_cast<int>(kS) > static_cast<int>(ld.Height)) continue;
        if (!cur.tex[k]) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = kS; d.Height = kS; d.MipLevels = 1; d.ArraySize = 1; d.Format = ld.Format; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &cur.tex[k]))) { cur.tex[k] = nullptr; continue; }
        }
        D3D11_BOX box{static_cast<UINT>(cx - kS / 2), static_cast<UINT>(cy - kS / 2), 0, static_cast<UINT>(cx + kS / 2), static_cast<UINT>(cy + kS / 2), 1};
        imm->CopySubresourceRegion(cur.tex[k], 0, 0, 0, 0, g_layer[0].tex, 0, &box);
        cur.full[k] = true;
    }
}

void on_present(ID3D11Device* dev, ID3D11DeviceContext* imm, unsigned eyeW, unsigned eyeH) {
    if (!g_installed.load()) return;
    g_dev = dev;
    glow_meter(dev, imm);
    g_catchCardsF = g_catchCardsAcc; g_catchRegionF = g_catchRegionAcc;   // the frame just drawn
    fill_catch(dev, imm);
    g_catchCardsAcc = 0; g_catchRegionAcc = 0;
    if (g_framesLog.load(std::memory_order_relaxed) > 0) {
        // MKFRAMES (V12n, JJ: bar + lock-on flicker several times a second, every 1-per-second capture complete): per
        // Present - card draws this frame, region HUD draws, points in the complete / filling set, whether the eye
        // composite would lay the layer in (any_this_frame), and whether this frame's layer was cleared
        g_framesLog.fetch_sub(1);
        {   // DRAWDIFF
            const auto same = [](const DrawRec& a, const DrawRec& b) {
                return a.vs == b.vs && a.count == b.count && a.start == b.start && a.base == b.base && a.blend == b.blend &&
                       a.dss == b.dss && a.ref == b.ref && a.mask == b.mask && a.cbHash == b.cbHash;
            };
            if (!g_drawsPrev.empty() && !g_drawsNow.empty()) {
                for (int pass = 0; pass < 2; ++pass) {
                    const auto& A = pass == 0 ? g_drawsNow : g_drawsPrev;
                    const auto& B = pass == 0 ? g_drawsPrev : g_drawsNow;
                    int logged = 0;
                    for (const auto& a : A) {
                        bool found = false;
                        for (const auto& b : B) if (same(a, b)) { found = true; break; }
                        if (!found && logged < 12) {
                            ++logged;
                            BVR_LOG("drawdiff: frame %u %s vs %016llx count %u start %u base %d blend %07X dss %08X ref %u %s | cb "
                                    "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                                    g_frameNo.load(), pass == 0 ? "NEW " : "GONE", static_cast<unsigned long long>(a.vs), a.count,
                                    a.start, a.base, a.blend, a.dss, a.ref, a.mask ? "MASK" : "colour", a.cb[0], a.cb[1], a.cb[2],
                                    a.cb[3], a.cb[4], a.cb[5], a.cb[6], a.cb[7], a.cb[8], a.cb[9], a.cb[10], a.cb[11]);
                        }
                    }
                }
            }
            g_drawsPrev.swap(g_drawsNow);
            g_drawsNow.clear();
        }
        BVR_LOG("mkframes: frame %u cards %u regionDraws %u points %d/%d layerIn %d cleared %d", g_frameNo.load(),
                g_cardsF.exchange(0), g_regionF.exchange(0), g_nPts[g_cur ^ 1], g_nPts[g_cur], any_this_frame() ? 1 : 0,
                g_clearedFrame.load() == g_frameNo.load() ? 1 : 0);
    }
    g_frameNo.fetch_add(1, std::memory_order_relaxed);
    // (the calibration now runs inside the projection hook, on the game's thread)

    // this frame's per-eye world -> clip, and the head-free camera's frame (the card's reference, guide §4)
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        for (int e = 0; e < 2; ++e) {
            if (g_vpOverOn[e]) { memcpy(g_eye[e].vp, g_vpOver[e], sizeof(g_eye[e].vp)); g_eye[e].ok = true; }   // AFW
            else g_eye[e].ok = picsplit::view_proj(e, g_eye[e].vp, nullptr);
        }
        camera::Cam g{};
        g_game.ok = camera::game_camera(g) && g.valid;
        if (g_game.ok) {
            for (int i = 0; i < 3; ++i) {
                g_game.pos[i] = g.r[3][i]; g_game.right[i] = g.r[0][i]; g_game.up[i] = g.r[1][i]; g_game.fwd[i] = g.r[2][i];
            }
            // Scaleform ShowAll: the 1920x1080 stage fits the sheet's WIDTH uniformly, so one stage unit is 1/960
            // of NDC across and the same times W/H down. Chained with the calibration (stage units per unit tangent)
            // that gives the HUD's own half-angles, which should land on the engine's own lens - logged as a check.
            std::lock_guard<std::mutex> fl(g_fitMutex);
            const float wh = (eyeW && eyeH) ? static_cast<float>(eyeW) / static_cast<float>(eyeH) : 1.0f;
            g_fit.ax = 1.0f / 960.0f;  g_fit.bx = -1.0f;
            g_fit.ay = -wh / 960.0f;   g_fit.by = 540.0f * wh / 960.0f;
            const float kx = g_fit.ax * g_cal.sx, ky = g_fit.ay * g_cal.sy;
            g_game.hx = fabsf(kx) > 1e-9f ? 1.0f / kx : 1.0f;
            g_game.hy = fabsf(ky) > 1e-9f ? 1.0f / ky : 1.0f;
        }
    }
    if (dev && imm) g_layersOk.store(ensure_layers(dev, eyeW, eyeH));
    {   // the points belong to one game frame only
        std::lock_guard<std::mutex> lk(g_ptMutex);
        if (g_probe.load() > 0 && g_nPts[g_cur] > 0) {
            g_probe.fetch_sub(1);
            uint32_t seen = 0, noCb = 0, taken = 0;
            picsplit::hud_stats(seen, noCb, taken);
            BVR_LOG("mkdump: %d marker points this frame; HUD draws seen %u (no placement %u), taken %u", g_nPts[g_cur],
                    seen, noCb, taken);
            for (int i = 0; i < g_nPts[g_cur]; ++i) {
                const Point& p = g_pts[g_cur][i];
                BVR_LOG("mkdump   point %2d  world %.2f %.2f %.2f  stage %.1f %.1f  -> hud %.3f %.3f  depth %.2f  from +0x%X",
                        i, p.w[0], p.w[1], p.w[2], p.stage[0], p.stage[1], p.ndc[0], p.ndc[1], p.z, p.caller);
            }
        }
        g_cur ^= 1;
        g_nPts[g_cur] = 0;
        // MKTEST (V11): a test marker point at a chosen HUD position, `dist` m in front of the head-free camera, so the
        // whole card path (rows, batches, the flat copy removed, size, AFW) can be checked on fixed HUD pieces such as the
        // player's own bars without an enemy. `ds mktest x y dist` (dist 0 = off).
        const float tz = g_testPt[2].load(std::memory_order_relaxed);
        if (tz > 0.1f && g_game.ok && g_nPts[g_cur] < kMaxPoints) {
            Point& p = g_pts[g_cur][g_nPts[g_cur]++];
            p = Point{};
            p.ndc[0] = g_testPt[0].load(); p.ndc[1] = g_testPt[1].load();
            const float tx = p.ndc[0] * g_game.hx * tz, ty = p.ndc[1] * g_game.hy * tz;
            for (int i = 0; i < 3; ++i) p.w[i] = g_game.pos[i] + tz * g_game.fwd[i] + tx * g_game.right[i] + ty * g_game.up[i];
            p.z = tz;
            p.caller = 0x7E57;
            p.ok = true;
        }
    }
    static uint32_t lastLog = 0;
    if (g_frameNo.load() - lastLog > 450) {
        lastLog = g_frameNo.load();
        std::lock_guard<std::mutex> fl(g_fitMutex);
        BVR_LOG("markers: %s", status());
        BVR_LOG("markers: stage -> HUD map x = %.6f * sx %+0.4f, y = %.6f * sy %+0.4f -> the HUD's own half-angles "
                "%.4f x %.4f (the engine's own lens measured 1.1504 x 1.2462); matched pieces sit %.4f NDC from their "
                "marker on average (%ld of them)", g_fit.ax, g_fit.bx, g_fit.ay, g_fit.by, g_game.hx, g_game.hy,
                g_fit.residN ? g_fit.resid / g_fit.residN : 0.0, g_fit.residN);
    }
}

bool any_this_frame() {
    const uint32_t now = g_frameNo.load(std::memory_order_relaxed), piece = g_pieceFrame.load(std::memory_order_relaxed);
    // V12n: up to 4 frames - the game does not draw its marker HUD every frame; with "<= 1" a 2-frame gap left the eye
    // composite without the layer for a frame = the bar and lock-on flickering (the layer keeps its cards until new ones)
    return piece != 0xFFFFFFFFu && now - piece <= static_cast<uint32_t>(g_layerHold.load(std::memory_order_relaxed));
}
ID3D11ShaderResourceView* layer(int eye) { return eye == 0 || eye == 1 ? g_layer[eye].srv : nullptr; }
ID3D11Texture2D* layer_tex(int eye) { return eye == 0 || eye == 1 ? g_layer[eye].tex : nullptr; }

// EYEPROBE (V12v, JJ: the lock-on dot still flashes; the catcher saw it on the sheet 0 times in 3214 frames, the left card
// never pulses): the FINISHED headset image of each eye around the dot, read back three Presents late, logged per eye while
// `ds mkframes` runs - everything we submit. A flash here = ours (which eye, which frame); none = after our hand-off.
struct EyeSlot { ID3D11Texture2D* tex = nullptr; bool full = false; uint32_t frame = 0; };
EyeSlot g_eyeProbe[2][4];
int g_eyeNext[2] = {};
void probe_eye_image(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, int eye) {
    if (g_framesLog.load(std::memory_order_relaxed) <= 0 || !ctx || !dst || eye < 0 || eye > 1 || !g_dev) return;
    constexpr UINT kS = 256;
    EyeSlot& old = g_eyeProbe[eye][(g_eyeNext[eye] + 1) % 4];
    if (old.full && old.tex) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(old.tex, 0, D3D11_MAP_READ, 0, &m))) {
            double sum = 0; unsigned bright = 0, white = 0;
            for (UINT y = 0; y < kS; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
                for (UINT x = 0; x < kS; ++x) {
                    const int v = row[4 * x] + row[4 * x + 1] + row[4 * x + 2];
                    sum += v; if (v > 600) ++bright; if (v > 720) ++white;
                }
            }
            ctx->Unmap(old.tex, 0);
            BVR_LOG("eyeprobe: eye %d frame %u mean %.2f bright px %u white px %u", eye, old.frame, sum / (kS * kS * 3.0), bright, white);
        }
        old.full = false;
    }
    EyeSlot& cur = g_eyeProbe[eye][g_eyeNext[eye]];
    g_eyeNext[eye] = (g_eyeNext[eye] + 1) % 4;
    Point p{}; bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_ptMutex);
        const uint64_t tnow = GetTickCount64();
        // the lock-on dot: of the live tracks, the LOWER point on the HUD (the bar sits above it)
        for (int t = 0; t < 16; ++t)
            if (g_trk[t].used && tnow - g_trk[t].at <= 120 && fabsf(g_trk[t].p.ndc[0]) < 1.0f && fabsf(g_trk[t].p.ndc[1]) < 1.0f &&
                (!have || g_trk[t].p.ndc[1] < p.ndc[1])) { p = g_trk[t].p; have = true; }
    }
    float M[16];
    if (!have || !picsplit::view_proj(eye, M, nullptr)) return;
    D3D11_TEXTURE2D_DESC dd{}; dst->GetDesc(&dd);
    float c[4];
    for (int j = 0; j < 4; ++j) c[j] = p.w[0] * M[j] + p.w[1] * M[4 + j] + p.w[2] * M[8 + j] + M[12 + j];
    if (c[3] <= 0.05f) return;
    const int cx = static_cast<int>((c[0] / c[3] + 1.0f) * 0.5f * dd.Width), cy = static_cast<int>((1.0f - c[1] / c[3]) * 0.5f * dd.Height);
    if (cx < static_cast<int>(kS / 2) || cy < static_cast<int>(kS / 2) || cx + static_cast<int>(kS / 2) > static_cast<int>(dd.Width) ||
        cy + static_cast<int>(kS / 2) > static_cast<int>(dd.Height)) return;
    if (!cur.tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kS; d.Height = kS; d.MipLevels = 1; d.ArraySize = 1; d.Format = dd.Format; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &cur.tex))) { cur.tex = nullptr; return; }
    }
    D3D11_BOX box{static_cast<UINT>(cx - kS / 2), static_cast<UINT>(cy - kS / 2), 0, static_cast<UINT>(cx + kS / 2), static_cast<UINT>(cy + kS / 2), 1};
    ctx->CopySubresourceRegion(cur.tex, 0, 0, 0, 0, dst, 0, &box);
    cur.full = true; cur.frame = g_frameNo.load();
}

// ---------------------------------------------------------------------------------------------------------------
// POSROWS (V11, JJ: "the health bar seems to be duplicated ... a smaller version and a much larger version, in
// different locations"). DS3's HUD vertex shaders do NOT share one constant layout (shader dump 2026-10-30): the classic
// GFx one places the piece with cb0 rows 2 / 3 (75c93907), others with rows 0 / 1 (f10647ba - one of the geo-11 fix's
// four -, 6c9e5a56, bb9d9888), and the batched ones pick their rows per piece from the vertex data (aa4d1367,
// fa3d4612, f710b858: cb0[r0.x + 0]). The piece's origin was always read from rows 2 / 3, so for the others it was a
// texture offset: pieces went to the wrong marker point (moved, wrong size), the rest of the bar stayed flat. Each HUD
// vertex shader's own position rows are read from its code once: "dp4 o<pos>.x, v<n>.xyzw, cb0[<row>]". A batched
// shader (its rows come from the vertex data) gives -1: its pieces stay where the game drew them.
std::mutex g_rowsMutex;
std::map<const void*, int> g_rows;   // vertex shader -> the cb0 row of its position's x (y = row + 1), -1 = not knowable
std::atomic<uint32_t> g_rowsBatched{0}, g_rowsUnknown{0};
int pos_rows(ID3D11VertexShader* vs) {
    {
        std::lock_guard<std::mutex> lk(g_rowsMutex);
        auto it = g_rows.find(vs);
        if (it != g_rows.end()) return it->second;
    }
    int row = -1;
    const void* code = nullptr; size_t len = 0;
    std::string why = "no bytecode";
    if (shaderid::vs_code(vs, &code, &len) && code && len) {
        ID3DBlob* text = nullptr;
        if (SUCCEEDED(D3DDisassemble(code, len, 0, nullptr, &text)) && text) {
            const std::string t(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
            text->Release();
            // the position output register
            int posReg = -1;
            const size_t d = t.find(", position");
            if (d != std::string::npos) {
                const size_t o = t.rfind("dcl_output_siv o", d);
                if (o != std::string::npos) posReg = atoi(t.c_str() + o + 16);
            }
            if (posReg < 0) why = "no position output";
            else {
                char pat[32];
                snprintf(pat, sizeof(pat), "dp4 o%d.x, v", posReg);
                const size_t q = t.find(pat);
                if (q == std::string::npos) why = "position not a dp4 of the vertex";
                else {
                    const size_t c = t.find("cb0[", q);
                    const size_t eol = t.find('\n', q);
                    if (c == std::string::npos || c > eol) why = "position not from cb0";
                    else if (t[c + 4] == 'r') { why = "batched (rows from the vertex data)"; g_rowsBatched.fetch_add(1); }
                    else { row = atoi(t.c_str() + c + 4); why = "ok"; }
                }
            }
        }
    }
    if (row < 0 && why != "batched (rows from the vertex data)") g_rowsUnknown.fetch_add(1);
    BVR_LOG("markers: HUD vertex shader %p (%016llx) places its pieces with cb0 row %d (%s)", static_cast<void*>(vs),
            static_cast<unsigned long long>(shaderid::hash_of(vs)), row, why.c_str());
    std::lock_guard<std::mutex> lk(g_rowsMutex);
    g_rows[vs] = row;
    return row;
}

// The card at a marker's world point -> each eye's map from a HUD point (x, y) to clip space (guide §5b step 3).
bool card_K(const Point& pt, float K[2][12]) {
    // ---- the card at the world point (guide §5b step 2 + §5c) -----------------------------------------------------
    {
        std::lock_guard<std::mutex> lk(g_frameMutex);
        if (!g_game.ok) return false;
        float n[3], right[3], up[3];
        for (int i = 0; i < 3; ++i) n[i] = g_game.pos[i] - pt.w[i];
        float nl = sqrtf(dot3(n, n));
        if (nl < 0.01f) return false;
        for (int i = 0; i < 3; ++i) n[i] /= nl;
        if (!g_facePos.load(std::memory_order_relaxed)) {   // upright card facing the camera's HEADING
            n[0] = -g_game.fwd[0]; n[1] = 0; n[2] = -g_game.fwd[2];
            nl = sqrtf(dot3(n, n));
            if (nl < 0.1f) { n[0] = -g_game.fwd[0]; n[1] = -g_game.fwd[1]; n[2] = -g_game.fwd[2]; nl = 1.0f; }
            for (int i = 0; i < 3; ++i) n[i] /= nl;
        }
        // right edge LEVEL (world up is +y in Dark Souls III) and perpendicular to the normal; up = n x right, kept upward.
        right[0] = -n[2]; right[1] = 0; right[2] = n[0];
        float rl = sqrtf(dot3(right, right));
        if (rl < 0.05f) { right[0] = g_game.right[0]; right[1] = 0; right[2] = g_game.right[2]; rl = sqrtf(dot3(right, right)); }
        if (rl < 1e-5f) return false;
        for (int i = 0; i < 3; ++i) right[i] /= rl;
        if (dot3(right, g_game.right) < 0) for (int i = 0; i < 3; ++i) right[i] = -right[i];
        up[0] = n[1] * right[2] - n[2] * right[1];
        up[1] = n[2] * right[0] - n[0] * right[2];
        up[2] = n[0] * right[1] - n[1] * right[0];
        const float ul = sqrtf(dot3(up, up));
        if (ul < 1e-5f) return false;
        for (int i = 0; i < 3; ++i) up[i] /= ul;
        if (up[1] < 0) for (int i = 0; i < 3; ++i) up[i] = -up[i];
        // The card is exactly the size the game drew it: world units per HUD unit = depth x the HUD's own half-angle.
        const float sx = pt.z * g_game.hx, sy = pt.z * g_game.hy;
        for (int e = 0; e < 2; ++e) {
            if (!g_eye[e].ok) return false;
            const float* M = g_eye[e].vp;
            // MKVPNOW (V12e, JJ: bar + lock-on jitter with head movement while the world point is steady to 1 mm): g_eye is
            // taken at Present, but the HUD draws in the NEXT frame - the card was laid over the new picture with the
            // previous frame's eye camera. Read the eye's world -> clip as it is now, at the HUD draw.
            float Mnow[16];
            if (g_vpNow.load(std::memory_order_relaxed) && !g_vpOverOn[e] && picsplit::view_proj(e, Mnow, nullptr)) {
                M = Mnow;
                g_vpNowUsed.fetch_add(1, std::memory_order_relaxed);
            }
            float cW[4] = {}, cR[4] = {}, cU[4] = {};
            for (int j = 0; j < 4; ++j) {
                cW[j] = pt.w[0] * M[j] + pt.w[1] * M[4 + j] + pt.w[2] * M[8 + j] + M[12 + j];
                cR[j] = sx * (right[0] * M[j] + right[1] * M[4 + j] + right[2] * M[8 + j]);
                cU[j] = sy * (up[0] * M[j] + up[1] * M[4 + j] + up[2] * M[8 + j]);
            }
            for (int j = 0; j < 4; ++j) {
                K[e][0 + j] = cW[j] - pt.ndc[0] * cR[j] - pt.ndc[1] * cU[j];   // K0
                K[e][4 + j] = cR[j];                                          // Kx
                K[e][8 + j] = cU[j];                                          // Ky
            }
        }
    }

    return true;
}

void card_probe(const Point& pt, const float K[2][12]) {
    if (g_pieceProbe.load(std::memory_order_relaxed) > 0 && g_pieceProbe.fetch_sub(1) > 0)
        for (int e = 0; e < 2; ++e) {
            const float w = K[e][3], x = K[e][0] + pt.ndc[0] * K[e][4] + pt.ndc[1] * K[e][8];
            const float wf = K[e][3] + pt.ndc[0] * K[e][7] + pt.ndc[1] * K[e][11];
            const float y = K[e][1] + pt.ndc[0] * K[e][5] + pt.ndc[1] * K[e][9];
            BVR_LOG("mkdump card: eye %d - the marker's own point lands at %.4f %.4f (w %.2f), HUD had it at %.4f %.4f",
                    e, wf != 0 ? x / wf : 0.0f, wf != 0 ? y / wf : 0.0f, wf, pt.ndc[0], pt.ndc[1]);
            (void)w;
        }

}

// Draw one piece (or one marker's pieces of a batch) into each eye's layer, then put everything back.
bool draw_layers(ID3D11DeviceContext* ctx, ID3D11GeometryShader* gs, const float K[2][12], bool indexed, unsigned count,
                 unsigned startIndex, int baseVertex, unsigned instances, ID3D11Buffer* cbOver, bool gameBlend,
                 const float* region = nullptr, const float* regions = nullptr) {
    // ---- draw the piece once into each eye's layer, then put everything back ---------------------------------------
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) return false;
    ID3D11RenderTargetView* oldRtv[4] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ctx->OMGetRenderTargets(4, oldRtv, &oldDsv);
    UINT nvp = 1;
    D3D11_VIEWPORT oldVp[8] = {};
    ctx->RSGetViewports(&nvp, oldVp);
    ID3D11BlendState* oldBlend = nullptr; FLOAT oldFactor[4] = {}; UINT oldMask = 0;
    ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
    ID3D11GeometryShader* oldGs = nullptr; UINT nGsCi = 0;
    ctx->GSGetShader(&oldGs, nullptr, &nGsCi);
    ID3D11Buffer* oldGsCb = nullptr;
    ctx->GSGetConstantBuffers(13, 1, &oldGsCb);
    // BATCH (V11): this marker's own copy of the batch's constants at VS b0 (the other pieces collapsed); put back after
    ID3D11DeviceContext1* c1 = nullptr;
    ID3D11Buffer* oldVsCb = nullptr; UINT oldFirst = 0, oldNum = 0;
    if (cbOver && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {
        c1->VSGetConstantBuffers1(0, 1, &oldVsCb, &oldFirst, &oldNum);
        ctx->VSSetConstantBuffers(0, 1, &cbOver);
    }
    std::lock_guard<std::mutex> lk(g_layerMutex);
    // V12w (JJ: the lock-on dot pulses once a second on the card, never on the flat panel - EYEPROBE: its white core on
    // ~0.65 s / off ~0.35 s in both eyes): our layer drew every colour piece with ONE plain "over" blend. A Scaleform piece
    // drawn with another blend (multiply, add, erase...) leaves the sheet unchanged or changes it differently, but came out
    // as plain colour on the card. A piece whose game blend is not the plain over keeps the game's own blend here.
    bool special = false;
    if (!gameBlend && g_keepSpecial.load(std::memory_order_relaxed) && oldBlend) {
        D3D11_BLEND_DESC bd{}; oldBlend->GetDesc(&bd);
        const auto& r = bd.RenderTarget[0];
        const bool over = !r.BlendEnable ||
                          ((r.SrcBlend == D3D11_BLEND_SRC_ALPHA || r.SrcBlend == D3D11_BLEND_ONE) && r.DestBlend == D3D11_BLEND_INV_SRC_ALPHA &&
                           r.BlendOp == D3D11_BLEND_OP_ADD);
        special = !over;
        if (special) {
            const uint32_t key = (static_cast<uint32_t>(r.SrcBlend) << 24) | (static_cast<uint32_t>(r.DestBlend) << 16) |
                                 (static_cast<uint32_t>(r.BlendOp) << 8) | static_cast<uint32_t>(r.RenderTargetWriteMask);
            g_specialDraws.fetch_add(1, std::memory_order_relaxed);
            static uint32_t seen[16] = {}; static int nSeen = 0;
            bool known = false;
            for (int i = 0; i < nSeen; ++i) if (seen[i] == key) known = true;
            if (!known && nSeen < 16) {
                seen[nSeen++] = key;
                BVR_LOG("markers: a HUD piece with its own blend - src %d dest %d op %d | alpha src %d dest %d op %d | write mask %d - "
                        "kept on the card", r.SrcBlend, r.DestBlend, r.BlendOp, r.SrcBlendAlpha, r.DestBlendAlpha, r.BlendOpAlpha,
                        r.RenderTargetWriteMask);
            }
        }
    }

    const FLOAT one[4] = {1, 1, 1, 1};
    {   // the first piece of a frame wipes last frame's cards, here, in GPU order
        const uint32_t now = g_frameNo.load(std::memory_order_relaxed);
        if (g_clearedFrame.exchange(now) != now) {
            const float zero[4] = {0, 0, 0, 0};
            for (auto& L : g_layer) {
                if (L.rtv) ctx->ClearRenderTargetView(L.rtv, zero);
                if (L.dsv) ctx->ClearDepthStencilView(L.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            }
        }
    }
    for (int e = 0; e < 2; ++e) {
        ID3D11Buffer* kb = g_kcb[g_kNext++ % kKRing];
        if (!kb) continue;
        float kr[52] = {};
        memcpy(kr, K[e], 12 * sizeof(float));
        if (region) memcpy(kr + 12, region, 4 * sizeof(float));   // P.z = 0: no region test (the old per-piece path)
        if (regions) memcpy(kr + 16, regions, 36 * sizeof(float));   // V12l: every marker's region + (count, own index)
        ctx->UpdateSubresource(kb, 0, nullptr, kr, 0, 0);
        ctx->OMSetRenderTargets(1, &g_layer[e].rtv, g_stencil.load(std::memory_order_relaxed) ? g_layer[e].dsv : nullptr);
        D3D11_VIEWPORT v{0.0f, 0.0f, static_cast<float>(g_layer[e].w), static_cast<float>(g_layer[e].h), 0.0f, 1.0f};
        ctx->RSSetViewports(1, &v);
        if (gameBlend || special) ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);   // masks, and V12w special blends
        else ctx->OMSetBlendState(g_blend, one, 0xFFFFFFFF);
        ctx->GSSetShader(gs, nullptr, 0);
        ctx->GSSetConstantBuffers(13, 1, &kb);
        if (indexed) ctx->DrawIndexed(count, startIndex, baseVertex);
        else ctx->Draw(count, startIndex);
        (void)instances;
    }
    if (c1) {
        c1->VSSetConstantBuffers1(0, 1, &oldVsCb, &oldFirst, &oldNum);
        if (oldVsCb) oldVsCb->Release();
        c1->Release();
    }
    ctx->GSSetConstantBuffers(13, 1, &oldGsCb);
    ctx->GSSetShader(oldGs, nullptr, 0);
    if (oldGs) oldGs->Release();
    if (oldGsCb) oldGsCb->Release();
    ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
    ctx->RSSetViewports(nvp, oldVp);
    ctx->OMSetRenderTargets(4, oldRtv, oldDsv);
    for (auto& r : oldRtv) if (r) r->Release();
    if (oldDsv) oldDsv->Release();
    if (oldBlend) oldBlend->Release();
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    g_pieceFrame.store(g_frameNo.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return true;
}


// BATCH (V11, JJ: the health bar duplicated; Sekiro session vr-modding-f2: "Scaleform BATCHES up to 24 shapes per draw").
// Three of DS3's HUD vertex shaders draw several pieces in one call, each vertex carrying its piece number, each piece
// owning a block of rows in cb0 (shader dump 2026-10-30): aa4d1367 6 rows a piece, position rows +2 / +3 (index COLOR1.x);
// fa3d4612 4 rows, position +0 / +1 (index v2.x); f710b858 4 rows, position +2 / +3 (index COLOR2.x). A piece is collapsed
// (and so drawn nowhere) by zeroing its two position rows. Per batch: every piece is matched to a marker point; each marker
// gets one draw into the eye layers with only its pieces kept, and the flat draw keeps only the pieces of no marker.
struct BatchLayout { int stride, pos; };
// Every batched HUD vertex shader in the 2026-10-30 dump (67), worked out by re/batch_layouts.py: the piece's row block is
// stride x piece + pos for the position's x (y = the next row). The piece number comes from a vertex input - an integer
// (imad / ishl / bfi forms) or a UNORM colour scaled back (mad by 255 x stride).
struct BatchEntry { uint64_t hash; int stride, pos; };
const BatchEntry kBatch[] = {
    {0x0053fa5f89d5ad21ull, 4, 0},
    {0x06b9fe9c1a8958c2ull, 6, 2},
    {0x0b26f53c67535930ull, 6, 0},
    {0x0c9eeed40e7d7fb3ull, 6, 2},
    {0x1c48f9c4185e3d3eull, 6, 2},
    {0x1d5d847a3a9ae11aull, 4, 0},
    {0x1ff5bf1bf55311ccull, 8, 2},
    {0x218fd62890899cd2ull, 10, 2},
    {0x243d460ee68cd3f0ull, 6, 2},
    {0x2504035533ad34faull, 8, 2},
    {0x26ddbad732797f78ull, 4, 0},
    {0x2783ec8c2fed596full, 8, 2},
    {0x28c4a57cb7edec93ull, 6, 2},
    {0x2bfc02302beae4aeull, 2, 0},
    {0x35ebb5ceea8cc2aeull, 4, 0},
    {0x38b6dd440a9b0605ull, 2, 0},
    {0x40d8ba1b221d9571ull, 6, 0},
    {0x4352aae24f34ae86ull, 8, 2},
    {0x453b40119c454706ull, 6, 2},
    {0x46b97536a286bdacull, 6, 0},
    {0x4b3d709551e27f54ull, 8, 2},
    {0x4b3ea7e94f2ac4a1ull, 4, 2},
    {0x4d1cb9953a046e68ull, 8, 0},
    {0x5a3f1fff111d9570ull, 6, 0},
    {0x5be5850685d90498ull, 8, 2},
    {0x5d6271b1dfc29d04ull, 6, 0},
    {0x609d22e648b669b5ull, 2, 0},
    {0x61cd851dd0c72887ull, 8, 2},
    {0x62c2730d6aa3d2b6ull, 6, 2},
    {0x63cc22e62f739b76ull, 4, 2},
    {0x65d1b53f8bab39b7ull, 6, 2},
    {0x716ccbc0bc138a96ull, 4, 0},
    {0x71aff9633f674226ull, 4, 0},
    {0x722e1d71aca94f7dull, 8, 2},
    {0x76b326524f2af9a9ull, 8, 2},
    {0x83269005f8b105a9ull, 4, 0},
    {0x864c31d2fdb76216ull, 4, 0},
    {0x86b1c060b8fd4bb2ull, 4, 0},
    {0x8c855748bf821e31ull, 6, 2},
    {0x9ad7e9a99be2d323ull, 8, 0},
    {0x9ee13112904f975eull, 4, 2},
    {0xa714ac1d50c2d065ull, 4, 0},
    {0xaa4d136778e07873ull, 6, 2},
    {0xaae058ce3eca1e90ull, 2, 0},
    {0xac63605fba67f796ull, 8, 0},
    {0xad0cc2362a7a0e08ull, 6, 0},
    {0xb43a65a96c4d5b13ull, 6, 2},
    {0xb75227baa9591403ull, 6, 0},
    {0xb9d773efb11c4393ull, 6, 2},
    {0xc1bf5516a5299162ull, 4, 2},
    {0xc238ca2d1b4bbfb0ull, 10, 2},
    {0xc602d4d078ece3c5ull, 4, 0},
    {0xc73569859d992da5ull, 6, 2},
    {0xcebca1d95849f2a3ull, 6, 0},
    {0xd2a198cec649ebcaull, 4, 0},
    {0xd2b4323097e0ebe4ull, 2, 0},
    {0xd2dc6acd4eaf4724ull, 4, 0},
    {0xd440c67599f4b585ull, 6, 0},
    {0xd4e71cf18cd571e1ull, 4, 2},
    {0xdcdd8e0b231b3724ull, 6, 2},
    {0xe131842a4dfa9b7cull, 2, 0},
    {0xe58d2c7d87060528ull, 4, 2},
    {0xe75a4f38d0e37b34ull, 2, 0},
    {0xe7b6a383c610a511ull, 4, 2},
    {0xeaabf4583427852cull, 2, 0},
    {0xf710b858a2647473ull, 4, 2},
    {0xfa3d4612573b6d52ull, 4, 0},
};
BatchLayout batch_layout(ID3D11VertexShader* vs) {
    const uint64_t h = shaderid::hash_of(vs);
    for (const BatchEntry& b : kBatch) if (b.hash == h) return {b.stride, b.pos};
    return {0, 0};
}
constexpr unsigned kBatchRows = 144;                  // the shaders' CB0[144]
ID3D11Buffer* g_batchCb[8] = {};                      // ring of b0 copies (no write lands in a buffer a draw still reads)
unsigned g_batchNext = 0;
thread_local ID3D11Buffer* t_flatOver = nullptr;      // the flat draw's own copy (only the pieces of no marker)
std::atomic<uint32_t> g_batchDraws{0}, g_batchPieces{0}, g_batchPlaced{0}, g_masks{0};
ID3D11Buffer* batch_copy(ID3D11DeviceContext* ctx, const float* rows, unsigned n) {
    if (!g_dev) return nullptr;
    ID3D11Buffer*& b = g_batchCb[g_batchNext++ % 8];
    if (!b) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = kBatchRows * 16; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&d, nullptr, &b))) { b = nullptr; return nullptr; }
    }
    alignas(16) float tmp[kBatchRows * 4] = {};
    memcpy(tmp, rows, (n < kBatchRows ? n : kBatchRows) * 16);
    ctx->UpdateSubresource(b, 0, nullptr, tmp, 0, 0);
    return b;
}
bool nearest_point(float ax, float ay, Point& pt, float& bestD) {
    bestD = g_reach.load(std::memory_order_relaxed);
    bool have = false;
    std::lock_guard<std::mutex> lk(g_ptMutex);
    for (int bucket = 0; bucket < 2 && !have; ++bucket) {
        const int b = bucket == 0 ? g_cur : (g_cur ^ 1);
        for (int i = 0; i < g_nPts[b]; ++i) {
            if (fabsf(g_pts[b][i].ndc[0]) > 1.2f || fabsf(g_pts[b][i].ndc[1]) > 1.2f) continue;   // edge indicators
            const float dx = g_pts[b][i].ndc[0] - ax, dy = g_pts[b][i].ndc[1] - ay;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d < bestD) { bestD = d; pt = g_pts[b][i]; have = true; }
        }
    }
    return have;
}
bool same_point(const Point& a, const Point& b) { return a.w[0] == b.w[0] && a.w[1] == b.w[1] && a.w[2] == b.w[2]; }
void remember_last(const Point& p) { for (int i = 0; i < 3; ++i) g_last.w[i] = p.w[i]; g_last.seq = g_drawSeq; g_last.ok = true; }
// MKFOLLOW: the point of the marker the PREVIOUS HUD piece (or one of the two before it) went to, if this piece sits within
// mkfollowreach of it - Scaleform draws one marker's pieces back to back
bool follow_point(float ax, float ay, Point& pt, float& d) {
    if (!g_follow.load(std::memory_order_relaxed) || !g_last.ok || g_drawSeq - g_last.seq > 3) return false;
    std::lock_guard<std::mutex> lk(g_ptMutex);
    for (int bucket = 0; bucket < 2; ++bucket) {
        const int b = bucket == 0 ? g_cur : (g_cur ^ 1);
        for (int i = 0; i < g_nPts[b]; ++i) {
            const Point& q = g_pts[b][i];
            if (q.w[0] != g_last.w[0] || q.w[1] != g_last.w[1] || q.w[2] != g_last.w[2]) continue;
            const float dx = q.ndc[0] - ax, dy = q.ndc[1] - ay;
            d = sqrtf(dx * dx + dy * dy);
            if (d > g_followReach.load(std::memory_order_relaxed)) return false;
            pt = q;
            return true;
        }
    }
    return false;
}
float nearest_any(float ax, float ay) {   // for the log: how far the nearest marker point is, whatever the reach
    float best = 99.0f;
    std::lock_guard<std::mutex> lk(g_ptMutex);
    for (int i = 0; i < g_nPts[g_cur]; ++i) {
        const float dx = g_pts[g_cur][i].ndc[0] - ax, dy = g_pts[g_cur][i].ndc[1] - ay;
        best = fminf(best, sqrtf(dx * dx + dy * dy));
    }
    return best;
}

std::atomic<int> g_batchMixLog{40};   // V12b: the first batches holding marker AND non-marker pieces, logged on their own
bool hud_draw_batch(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, const float* cb0, unsigned cbRows, const BatchLayout& bl,
                    bool indexed, unsigned count, unsigned startIndex, int baseVertex, unsigned instances, bool mask) {
    const unsigned rows = cbRows < kBatchRows ? cbRows : kBatchRows;
    const unsigned pieces = rows / static_cast<unsigned>(bl.stride);
    if (!pieces) return false;
    g_batchDraws.fetch_add(1, std::memory_order_relaxed);
    // each piece's marker (index into `pts`), or -1
    Point pts[16]; int nPts = 0;
    int of[64];
    float pax[64] = {}, pay[64] = {};
    bool valid[64] = {};
    bool any = false;
    for (unsigned i = 0; i < pieces && i < 64; ++i) {
        of[i] = -1;
        const unsigned r = i * bl.stride + bl.pos;
        const float ax = cb0[r * 4 + 3], ay = cb0[(r + 1) * 4 + 3];
        const float sx = cb0[r * 4], sy = cb0[(r + 1) * 4 + 1];
        if (!(ax > -4.0f && ax < 4.0f && ay > -4.0f && ay < 4.0f) || (sx == 0.0f && sy == 0.0f)) continue;
        pax[i] = ax; pay[i] = ay; valid[i] = true;
        g_batchPieces.fetch_add(1, std::memory_order_relaxed);
        Point p{}; float d = 0;
        const bool hit = nearest_point(ax, ay, p, d);
        if (g_matchProbe.load(std::memory_order_relaxed) > 0 && g_matchProbe.fetch_sub(1) > 0)
            BVR_LOG("mkdump batch: vs %016llx piece %u/%u (rows %u+%d, %u in the window) origin %.4f %.4f scale %.6f %.6f -> %s",
                    static_cast<unsigned long long>(shaderid::hash_of(vs)), i, pieces, i * bl.stride, bl.pos, cbRows, ax, ay, sx, sy,
                    hit ? "a marker" : "none");
        if (!hit) continue;
        int k = 0;
        for (; k < nPts; ++k) if (same_point(pts[k], p)) break;
        if (k == nPts) { if (nPts >= 16) continue; pts[nPts++] = p; }
        of[i] = k;
        any = true;
    }
    // V12b MKFOLLOW inside a batch (JJ 14:54: the fill stayed flat with the single-piece follow - its pieces come in a
    // BATCH): a piece out of reach goes with the nearest marker of the same batch, or of the previous marker draw, within
    // mkfollowreach. Scaleform puts one marker's frame, fill and mask into one draw.
    if (g_follow.load(std::memory_order_relaxed)) {
        Point prev{}; float pd = 0;
        for (unsigned i = 0; i < pieces && i < 64; ++i) {
            if (of[i] >= 0 || !valid[i]) continue;
            int bestK = -1; float bd = g_followReach.load(std::memory_order_relaxed);
            for (int k = 0; k < nPts; ++k) {
                const float dx = pts[k].ndc[0] - pax[i], dy = pts[k].ndc[1] - pay[i];
                const float d = sqrtf(dx * dx + dy * dy);
                if (d < bd) { bd = d; bestK = k; }
            }
            if (bestK < 0 && nPts < 16 && follow_point(pax[i], pay[i], prev, pd)) { pts[nPts] = prev; bestK = nPts++; }
            if (bestK >= 0) { of[i] = bestK; any = true; g_followTaken.fetch_add(1, std::memory_order_relaxed); }
        }
    }
    if (g_batchMixLog.load(std::memory_order_relaxed) > 0 && g_nPts[g_cur] > 0) {
        // also a batch with NO marker piece, if one of its pieces is near a marker point (a fill drawn in its own batch)
        int nOn = 0, nOff = 0; float minNear = 99.0f;
        for (unsigned i = 0; i < pieces && i < 64; ++i) if (valid[i]) (of[i] >= 0 ? nOn : nOff)++;
        if (!nOn) for (unsigned i = 0; i < pieces && i < 64; ++i) if (valid[i]) minNear = fminf(minNear, nearest_any(pax[i], pay[i]));
        if (nOff > 0 && (nOn > 0 || minNear < 0.6f) && g_batchMixLog.fetch_sub(1) > 0) {
            char line[1400]; int n = snprintf(line, sizeof(line), "markers: mixed batch vs %016llx%s - %d on a marker, %d left:",
                static_cast<unsigned long long>(shaderid::hash_of(vs)), mask ? " MASK" : "", nOn, nOff);
            for (unsigned i = 0; i < pieces && i < 64 && n < 1300; ++i) {
                if (!valid[i]) continue;
                n += snprintf(line + n, sizeof(line) - n, " [%u %s %.3f,%.3f near %.3f]", i, of[i] >= 0 ? "ON" : "off", pax[i], pay[i],
                              nearest_any(pax[i], pay[i]));
            }
            BVR_LOG("%s", line);
        }
    }
    if (!any) return false;
    if (nPts == 1) remember_last(pts[0]);
    ID3D11GeometryShader* gs = gs_for(g_dev, vs, 0);
    if (!gs || !g_layersOk.load(std::memory_order_relaxed) || !g_blend) return false;
    alignas(16) float work[kBatchRows * 4];
    for (int k = 0; k < nPts; ++k) {
        float K[2][12];
        if (!card_K(pts[k], K)) continue;
        memcpy(work, cb0, rows * 16);
        for (unsigned i = 0; i < pieces && i < 64; ++i)
            if (of[i] != k) { const unsigned r = i * bl.stride + bl.pos; memset(work + r * 4, 0, 32); }
        ID3D11Buffer* b = batch_copy(ctx, work, rows);
        if (b && draw_layers(ctx, gs, K, indexed, count, startIndex, baseVertex, instances, b, mask)) g_batchPlaced.fetch_add(1, std::memory_order_relaxed);
    }
    if (mask) { g_maskTaken.fetch_add(1, std::memory_order_relaxed); return false; }   // a mask batch stays whole on the sheet
    // the flat draw: only the pieces of no marker
    memcpy(work, cb0, rows * 16);
    for (unsigned i = 0; i < pieces && i < 64; ++i)
        if (of[i] >= 0) { const unsigned r = i * bl.stride + bl.pos; memset(work + r * 4, 0, 32); }
    t_flatOver = batch_copy(ctx, work, rows);
    g_matched.fetch_add(1, std::memory_order_relaxed);
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    g_pieceFrame.store(g_frameNo.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return false;   // the caller draws the flat remainder with take_flat_override()
}

// V12m: may this draw hold a marker piece? Yes if any piece's own anchor is within 0.45 of a marker point, or sits at the
// stage centre (DS3 draws some pieces - the bar's fill - in absolute stage coordinates, anchor = centre: unknown), or the
// shader's placement rows are not known. No if every anchor is known and far from every point.
bool region_candidate(ID3D11VertexShader* vs, const float* cb0, unsigned cbRows) {
    float ax[64], ay[64]; int na = 0;
    if (const BatchLayout bl = batch_layout(vs); bl.stride) {
        const unsigned rows = cbRows < kBatchRows ? cbRows : kBatchRows;
        const unsigned pieces = rows / static_cast<unsigned>(bl.stride);
        for (unsigned i = 0; i < pieces && na < 64; ++i) {
            const unsigned r = i * bl.stride + bl.pos;
            const float x = cb0[r * 4 + 3], y = cb0[(r + 1) * 4 + 3];
            if (cb0[r * 4] == 0.0f && cb0[(r + 1) * 4 + 1] == 0.0f) continue;   // an unused / collapsed piece
            if (!(x > -4.0f && x < 4.0f && y > -4.0f && y < 4.0f)) continue;
            ax[na] = x; ay[na] = y; ++na;
        }
    } else {
        const int prow = pos_rows(vs);
        if (prow < 0 || prow > 4) return true;   // placement unknown
        ax[0] = cb0[prow * 4 + 3]; ay[0] = cb0[(prow + 1) * 4 + 3]; na = 1;
    }
    if (!na) return false;
    std::lock_guard<std::mutex> lk(g_ptMutex);
    float nearest = 99.0f;
    for (int a = 0; a < na; ++a)
        for (int b = 0; b < 2; ++b)
            for (int i = 0; i < g_nPts[b]; ++i) {
                const float dx = g_pts[b][i].ndc[0] - ax[a], dy = g_pts[b][i].ndc[1] - ay[a];
                nearest = fminf(nearest, sqrtf(dx * dx + dy * dy));
            }
    {   // V12t: the live tracks count too (a marker missing from both buckets for a frame)
        const uint64_t tnow = GetTickCount64();
        for (int a = 0; a < na; ++a)
            for (int t = 0; t < 16; ++t) {
                if (!g_trk[t].used || tnow - g_trk[t].at > 120) continue;
                const float dx = g_trk[t].p.ndc[0] - ax[a], dy = g_trk[t].p.ndc[1] - ay[a];
                nearest = fminf(nearest, sqrtf(dx * dx + dy * dy));
            }
    }
    if (nearest < 0.45f) return true;
    // V12p (JJ: the red fill still jumps to the 2D sheet now and then): every draw this filter leaves to the sheet while it
    // sits fairly near a marker (0.45-0.6) is logged - the first 60 a session, no command needed
    const auto log_far = [&](bool decision) {
        if (!decision && nearest >= 0.45f && nearest < 0.6f && g_farLog.load(std::memory_order_relaxed) > 0 && g_farLog.fetch_sub(1) > 0)
            BVR_LOG("markers: region filter left a draw to the sheet - vs %016llx, %d pieces, first anchor %.3f %.3f, nearest point %.3f",
                    static_cast<unsigned long long>(shaderid::hash_of(vs)), na, ax[0], ay[0], nearest);
        return decision;
    };
    for (int a = 0; a < na; ++a) {
        if (fabsf(ax[a]) < 0.06f && fabsf(ay[a] - 0.046f) < 0.06f) return log_far(true);   // anchored at the stage centre: unknown
        for (int b = 0; b < 2; ++b)
            for (int i = 0; i < g_nPts[b]; ++i) {
                const float dx = g_pts[b][i].ndc[0] - ax[a], dy = g_pts[b][i].ndc[1] - ay[a];
                if (dx * dx + dy * dy < 0.45f * 0.45f) return log_far(true);
            }
    }
    return log_far(false);
}

// MKREGION (V12g): every HUD draw while markers are on screen is drawn once per marker onto that marker's card, keeping only
// the triangles in the marker's region (GS variant 0 with P), and once flat onto the sheet WITHOUT those triangles (variant 1,
// re-issued by picsplit, which then skips the game's own draw). Clip masks travel the same way (drawn colour-off into the
// layer, whose own stencil is cleared per frame), so a clipped fill is clipped on the card too.
bool hud_draw_region(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, bool colourOff, bool indexed, unsigned count,
                     unsigned startIndex, int baseVertex, unsigned instances, bool hide) {
    // V12h: clip masks stay the game's own draw (V12j: back ON - the crashes were our own draws re-entering our hooks, fixed
    // in V12i; with masks off the fill, which is clipped by one, never showed on the card). Every build that sent masks into the marker layers (V12b batched masks,
    // V12g region) crashed after a lock-on (d3d11 ClearRenderTargetView / OMSetRenderTargets hazard walk, dumps 15:02,
    // 15:31, 16:03); the builds without them did not.
    if (colourOff && !g_regionMasks.load(std::memory_order_relaxed)) return false;
    Point pts[8]; int n = 0;
    {
        // V12j (JJ: lock-on glow on/off, the fill flashing on the sheet): the projection hook fills g_cur on the GAME thread
        // while the HUD draws here on the render thread, so g_cur was often half filled - a marker with no point that draw
        // fell to the sheet. Take the last COMPLETE frame's points (g_cur ^ 1) and add only points of g_cur that no complete
        // one is near (a marker just appearing) - one point per marker, every draw.
        std::lock_guard<std::mutex> lk(g_ptMutex);
        if (g_trackOn.load(std::memory_order_relaxed)) {
            const uint64_t tnow = GetTickCount64();
            for (int t = 0; t < 16 && n < 8; ++t) {
                if (!g_trk[t].used || tnow - g_trk[t].at > 120) continue;
                const Point& q = g_trk[t].p;
                if (fabsf(q.ndc[0]) > 1.2f || fabsf(q.ndc[1]) > 1.2f) continue;
                bool dup = false;
                for (int k = 0; k < n; ++k)
                    if (fabsf(pts[k].ndc[0] - q.ndc[0]) < 0.02f && fabsf(pts[k].ndc[1] - q.ndc[1]) < 0.02f) dup = true;
                if (!dup) pts[n++] = q;
            }
        }
        for (int pass = 0; pass < 2 && n == 0; ++pass) {
            const int b = pass == 0 ? (g_cur ^ 1) : g_cur;
            for (int i = 0; i < g_nPts[b] && n < 8; ++i) {
                const Point& q = g_pts[b][i];
                if (fabsf(q.ndc[0]) > 1.2f || fabsf(q.ndc[1]) > 1.2f) continue;   // edge indicators stay flat
                bool dup = false;
                for (int k = 0; k < n; ++k)
                    if (same_point(pts[k], q) || (fabsf(pts[k].ndc[0] - q.ndc[0]) < 0.05f && fabsf(pts[k].ndc[1] - q.ndc[1]) < 0.05f))
                        dup = true;
                if (!dup) pts[n++] = q;
            }
        }
        // V12k (JJ: the fill still flashes onto the sheet now and then): a frame with no point at all while a marker was on
        // screen a moment ago keeps the last points for up to 150 ms - the game does not project its markers every frame
        static Point s_last[8]; static int s_nLast = 0; static uint64_t s_lastAt = 0;
        const uint64_t now = GetTickCount64();
        if (n) { memcpy(s_last, pts, sizeof(Point) * n); s_nLast = n; s_lastAt = now; }
        else if (s_nLast && now - s_lastAt < 150) { memcpy(pts, s_last, sizeof(Point) * s_nLast); n = s_nLast; g_regionHeld.fetch_add(1); }
    }
    if (!n || !g_layersOk.load(std::memory_order_relaxed) || !g_blend) return false;
    ID3D11GeometryShader* gs = gs_for(g_dev, vs, 0);
    ID3D11GeometryShader* gsFlat = gs_for(g_dev, vs, 1);
    if (!gs || !gsFlat || !g_fcb[0]) return false;
    const float rx = g_regionX.load(std::memory_order_relaxed), ry = g_regionY.load(std::memory_order_relaxed);
    float R[36] = {};
    int nR = 0;
    float Ks[8][2][12];
    int idx[8];
    for (int k = 0; k < n; ++k) {
        if (!card_K(pts[k], Ks[nR])) continue;
        const float P[4] = {pts[k].ndc[0], pts[k].ndc[1], rx, ry};
        memcpy(R + 4 * nR, P, sizeof(P));
        idx[nR] = k;
        ++nR;
    }
    if (!nR) return false;
    R[32] = static_cast<float>(nR);
    for (int r = 0; r < nR && !hide; ++r) {   // V12z: a hidden piece goes on no card (and the flat side drops it too)
        R[33] = static_cast<float>(r);   // this card's own index
        // V12x (captures 185311-19: a large soft halo round the lock-on dot on the card, never on the flat panel): a clip MASK
        // goes WHOLE onto every nearby card - no region, no nearest-marker split. Big masks have their triangles' middles
        // away from the dot (or nearer the bar's point), so the halo's mask missed the dot's card and the halo showed
        // unclipped whenever the game's animation brought it up. A mask only writes the stencil, so a whole one is harmless.
        const float Pnone[4] = {R[4 * r], R[4 * r + 1], 0.0f, 0.0f};
        const bool maskWhole = colourOff && g_maskWhole.load(std::memory_order_relaxed);
        if (draw_layers(ctx, gs, Ks[r], indexed, count, startIndex, baseVertex, instances, nullptr, colourOff,
                        maskWhole ? Pnone : R + 4 * r, maskWhole ? nullptr : R)) {
            g_regionCards.fetch_add(1, std::memory_order_relaxed);
            g_cardsF.fetch_add(1, std::memory_order_relaxed);
            ++g_catchCardsAcc;
        }
    }
    g_regionF.fetch_add(1, std::memory_order_relaxed);
    ++g_catchRegionAcc;
    if (g_framesLog.load(std::memory_order_relaxed) > 0 && g_drawsNow.size() < 400) {
        DrawRec d{};
        d.vs = shaderid::hash_of(vs); d.count = count; d.start = startIndex; d.base = baseVertex; d.mask = colourOff;
        ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT bm = 0;
        ctx->OMGetBlendState(&bs, bf, &bm);
        if (bs) {
            D3D11_BLEND_DESC bd{}; bs->GetDesc(&bd); bs->Release();
            const auto& r = bd.RenderTarget[0];
            d.blend = (static_cast<uint32_t>(r.BlendEnable) << 28) | (static_cast<uint32_t>(r.SrcBlend) << 20) |
                      (static_cast<uint32_t>(r.DestBlend) << 12) | (static_cast<uint32_t>(r.SrcBlendAlpha) << 8) |
                      (static_cast<uint32_t>(r.DestBlendAlpha) << 4) | static_cast<uint32_t>(r.RenderTargetWriteMask);
        }
        ID3D11DepthStencilState* ds = nullptr; UINT ref = 0;
        ctx->OMGetDepthStencilState(&ds, &ref);
        if (ds) {
            D3D11_DEPTH_STENCIL_DESC dd{}; ds->GetDesc(&dd); ds->Release();
            d.dss = (static_cast<uint32_t>(dd.DepthEnable) << 28) | (static_cast<uint32_t>(dd.DepthFunc) << 24) |
                    (static_cast<uint32_t>(dd.StencilEnable) << 20) | (static_cast<uint32_t>(dd.FrontFace.StencilFunc) << 16) |
                    (static_cast<uint32_t>(dd.FrontFace.StencilPassOp) << 12) | (static_cast<uint32_t>(dd.StencilWriteMask) << 4) |
                    (dd.DepthWriteMask ? 1u : 0u);
        }
        d.ref = ref;
        d.ax = R[0]; d.ay = R[1];
        d.cbHash = t_cbHash; memcpy(d.cb, t_cb, sizeof(d.cb));
        g_drawsNow.push_back(d);
    }
    (void)idx;
    R[33] = 0.0f;
    // V12k (JJ: the main HUD's weapon icon vanished): a clip mask goes onto the card AND stays whole on the sheet - DS3's
    // big masks (the weapon slot's) have their middle inside an enemy's region, and dropping them there emptied the icon
    if (colourOff) { g_regionDraws.fetch_add(1, std::memory_order_relaxed); return false; }
    R[32] = static_cast<float>(nR);
    ID3D11Buffer* fb = g_fcb[g_fNext++ % kFRing];
    ctx->UpdateSubresource(fb, 0, nullptr, R, 0, 0);
    t_flatGs = gsFlat; t_flatCb = fb;
    g_regionDraws.fetch_add(1, std::memory_order_relaxed);
    g_pieceFrame.store(g_frameNo.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return false;   // picsplit draws the flat side with take_flat_gs() and skips the game's draw
}

bool take_flat_gs(ID3D11GeometryShader** gs, ID3D11Buffer** cb) {
    *gs = t_flatGs; *cb = t_flatCb;
    t_flatGs = nullptr; t_flatCb = nullptr;
    return *gs && *cb;
}

bool hud_draw(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, const float* cb0, unsigned cbRows, bool indexed, unsigned count,
              unsigned startIndex, int baseVertex, unsigned instances) {
    if (!g_on.load(std::memory_order_relaxed) || !ctx || !vs || !cb0) return false;
    // our own layer draws / flat redraws go back through the draw hooks: never handle them (the layer lock is not
    // re-entrant - vr-modding-f2 hit exactly this in Sekiro)
    static thread_local bool t_in = false;
    if (t_in) return false;
    struct Guard { Guard() { t_in = true; } ~Guard() { t_in = false; } } guard;
    // The piece's own placement: Scaleform's mvp maps the movie's coordinates to the sheet, so the piece's anchor is
    // where its local origin lands - mvp[0].w, mvp[1].w (rows c2 and c3 of the 96-byte Constants buffer).
    if (g_allProbe.load(std::memory_order_relaxed) > 0 && g_allProbe.fetch_sub(1) > 0)
        BVR_LOG("mkdump piece: %s %u verts | cxadd %.3f %.3f %.3f %.3f | cxmul %.3f %.3f %.3f %.3f | mvp0 %.5f %.5f %.5f "
                "%.5f | mvp1 %.5f %.5f %.5f %.5f | texgen0 %.4f %.4f %.4f %.4f", indexed ? "indexed" : "plain", count,
                cb0[0], cb0[1], cb0[2], cb0[3], cb0[4], cb0[5], cb0[6], cb0[7], cb0[8], cb0[9], cb0[10], cb0[11],
                cb0[12], cb0[13], cb0[14], cb0[15], cb0[16], cb0[17], cb0[18], cb0[19]);
    t_flatOver = nullptr;
    t_flatGs = nullptr; t_flatCb = nullptr;
    ++g_drawSeq;
    bool mask = false;
    {   // MASKS (V11, the "much larger red square"): Scaleform draws its clip shapes with colour writes OFF - they only mark
        // the stencil. Our layer draw uses its own blend, which writes colour, so a mask came out as a solid shape. A
        // piece the game draws without colour stays the game's own draw (on the sheet, where its stencil is used).
        ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT bm = 0;
        ctx->OMGetBlendState(&bs, bf, &bm);
        bool colourOff = false;
        if (bs) { D3D11_BLEND_DESC bd{}; bs->GetDesc(&bd); colourOff = (bd.RenderTarget[0].RenderTargetWriteMask & 0xF) == 0; bs->Release(); }
        if (g_region.load(std::memory_order_relaxed)) {
            // V12m (JJ: the main HUD's weapon icon gone since points are present every frame - it FLICKERED with the lock-on
            // when they were not): only draws that can hold a marker piece go the region way. A draw whose pieces all sit
            // far from every marker point by their own anchors (the player's bars, slots, weapon icon) stays the game's draw.
            D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ctx->IAGetPrimitiveTopology(&topo);
            if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
                g_regionNotTri.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (!region_candidate(vs, cb0, cbRows)) { g_regionFar.fetch_add(1, std::memory_order_relaxed); return false; }
            if (g_framesLog.load(std::memory_order_relaxed) > 0) {   // DRAWDIFF: this draw's constants (Scaleform cxform + mvp)
                uint64_t h = 1469598103934665603ull;
                const unsigned nf = (cbRows < 32 ? cbRows : 32) * 4;
                for (unsigned i = 0; i < nf; ++i) { uint32_t u; memcpy(&u, cb0 + i, 4); h = (h ^ u) * 1099511628211ull; }
                t_cbHash = h;
                for (int i = 0; i < 12; ++i) t_cb[i] = cbRows * 4 > static_cast<unsigned>(i) ? cb0[i] : 0.0f;
            }
            // V12z HALOHIDE (JJ: "hide the halo"): DRAWDIFF showed the lock-on's flash as a short-lived draw of vs fa3d4612
            // (present a frame or two, ~once a second) near the marker; near a marker it is dropped from card and sheet
            const uint64_t vh = shaderid::hash_of(vs);
            bool hide = false;
            for (int i = 0; i < g_nHide; ++i) if (g_hide[i] == vh) hide = true;
            if (hide) g_hidden.fetch_add(1, std::memory_order_relaxed);
            return hud_draw_region(ctx, vs, colourOff, indexed, count, startIndex, baseVertex, instances, hide);
        }
        if (colourOff) {
            g_masks.fetch_add(1, std::memory_order_relaxed);
            // MKMASK: with the layers' own stencil, a mask can go with its marker too (drawn colour-off, and kept on the sheet)
            if (!g_maskLayer.load(std::memory_order_relaxed) || !g_stencil.load(std::memory_order_relaxed) || batch_layout(vs).stride)
                return false;   // V12c: batched masks stay the game's (V12b drew them per frame or not = flicker, JJ 15:02)
            mask = true;
        }
    }
    if (const BatchLayout bl = batch_layout(vs); bl.stride)   // BATCH: several pieces in one draw
        return hud_draw_batch(ctx, vs, cb0, cbRows, bl, indexed, count, startIndex, baseVertex, instances, mask);
    const int prow = pos_rows(vs);                      // POSROWS: this shader's own placement rows
    if (prow < 0 || prow > 4) return false;               // batched / unknown: the piece stays where the game drew it
    const float ax = cb0[prow * 4 + 3], ay = cb0[(prow + 1) * 4 + 3];
    if (!(ax > -4.0f && ax < 4.0f && ay > -4.0f && ay < 4.0f)) return false;

    // nearest marker point. Every piece of one marker must take the SAME point (guide §6: never route by screen band).
    float bestD = g_reach.load(std::memory_order_relaxed);
    Point pt{};
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_ptMutex);
        for (int bucket = 0; bucket < 2 && !have; ++bucket) {
            const int b = bucket == 0 ? g_cur : (g_cur ^ 1);
            for (int i = 0; i < g_nPts[b]; ++i) {
                // Off-screen indicators are pinned to the edge, not at their projected point (guide section 6).
                if (fabsf(g_pts[b][i].ndc[0]) > 1.2f || fabsf(g_pts[b][i].ndc[1]) > 1.2f) continue;
                const float dx = g_pts[b][i].ndc[0] - ax, dy = g_pts[b][i].ndc[1] - ay;
                const float d = sqrtf(dx * dx + dy * dy);
                if (d < bestD) { bestD = d; pt = g_pts[b][i]; have = true; }
            }
        }
    }
    {   // how far the matched pieces sit from where the derived map puts their marker (a check, not a correction)
        std::lock_guard<std::mutex> fl(g_fitMutex);
        if (have) { g_fit.resid += bestD; ++g_fit.residN; ++g_fit.n; }
    }
    bool followed = false;
    if (!have && follow_point(ax, ay, pt, bestD)) { have = followed = true; g_followTaken.fetch_add(1, std::memory_order_relaxed); }
    g_placed.fetch_add(1, std::memory_order_relaxed);
    if (g_matchProbe.load(std::memory_order_relaxed) > 0 && g_matchProbe.fetch_sub(1) > 0)   // V11: which point each piece took
        BVR_LOG("mkdump match: vs %016llx %u verts%s | row %d origin %.4f %.4f | scale %.6f %.6f | cxmul %.2f %.2f %.2f %.2f | %s point "
                "world %.2f %.2f %.2f hud %.4f %.4f depth %.2f from +0x%X (%.4f away; nearest point %.4f; last marker %u draws ago)",
                static_cast<unsigned long long>(shaderid::hash_of(vs)), count, mask ? " MASK" : "", prow, ax, ay, cb0[prow * 4],
                cb0[(prow + 1) * 4 + 1], cb0[4], cb0[5], cb0[6], cb0[7], followed ? "FOLLOWS" : (have ? "->" : "no"), pt.w[0], pt.w[1],
                pt.w[2], pt.ndc[0], pt.ndc[1], pt.z, pt.caller, have ? bestD : 0.0f, nearest_any(ax, ay), g_drawSeq - g_last.seq);
    if (!have) { g_unmatched.fetch_add(1, std::memory_order_relaxed); return false; }
    g_matched.fetch_add(1, std::memory_order_relaxed);

    ID3D11GeometryShader* gs = gs_for(g_dev, vs, 0);
    if (!gs || !g_layersOk.load(std::memory_order_relaxed) || !g_blend) return false;

    float K[2][12];
    if (!card_K(pt, K)) return false;
    card_probe(pt, K);
    if (!draw_layers(ctx, gs, K, indexed, count, startIndex, baseVertex, instances, nullptr, mask)) return false;
    remember_last(pt);
    if (mask) { g_maskTaken.fetch_add(1, std::memory_order_relaxed); return false; }   // the sheet keeps its own copy: its
                                                                                        // other pieces may clip by it too
    return true;   // the piece must NOT also go onto the flat HUD panel
}

ID3D11Buffer* take_flat_override() { ID3D11Buffer* b = t_flatOver; t_flatOver = nullptr; return b; }


void draw_ui() {
    // rows are drawn by adapter.cpp (HUD drawer); kept here so the wording lives with the code
}

const char* status() {
    static char s[1200];
    uint32_t seen = 0, noCb = 0, taken = 0;
    picsplit::hud_stats(seen, noCb, taken);
    snprintf(s, sizeof(s), "marker points %u | draws offered %u, into the HUD sheet %u (no placement %u) | pieces on a "
                           "marker %u, elsewhere %u, drawn in the eyes %u | card shaders built %u, failed %u, no "
                           "bytecode %u%s%s",
             g_pointsSeen.load(), picsplit::hud_armed(), seen, noCb, g_matched.load(), g_unmatched.load(), g_drawn.load(),
             g_gsBuilt.load(), g_gsFailed.load(), g_noCode.load(), g_lastErr[0] ? " | last trouble: " : "", g_lastErr);
    {
        const size_t n = strlen(s);
        snprintf(s + n, sizeof(s) - n, " | batches %u, their pieces %u, marker draws from them %u | masks left to the game %u | holder camera %llu (missed %llu, %.3f m from ours) | followed %u, masks with their marker %u | eye camera read at the draw %u | region: HUD draws %u, card draws %u, draws with held points %u, left alone (far %u, not triangles %u) | fillcatch: checked %u, red on the sheet %u | hidden near markers %u", g_batchDraws.load(),
                 g_batchPieces.load(), g_batchPlaced.load(), g_masks.load(), static_cast<unsigned long long>(g_holderUsed.load()),
                 static_cast<unsigned long long>(g_holderMiss.load()), static_cast<double>(g_holderGap.load()), g_followTaken.load(), g_maskTaken.load(), g_vpNowUsed.load(), g_regionDraws.load(), g_regionCards.load(), g_regionHeld.load(), g_regionFar.load(), g_regionNotTri.load(), g_catchChecked.load(), g_catchHits.load(), g_hidden.load());
    }
    return s;
}

bool handle_command(const char* cmd, const char* args) {
    if (_stricmp(cmd, "markers") == 0) { g_on.store(atoi(args)); settings::set_i("markers", g_on.load()); BVR_LOG("markers: %s", g_on.load() ? "on" : "off"); return true; }
    if (_stricmp(cmd, "mkstencil") == 0) { g_stencil.store(atoi(args)); settings::set_i("marker_stencil", g_stencil.load()); return true; }
    if (_stricmp(cmd, "mkmask") == 0) { g_maskLayer.store(atoi(args)); settings::set_i("marker_masks", g_maskLayer.load()); return true; }
    if (_stricmp(cmd, "mkfollow") == 0) { g_follow.store(atoi(args)); settings::set_i("marker_follow", g_follow.load()); return true; }
    if (_stricmp(cmd, "mkfollowreach") == 0) { g_followReach.store(static_cast<float>(atof(args))); settings::set_f("marker_follow_reach", g_followReach.load()); return true; }
    if (_stricmp(cmd, "mkregion") == 0) {   // "mkregion 0|1 [half-width half-height]"
        int on = 1; float rx = 0, ry = 0;
        const int n = sscanf_s(args, "%d %f %f", &on, &rx, &ry);
        g_region.store(on); settings::set_i("marker_region", on);
        if (n >= 3 && rx > 0 && ry > 0) { g_regionX.store(rx); g_regionY.store(ry); settings::set_f("marker_region_x", rx); settings::set_f("marker_region_y", ry); }
        BVR_LOG("markers: region sorting %d (%.3f x %.3f)", on, g_regionX.load(), g_regionY.load());
        return true;
    }
    if (_stricmp(cmd, "mkhold") == 0) { g_layerHold.store(atoi(args)); return true; }
    if (_stricmp(cmd, "mkcatch") == 0) { g_catchOn.store(atoi(args)); return true; }
    if (_stricmp(cmd, "mkhide") == 0) {   // "mkhide off" | "mkhide <vs hash hex>" (adds; up to 8)
        if (!_strnicmp(args, "off", 3)) g_nHide = 0;
        else if (g_nHide < 8) g_hide[g_nHide++] = _strtoui64(args, nullptr, 16);
        BVR_LOG("markers: %d shader(s) hidden near markers (%u pieces hidden so far)", g_nHide, g_hidden.load());
        return true;
    }
    if (_stricmp(cmd, "mkmaskwhole") == 0) { g_maskWhole.store(atoi(args)); return true; }
    if (_stricmp(cmd, "mkblend") == 0) { g_keepSpecial.store(atoi(args)); BVR_LOG("markers: special blends %s (%u drawn so far)", atoi(args) ? "kept" : "plain over", g_specialDraws.load()); return true; }
    if (_stricmp(cmd, "mktrack") == 0) { g_trackOn.store(atoi(args)); return true; }
    if (_stricmp(cmd, "mkframes") == 0) { g_framesLog.store(atoi(args) > 0 ? atoi(args) : 120); return true; }
    if (_stricmp(cmd, "mkregionmasks") == 0) { g_regionMasks.store(atoi(args)); return true; }
    if (_stricmp(cmd, "mkvpnow") == 0) { g_vpNow.store(atoi(args)); settings::set_i("marker_vp_now", g_vpNow.load()); return true; }
    if (_stricmp(cmd, "mkworld") == 0) { g_world.store(atoi(args)); settings::set_i("marker_world", g_world.load()); return true; }
    if (_stricmp(cmd, "mkholder") == 0) { g_holderCam.store(atoi(args)); settings::set_i("marker_holder", g_holderCam.load()); return true; }
    if (_stricmp(cmd, "mkbase") == 0) { g_base.store(atoi(args)); settings::set_i("marker_base", g_base.load()); return true; }
    if (_stricmp(cmd, "mkface") == 0) { g_facePos.store(atoi(args)); settings::set_i("marker_face_pos", g_facePos.load()); return true; }
    if (_stricmp(cmd, "mkreach") == 0) { g_reach.store(static_cast<float>(atof(args))); settings::set_f("marker_reach", g_reach.load()); return true; }
    if (_stricmp(cmd, "mksmooth") == 0) { g_smooth.store(static_cast<float>(atof(args))); settings::set_f("marker_smooth", g_smooth.load()); return true; }
    if (_stricmp(cmd, "mkdump") == 0) {
        const int n = atoi(args) > 0 ? atoi(args) : 2;
        g_probe.store(n); g_allProbe.store(n * 8); g_pieceProbe.store(n * 4); g_matchProbe.store(n * 40);
        return true;
    }
    if (_stricmp(cmd, "mktest") == 0) {
        float x = 0, y = 0, d = 0;
        sscanf_s(args, "%f %f %f", &x, &y, &d);
        g_testPt[0].store(x); g_testPt[1].store(y); g_testPt[2].store(d);
        BVR_LOG("markers: test point %s (HUD %.3f %.3f, %.1f m ahead)", d > 0.1f ? "ON" : "off", x, y, d);
        return true;
    }
    if (_stricmp(cmd, "mkinfo") == 0) { BVR_LOG("markers: %s", status()); return true; }
    return false;
}

} // namespace bvr::ds3::markers

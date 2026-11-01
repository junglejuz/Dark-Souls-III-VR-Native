// DS3VR2 camera - see camera.h. Ported from v1 skvr/src/sk_camera.cpp (HEADSET-proven composition), with the head and
// both eyes taken from ONE locate generation of the core runtime (get_stereo_pose: playbook 09 #pose-timing-contract,
// XR-005) instead of AKVR's xr.cpp, and the per-eye cameras published for the engine's views (M3).
#include "camera.h"

#include "addresses.h"
#include "eyesize.h"
#include "lens.h"
#include "offaxis.h"
#include "settings.h"

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <MinHook.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
// The two `call ChrCam-director` sites are re-pointed at ds3_dir_stub (camera_stub.asm), which restores the game's
// own camera values, calls the real director through ds3_director_abs, and then writes the head / eye pose into the
// ChrCam. Nothing of ours is written INSIDE the director: Arxan guards that function and undoes it (HOOKLIVE,
// 2026-10-30 - the seam fired 73 times and then the game's own bytes were back).
void* ds3_director_abs = nullptr;
void  ds3_dir_stub();
void  ds3_restore_camera(unsigned char* chrcam);
void  ds3_after_director(unsigned char* chrcam);
}

namespace bvr::ds3::camera {
namespace {

// ---------------- small maths (v1) ----------------
struct V3 { float x, y, z; };
struct Q { float x, y, z, w; };
inline V3 v(float x, float y, float z) { return V3{x, y, z}; }
inline V3 add(V3 a, V3 b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 sub(V3 a, V3 b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 mul(V3 a, float s) { return v(a.x * s, a.y * s, a.z * s); }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float len(V3 a) { return sqrtf(dot(a, a)); }
inline V3 norm(V3 a) { float l = len(a); return l > 1e-8f ? mul(a, 1.0f / l) : a; }
inline Q qmul(Q a, Q b) {
    return Q{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline V3 qrot(Q q, V3 p) {
    V3 qv = v(q.x, q.y, q.z);
    V3 t = mul(cross(qv, p), 2.0f);
    return add(add(p, mul(t, q.w)), cross(qv, t));
}
inline V3 rotY(V3 p, float a) { float c = cosf(a), s = sinf(a); return v(c * p.x + s * p.z, p.y, -s * p.x + c * p.z); }
inline float xr_heading(Q q) { V3 f = qrot(q, v(0, 0, -1)); return atan2f(-f.x, -f.z); }
constexpr float kRad2Deg = 57.2957795f;

// ---------------- state ----------------
uintptr_t g_outSite = 0, g_director = 0;
bool g_installed = false;
char g_status[256] = "camera: not installed";

bool  g_headOn = true;
float g_posScale = 1.0f;   // head lean, 1 = metres 1:1
float g_pitchKeep = 1.0f;  // JJ v1: 1.0 = the game camera's tilt kept, the headset adds to it
float g_ipdScale = 1.0f;   // eye separation multiplier (world scale: 1 = the runtime's real eye distance)
bool  g_fovLock = true;
int   g_fwdSign = 1;       // row +0x30 points where the camera looks (v1 DS3/Dark Souls III: +1)

float g_refYaw = 0.0f;
V3    g_refPos{0, 0, 0};
bool  g_refSet = false;
uint32_t g_recenterSeen = 0;
volatile LONG g_recenterReq = 0;

unsigned char* volatile g_lastCam = nullptr;
volatile uintptr_t g_lastCamVt = 0;
volatile ULONGLONG g_lastCamMs = 0;
unsigned char g_prist[0x50];
unsigned char g_written[0x50];
bool g_wrote = false;
volatile LONG64 g_updates = 0, g_injected = 0;
V3 g_lastFh{0, 0, 1};

// HOOKLIVE counters: how often each of our three hooks is actually entered. On a game with Arxan-protected code
// (and function entries that are `jmp` stubs) "the hook installed" is not the same as "the hook runs", so each one
// is counted and reported by `ds camstat`.
volatile LONG64 g_dirCalls = 0, g_cullCalls = 0, g_cullWidened[2] = {0, 0};
volatile float g_cullMargin = 1.15f;   // CULLMARGIN2 (V11), live since V12
volatile bool g_cullRight = true;   // CULLRIGHT: `ds cullright 0` = the old exact match (A/B)

// CULLING CAMERA (v1, LIVE): two builders read +0x50..+0x5C. When the source carries the FOV we wrote they get a copy
// whose frustum covers what is really drawn (both eyes, M4: the union).
volatile float g_lastFovWritten = 0.0f, g_cullFov = 0.0f, g_cullAspect = 0.0f;
using CullFn = void*(__fastcall*)(void*, void*);
CullFn oCull[2] = {nullptr, nullptr};
// V13c CULLANY (JJ: gravestones pop in the periphery of both eyes; AFW too; no widening so far touched it): the builders were
// widened only for a source carrying OUR fov (right eye: never). Every distinct source fov is now logged (first 12), and with
// `ds cullany 1` (default) any source narrower than the culling fov is widened as well.
volatile float g_seenFov[12] = {}; volatile LONG g_nSeenFov = 0;
volatile LONG64 g_cullAnyWidened = 0;
volatile bool g_cullAny = false;   // OFF - did not stop the popping
template <int I> void* __fastcall hkCull(void* src, void* dst) {
    InterlockedIncrement64(&g_cullCalls);
    const float wrote = g_lastFovWritten, cf = g_cullFov, ca = g_cullAspect;
    if (src) {
        const float sf = *(const float*)((const uint8_t*)src + 0x50), sa = *(const float*)((const uint8_t*)src + 0x54);
        bool known = false;
        for (LONG i = 0; i < g_nSeenFov; ++i) if (fabsf(g_seenFov[i] - sf) < 0.002f) known = true;
        if (!known && g_nSeenFov < 12) {
            const LONG k = InterlockedIncrement(&g_nSeenFov) - 1;
            if (k < 12) {
                g_seenFov[k] = sf;
                BVR_LOG("camera: cull builder %d sees a source fov %.4f (aspect %.4f) - ours %.4f, culling fov %.4f", I, sf, sa, wrote, cf);
            }
        }
        const float d0 = sf - wrote;
        const bool ours = wrote > 0.0f && d0 > -0.0005f && d0 < 0.0015f;
        if (!ours && g_cullAny && cf > 0.0f && sf > 0.1f && sf < cf) {
            InterlockedIncrement64(&g_cullAnyWidened);
            alignas(16) uint8_t fake[0x60];
            memcpy(fake, src, sizeof(fake));
            *(float*)(fake + 0x50) = cf;
            if (ca > 0.0f && sa < ca) *(float*)(fake + 0x54) = ca;
            return oCull[I](fake, dst);
        }
    }
    // CULLRIGHT (V5, JJ: culling at the very right edge): with OFFAXIS the right eye's link fov is ours + 0.001 (the eye tag,
    // stereo.cpp) and the engine's copies drift a few float steps, so an exact match only ever widened the LEFT eye's frustum.
    const float fov = src ? *(const float*)((const uint8_t*)src + 0x50) : 0.0f;
    const float d = fov - wrote;
    if (src && wrote > 0.0f && cf > 0.0f && (g_cullRight ? d > -0.0005f && d < 0.0015f : fov == wrote)) {
        InterlockedIncrement64(&g_cullWidened[d > 0.0005f ? 1 : 0]);
        alignas(16) uint8_t fake[0x60];
        memcpy(fake, src, sizeof(fake));
        *(float*)(fake + 0x50) = cf;
        *(float*)(fake + 0x54) = ca;
        return oCull[I](fake, dst);
    }
    return oCull[I](src, dst);
}

// CULL3 (V13, JJ: gravestones and rocks pop at the outer edges of BOTH eyes; cull_margin 2.0 and the two hooked builders
// change nothing). CULLHUNT (hardware read watches on both views' camera links) found two readers of the MAIN link only;
// 0x1400522B0(this, ?, link, ...) copies the link's rows, fov (+0x50), near / far, aspect (+0x64) and then walks an
// object list - a culler that never sees the second view's camera. Hooked: it gets a copy of the link whose frustum is
// widened (tangent x g_cull3K, default 1.35) - covering both eyes' outer edges from the first eye's camera.
using Cull3Fn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
Cull3Fn oCull3 = nullptr;
volatile LONG64 g_cull3Calls = 0, g_cull3Widened = 0;
volatile float g_cull3K = 1.0f;   // OFF - not the gravestone culler
void* __fastcall hkCull3(void* a, void* b, void* link, void* d, void* e, void* f, void* g, void* h) {
    InterlockedIncrement64(&g_cull3Calls);
    const float k = g_cull3K;
    if (link && k > 1.0f) {
        alignas(16) uint8_t fake[0xA0];
        memcpy(fake, link, sizeof(fake));
        const float fov = *reinterpret_cast<const float*>(fake + 0x50);
        if (fov > 0.1f && fov < 3.0f) {
            float nf = 2.0f * atanf(tanf(0.5f * fov) * k);
            if (nf > 2.9f) nf = 2.9f;
            *reinterpret_cast<float*>(fake + 0x50) = nf;
            InterlockedIncrement64(&g_cull3Widened);
            return oCull3(a, b, fake, d, e, f, g, h);
        }
    }
    return oCull3(a, b, link, d, e, f, g, h);
}

// Published cameras: [0] head, [1] left, [2] right, [3] the game's own (pre-head). Seqlock.
volatile LONG g_seq = 0;
Cam g_pub[4];

// ---------------- LENS (M4): each eye's shape (lens.h) ----------------
// mode 2 = turned eyes (default), 0 = one symmetric lens for both eyes, -1 = the pre-M4 behaviour (core's suggested fov,
// nothing declared per eye). The launch's mode sizes the picture (eyesize); `ds lens` switches live for A/B tests (the
// declared lens always follows the real picture shape, so a switch stays correct, only less tight).
int g_lensMode = 1;
bool g_lensNoted = false, g_lensShortLogged = false;
struct LensNow { bool ok = false; float hx = 0, vy = 0, fov = 0, cullFov = 0, cullAspect = 0; float q[2][4]; unsigned w = 0, h = 0;
                 bool offAxis = false; float ang[2][4] = {}; };   // mode 1: each eye's exact window (runtime angles)
LensNow g_lens;
float fov_for_half_h(float halfH);

bool compute_lens(LensNow& L) {
    float ang[2][4];
    if (g_lensMode < 0 || !bvr::vr::eye_fov_angles(0, ang[0]) || !bvr::vr::eye_fov_angles(1, ang[1])) return false;
    uint32_t rw = 0, rh = 0;
    bvr::vr::recommended_eye_size(&rw, &rh);
    if (!g_lensNoted && rw && rh) { eyesize::note_runtime(rw, rh, ang); g_lensNoted = true; } // sizes the NEXT launch
    const lens::Shape s = lens::shape(g_lensMode, ang);
    unsigned w = 0, h = 0;
    if (!eyesize::internal(w, h) || !w || !h) { w = rw; h = rh; }
    if (!w || !h || s.hx <= 0.0f) return false;
    L.w = w; L.h = h;
    L.hx = s.hx;
    L.vy = s.hx * static_cast<float>(h) / static_cast<float>(w); // Dark Souls III keeps the horizontal extent (v1 FOVLAW)
    if (L.vy < s.hy * 0.98f && !g_lensShortLogged) {
        g_lensShortLogged = true;
        BVR_LOG("lens: the picture %ux%u is too wide for the lens - top/bottom of each eye uncovered (needs %.3f, has %.3f)", w, h, s.hy, L.vy);
    }
    memcpy(L.q, s.q, sizeof(L.q));
    L.fov = fov_for_half_h(atanf(L.hx));
    L.offAxis = g_lensMode == 1;
    if (L.offAxis) {
        // OFFAXIS: the projection itself is replaced per eye (offaxis.cpp), so the engine's own centred fov only has to
        // COVER both windows - its other users (LOD, the god-ray focal length) then see the whole field.
        memcpy(L.ang, ang, sizeof(L.ang));
        float bx = 0, by = 0;
        for (int e = 0; e < 2; ++e) {
            bx = fmaxf(bx, fmaxf(fabsf(tanf(ang[e][0])), fabsf(tanf(ang[e][1]))));
            by = fmaxf(by, fmaxf(fabsf(tanf(ang[e][2])), fabsf(tanf(ang[e][3]))));
            offaxis::set_window(e, tanf(ang[e][0]), tanf(ang[e][1]), tanf(ang[e][3]), tanf(ang[e][2]));
        }
        L.vy = s.hy;
        // CULLRIGHT2 (V9, JJ: culling at the very right edge): our view does not cull through the two hooked cull builders
        // (camstat: right-eye camera widened 0), it culls with this fov - which covered both windows EXACTLY, so objects
        // whose bounds straddle the outer edge of the right eye popped. A margin (setting cull_margin, default 1.05 = the
        // game view's culling margin) - nothing else draws with this fov: each eye's projection is its own window.
        // CULLMARGIN2 (V11, JJ: still culling at the right edge, and at the left one on quick head turns): the game culls with
        // a camera a frame behind the head - ~3 deg at a quick 300 deg/s turn = ~11 % of the tangent at the edge - so 5 % was too
        // small. 15 % for both views (the game view's cull hook below uses the same margin).
        // live: `ds cullmargin X` (1.0 - 2.0) for the A/B in one launch
        static const bool kRead = [] { g_cullMargin = settings::f("cull_margin", 1.15f); return true; }();
        (void)kRead;
        const float kMargin = g_cullMargin < 1.0f ? 1.0f : g_cullMargin > 2.0f ? 2.0f : g_cullMargin;
        L.fov = fov_for_half_h(atanf(bx * kMargin));
        L.cullFov = 2.0f * atanf(by * kMargin);
        L.cullAspect = bx / by;
        L.ok = true;
        return true;
    }
    // Culling (both views and the animation culler): one frustum around both eyes' turned lenses, seen from the head.
    float mx = 0, my = 0;
    for (int e = 0; e < 2; ++e)
        for (float x : {-L.hx, L.hx})
            for (float y : {-L.vy, L.vy}) {
                const float c[3] = {x, y, -1.0f};
                float t[3];
                lens::quat_rot(L.q[e], c, t);
                if (t[2] >= -1e-4f) continue;
                mx = fmaxf(mx, fabsf(t[0] / -t[2]));
                my = fmaxf(my, fabsf(t[1] / -t[2]));
            }
    mx *= 1.05f; my *= 1.05f;
    L.cullFov = 2.0f * atanf(my);
    L.cullAspect = mx / my;
    L.ok = true;
    return true;
}
bvr::vr::HeadPose turned(bvr::vr::HeadPose p, const float q[4]) {
    const Q r = qmul(Q{p.qx, p.qy, p.qz, p.qw}, Q{q[0], q[1], q[2], q[3]});
    p.qx = r.x; p.qy = r.y; p.qz = r.z; p.qw = r.w;
    return p;
}
uint32_t g_pubGen = 0;
constexpr int kGenRing = 16;
uint32_t g_genRing[kGenRing];
volatile LONG64 g_genN = 0;

void publish(const Cam* cams, uint32_t gen) {
    LONG s = g_seq; g_seq = s + 1; MemoryBarrier();
    for (int i = 0; i < 4; ++i) g_pub[i] = cams[i];
    g_pubGen = gen;
    MemoryBarrier(); g_seq = s + 2;
}

// ---------------- director entry: put the game's own values back ----------------
// A2.3 "don't read your own output back": before the director runs again, the ChrCam must hold the values the
// GAME last wrote, not the head pose we injected. On Sekiro this was a MinHook on the director's entry; DS3's
// entry is an Arxan `jmp` into the protected .text, so the two `call director` sites are re-pointed instead.
void restore_pristine(unsigned char* self) {
    InterlockedIncrement64(&g_dirCalls);
    if (g_wrote && self == g_lastCam) {
        if (memcmp(self + 0x10, g_written, sizeof(g_written)) == 0) memcpy(self + 0x10, g_prist, sizeof(g_prist));
        g_wrote = false;
    }
}
int g_dirMode = 1;            // 1 = re-point the call sites (default), 0 = no restore (drift; test only)
int g_dirSites = 0;
uintptr_t g_dirSite[4] = {};
// HOOKLIVE: the first bytes of the copy-out seam, as our hook left them. Arxan's guards repair modified code, so
// the same bytes are read back later: if they changed, the hook was undone and that is why nothing fires.
uint8_t g_outAfterHook[8] = {};

// ---------------- pattern scan on the live image (v1) ----------------
bool scan_text(const char* pat, uintptr_t& out, int& hits, int want = 1) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t bytes[64]; bool wild[64]; int n = 0;
    for (const char* p = pat; *p && n < 64;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') { wild[n] = true; bytes[n++] = 0; p += (p[1] == '?') ? 2 : 1; }
        else { char hx[3] = {p[0], p[1], 0}; wild[n] = false; bytes[n++] = (uint8_t)strtoul(hx, nullptr, 16); p += 2; }
    }
    hits = 0; out = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        const uint8_t* b = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
        const size_t sz = sec->Misc.VirtualSize;
        for (size_t k = 0; k + n <= sz; ++k) {
            int j = 0;
            for (; j < n; ++j) if (!wild[j] && b[k + j] != bytes[j]) break;
            if (j == n) { if (!hits) out = reinterpret_cast<uintptr_t>(b + k); ++hits; }
        }
    }
    return hits == want;
}

// ---------------- re-point the `call director` sites (DS3; v1 dirhook=1) ----------------
// Our DLL can land further than 2 GB from the game's .text, so a 5-byte `call rel32` cannot reach ds3_dir_stub
// directly. One 12-byte near trampoline (`mov rax, imm64 / jmp rax`) is allocated close to the module and every
// call site's rel32 is re-aimed at it. rax is caller-saved at a call site, so clobbering it is safe.
void* alloc_near(uintptr_t anchor, size_t bytes) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t step = gran; step < 0x60000000ULL; step += gran) {
        for (int dir = 0; dir < 2; ++dir) {
            const uintptr_t want = dir ? anchor + step : (anchor > step ? anchor - step : 0);
            if (!want) continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(want & ~(gran - 1)), bytes, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

void repoint_director_calls() {
    ds3_director_abs = reinterpret_cast<void*>(g_director);
    uint8_t* tramp = static_cast<uint8_t*>(alloc_near(g_director, 64));
    if (!tramp) { BVR_LOG("camera: no memory near the game for the director stub - restore OFF"); return; }
    tramp[0] = 0x48; tramp[1] = 0xB8;                                    // mov rax, imm64
    *reinterpret_cast<void**>(tramp + 2) = reinterpret_cast<void*>(&ds3_dir_stub);
    tramp[10] = 0xFF; tramp[11] = 0xE0;                                  // jmp rax
    DWORD old = 0;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);

    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        uint8_t* b = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
        const size_t sz = sec->Misc.VirtualSize;
        for (size_t k = 0; k + 5 <= sz; ++k) {
            if (b[k] != 0xE8) continue;
            const int32_t rel = *reinterpret_cast<const int32_t*>(b + k + 1);
            const uintptr_t dest = reinterpret_cast<uintptr_t>(b + k) + 5 + rel;
            if (dest != g_director) continue;
            const int64_t newRel = static_cast<int64_t>(reinterpret_cast<uintptr_t>(tramp)) -
                                   static_cast<int64_t>(reinterpret_cast<uintptr_t>(b + k) + 5);
            if (newRel < INT32_MIN || newRel > INT32_MAX) {
                BVR_LOG("camera: director call site %p out of reach of the stub", b + k);
                continue;
            }
            DWORD o = 0;
            if (!VirtualProtect(b + k + 1, 4, PAGE_EXECUTE_READWRITE, &o)) continue;
            *reinterpret_cast<int32_t*>(b + k + 1) = static_cast<int32_t>(newRel);
            VirtualProtect(b + k + 1, 4, o, &o);
            FlushInstructionCache(GetCurrentProcess(), b + k, 5);
            if (g_dirSites < 4) g_dirSite[g_dirSites] = reinterpret_cast<uintptr_t>(b + k);
            ++g_dirSites;
            BVR_LOG("camera: director call site %p re-pointed", b + k);
        }
    }
    if (!g_dirSites) BVR_LOG("camera: no `call director` site found - the restore is OFF (expect drift)");
}

uintptr_t function_begin(uintptr_t addr) {
    DWORD64 base = 0;
    auto rf = RtlLookupFunctionEntry(static_cast<DWORD64>(addr), &base, nullptr);
    for (int guard = 0; rf && guard < 8; ++guard) {
        const uint8_t* ui = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
        const uint8_t flags = ui[0] >> 3, count = ui[2];
        if (!(flags & 0x4)) return static_cast<uintptr_t>(base + rf->BeginAddress);
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(ui + 4 + ((count + 1) & ~1) * 2));
    }
    return 0;
}

// ---------------- composition (v1 inject, generalised to any pose) ----------------
struct Frame { V3 Rb, Ub, Fb, Rh, Fh, pos; float s; };

Frame base_frame(const float* m) {
    const V3 r0 = v(m[0], m[1], m[2]), r2 = v(m[8], m[9], m[10]);
    Frame f{};
    f.s = static_cast<float>(g_fwdSign);
    const V3 F = mul(r2, f.s);
    const V3 Wup = v(0, 1, 0);
    V3 Fh = sub(F, mul(Wup, dot(F, Wup)));
    if (len(Fh) < 1e-3f) Fh = g_lastFh; else Fh = norm(Fh);
    g_lastFh = Fh;
    V3 Rh = sub(r0, mul(Wup, dot(r0, Wup)));
    Rh = len(Rh) < 1e-3f ? norm(cross(Wup, Fh)) : norm(Rh);
    const float elev = asinf(fmaxf(-1.0f, fminf(1.0f, dot(F, Wup))));
    const float ke = elev * g_pitchKeep;
    f.Fb = norm(add(mul(Fh, cosf(ke)), mul(Wup, sinf(ke))));
    f.Ub = norm(cross(f.Fb, Rh));
    if (dot(f.Ub, Wup) < 0.0f) f.Ub = mul(f.Ub, -1.0f);
    f.Rb = Rh; f.Rh = Rh; f.Fh = Fh;
    f.pos = v(m[12], m[13], m[14]);
    return f;
}

// One XR pose (LOCAL space) -> engine camera rows + position, relative to the recentre reference. `lean` scales the
// translation (head lean for the head; the same frame for the eyes, so the eye offsets stay rigid with the head).
Cam compose(const Frame& f, const bvr::vr::HeadPose& p, float fovRad) {
    Q cur{p.qx, p.qy, p.qz, p.qw};
    const float h = -g_refYaw * 0.5f;
    const Q d = qmul(Q{0.0f, sinf(h), 0.0f, cosf(h)}, cur);
    const V3 lp = rotY(sub(v(p.px, p.py, p.pz), g_refPos), -g_refYaw);
    auto world = [&](V3 a) { return add(add(mul(f.Rb, a.x), mul(f.Ub, a.y)), mul(f.Fb, -a.z)); };
    const V3 nR = world(qrot(d, v(1, 0, 0)));
    const V3 nU = world(qrot(d, v(0, 1, 0)));
    const V3 nF = mul(world(qrot(d, v(0, 0, 1))), -1.0f);
    const V3 lean = add(add(mul(f.Rh, lp.x), mul(v(0, 1, 0), lp.y)), mul(f.Fh, -lp.z));
    const V3 np = add(f.pos, mul(lean, g_posScale));
    Cam c{};
    c.r[0][0] = nR.x; c.r[0][1] = nR.y; c.r[0][2] = nR.z;
    c.r[1][0] = nU.x; c.r[1][1] = nU.y; c.r[1][2] = nU.z;
    const V3 r2 = mul(nF, f.s);
    c.r[2][0] = r2.x; c.r[2][1] = r2.y; c.r[2][2] = r2.z;
    c.r[3][0] = np.x; c.r[3][1] = np.y; c.r[3][2] = np.z; c.r[3][3] = 1.0f;
    c.fov = fovRad;
    c.valid = true;
    return c;
}

// The eye poses with the separation scaled about the head (world scale).
bvr::vr::HeadPose scaled_eye(const bvr::vr::StereoPose& sp, int eye) {
    bvr::vr::HeadPose e = sp.eyes[eye];
    e.px = sp.head.px + (e.px - sp.head.px) * g_ipdScale;
    e.py = sp.head.py + (e.py - sp.head.py) * g_ipdScale;
    e.pz = sp.head.pz + (e.pz - sp.head.pz) * g_ipdScale;
    return e;
}

void do_recenter(const bvr::vr::HeadPose& hp) {
    g_refYaw = xr_heading(Q{hp.qx, hp.qy, hp.qz, hp.qw});
    g_refPos = v(hp.px, hp.py, hp.pz);
    g_refSet = true;
    BVR_LOG("camera: recentred (heading %.1f deg, position %.2f %.2f %.2f m)", g_refYaw * kRad2Deg, hp.px, hp.py, hp.pz);
}

// DS3'S FOV LAW (v1 CAM-004, HEADSET): +0x50 is a VERTICAL fov referenced to 16:9 - for pictures narrower than 16:9
// the game keeps the 16:9 horizontal extent. So write the fov that gives the wanted horizontal half-angle at 16:9.
float fov_for_half_h(float halfH) { return 2.0f * atanf(tanf(halfH) / (16.0f / 9.0f)); }

void inject(unsigned char* c, bool game) {
    float* m = reinterpret_cast<float*>(c + 0x10);
    if (game) {
        InterlockedIncrement64(&g_updates);
        memcpy(g_prist, c + 0x10, sizeof(g_prist));
        g_lastCamVt = *reinterpret_cast<const uintptr_t*>(c);
        g_lastCamMs = GetTickCount64();
    } else {
        memcpy(c + 0x10, g_prist, sizeof(g_prist));
    }
    g_lastCam = c;
    g_wrote = false;

    Cam cams[4]{};
    memcpy(cams[3].r, g_prist, sizeof(cams[3].r));
    cams[3].fov = *reinterpret_cast<const float*>(g_prist + 0x40);
    cams[3].valid = true;

    bvr::vr::StereoPose sp{};
    // The head moves the game camera only in VR-camera mode: on the flat screen in the room it would swing the picture.
    const bool live = bvr::vr::session_live() && bvr::vr::vr_camera_mode() && bvr::vr::get_stereo_pose(sp);
    if (live) {
        const uint32_t rc = bvr::vr::runtime_recenter_count();
        if (!g_refSet || rc != g_recenterSeen || InterlockedExchange(&g_recenterReq, 0)) {
            g_recenterSeen = rc;
            do_recenter(sp.head);
        }
    }
    if (!g_headOn || !live || !g_refSet) {
        if (game) {
            const LONG64 k = InterlockedIncrement64(&g_genN) - 1;
            g_genRing[k % kGenRing] = 0;
            publish(cams, 0);
        }
        return;
    }

    const Frame f = base_frame(m);

    // FOV: the mid-eye camera covers the headset's field (both eyes' union horizontally) - M4 gives each engine view its
    // own asymmetric lens; until then the flat/mono picture uses this.
    // FOVMATCH (P0 run 3, JJ: "moving my head around just shears and warps the image"): the runtime declares the picture
    // at its suggested horizontal FOV (the one that covers the headset's height at the picture's shape: 130.3 deg for
    // 2048x1152) while we rendered the headset's half-width (99 deg) - declared != rendered FOV (FAIL-STR-009). Now the
    // game draws exactly the suggested FOV and tells the runtime so (set_rendered_hfov).
    float fovW = *reinterpret_cast<const float*>(c + 0x50);
    const float hfovDeg = bvr::vr::suggested_hfov_deg();
    LensNow L;
    if (g_fovLock && compute_lens(L)) {
        // M4 LENS: the engine draws the lens's horizontal half-angle (FOVLAW); each eye is declared with exactly it.
        fovW = L.fov;
        bvr::vr::set_rendered_hfov(2.0f * atanf(L.hx) * kRad2Deg);
        for (int e = 0; e < 2; ++e) {
            const float fa[4] = {-atanf(L.hx), atanf(L.hx), atanf(L.vy), -atanf(L.vy)};
            bvr::vr::set_eye_lens(e, fa, L.q[e]);
        }
        g_cullFov = L.cullFov;
        g_cullAspect = L.cullAspect;
        g_lens = L;
        if (L.offAxis) {   // OFFAXIS: declared = rendered = each eye's exact angles, the eyes straight ahead
            const float q0[4] = {0, 0, 0, 1};
            bvr::vr::set_rendered_hfov(2.0f * atanf(0.5f * (tanf(L.ang[0][1]) - tanf(L.ang[0][0]))) * kRad2Deg);
            for (int e = 0; e < 2; ++e) bvr::vr::set_eye_lens(e, L.ang[e], q0);
        }
        offaxis::set_on(L.offAxis);
    } else if (g_fovLock && hfovDeg > 20.0f && hfovDeg < 170.0f) {
        if (bvr::vr::eye_lens_on()) bvr::vr::clear_eye_lens();
        const float halfH = 0.5f * hfovDeg / kRad2Deg;
        fovW = fov_for_half_h(halfH);
        bvr::vr::set_rendered_hfov(hfovDeg);
        uint32_t ew = 0, eh = 0;
        bvr::vr::recommended_eye_size(&ew, &eh);
        const float aspect = (ew && eh) ? static_cast<float>(ew) / static_cast<float>(eh) : 1.0f;
        g_cullAspect = (16.0f / 9.0f) * 1.05f;                         // the drawn picture is 16:9-referenced
        g_cullFov = 2.0f * atanf(1.05f * tanf(halfH) / (16.0f / 9.0f));
        (void)aspect;
    }

    cams[0] = compose(f, sp.head, fovW);
    // TURNED eyes: each eye camera points at the middle of its own view (the turn of its lens); positions are unchanged.
    cams[1] = compose(f, L.ok ? turned(scaled_eye(sp, 0), L.q[0]) : scaled_eye(sp, 0), fovW);
    cams[2] = compose(f, L.ok ? turned(scaled_eye(sp, 1), L.q[1]) : scaled_eye(sp, 1), fovW);

    // ChrCam gets the mid-eye head (the main view; M3 moves each view to its eye).
    memcpy(m, cams[0].r, sizeof(cams[0].r));
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = reinterpret_cast<const float*>(g_prist)[15];
    *reinterpret_cast<float*>(c + 0x50) = fovW;
    g_lastFovWritten = fovW;
    memcpy(g_written, c + 0x10, sizeof(g_written));
    g_wrote = true;
    InterlockedIncrement64(&g_injected);
    if (game) {
        const LONG64 k = InterlockedIncrement64(&g_genN) - 1;
        g_genRing[k % kGenRing] = sp.generation;
        publish(cams, sp.generation);
        // The runtime shows a head-tracked picture only while gameplay views keep arriving (else it falls back to the
        // cutscene screen - "cinematic quad ... stale=1", run 2). Stamped on every camera update we drive.
        bvr::vr::publish_gameplay_view(true);
    }
}

} // namespace

} // namespace bvr::ds3::camera

extern "C" void ds3_restore_camera(unsigned char* c) { bvr::ds3::camera::restore_pristine(c); }
extern "C" void ds3_after_director(unsigned char* c) { bvr::ds3::camera::inject(c, true); }

namespace bvr::ds3::camera {

bool install() {
    g_cull3K = settings::f("cull3_k", 1.0f);   // CULL3
    {   // V13d: the lens mode is a setting (`lens`, the same key eyesize sizes the picture with) - JJ keeps 0, the symmetric lens,
        // until the gravestone culler gets a frustum spanning both off-centre windows
        const int m = settings::i("lens", 1);
        g_lensMode = (m == 0 || m == 1 || m == 2) ? m : 1;
    }
    if (g_installed) return true;
    g_headOn = settings::i("head", 1) != 0;
    g_posScale = settings::f("posscale", 1.0f);
    g_pitchKeep = settings::f("pitchkeep", 1.0f);
    g_ipdScale = settings::f("ipdscale", 1.0f);
    g_fovLock = settings::i("fovlock", 1) != 0;
    g_fwdSign = settings::i("fwdsign", 1) >= 0 ? 1 : -1;
    g_dirMode = settings::i("dirhook", 1);
    g_lensMode = eyesize::lens_mode();

    int hits = 0;
    uintptr_t at = 0;
    if (!scan_text(addr::kCopyOutPattern, at, hits)) {
        snprintf(g_status, sizeof(g_status), "camera: copy-out pattern hits=%d (want 1) - NOT installed", hits);
        BVR_LOG("%s", g_status);
        return false;
    }
    g_outSite = at + 9;
    g_director = function_begin(g_outSite);
    // DS3 1.15.2: the director's first byte is E9 (an Arxan jump into the protected .text). Sekiro's plain prologue
    // is accepted too, so a future build that drops the stub still works.
    const uint8_t first = g_director ? *reinterpret_cast<const uint8_t*>(g_director) : 0;
    static const uint8_t kPro[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54};
    const bool arxan = first == 0xE9;
    const bool plain = g_director && memcmp(reinterpret_cast<void*>(g_director), kPro, sizeof(kPro)) == 0;
    if (!g_director || (!arxan && !plain)) {
        snprintf(g_status, sizeof(g_status), "camera: director begin %p unexpected (first byte %02X) - NOT installed",
                 reinterpret_cast<void*>(g_director), first);
        BVR_LOG("%s", g_status);
        return false;
    }
    // The copy-out site is only used to FIND the director - nothing is written there (Arxan guards it).
    if (!g_dirMode) {
        snprintf(g_status, sizeof(g_status), "camera: dirhook = 0, the seam is off (test only)");
        BVR_LOG("%s", g_status);
        return false;
    }
    repoint_director_calls();
    if (!g_dirSites) {
        snprintf(g_status, sizeof(g_status), "camera: no `call director` site found - NOT installed");
        BVR_LOG("%s", g_status);
        return false;
    }

    // Culling-camera builders (two hits).
    int found = 0;
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        static const uint8_t pat[] = {0xF3, 0x0F, 0x10, 0x5B, 0x54, 0xF3, 0x0F, 0x10, 0x53, 0x50, 0xBA, 0x01, 0x00, 0x00, 0x00};
        void* hk[2] = {reinterpret_cast<void*>(&hkCull<0>), reinterpret_cast<void*>(&hkCull<1>)};
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (memcmp(sec->Name, ".text", 5) != 0) continue;
            const uint8_t* bb = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
            for (size_t k = 0; k + sizeof(pat) <= sec->Misc.VirtualSize && found < 2; ++k) {
                if (memcmp(bb + k, pat, sizeof(pat)) != 0) continue;
                const uintptr_t fn = function_begin(reinterpret_cast<uintptr_t>(bb + k));
                if (fn && MH_CreateHook(reinterpret_cast<void*>(fn), hk[found], reinterpret_cast<void**>(&oCull[found])) ==
                              MH_OK)
                    MH_QueueEnableHook(reinterpret_cast<void*>(fn));
                BVR_LOG("camera: culling-camera builder %d at %p", found, reinterpret_cast<void*>(fn));
                ++found;
            }
        }
    }
    {   // CULL3: the main-link-only culler, checked by its prologue bytes before hooking
        static const uint8_t pro[] = {0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54, 0x41, 0x56, 0x49, 0x8D, 0xAB};
        uint8_t* fn = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + 0x522B0;
        bool ok = false;
        __try { ok = memcmp(fn, pro, sizeof(pro)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ok && MH_CreateHook(fn, reinterpret_cast<void*>(&hkCull3), reinterpret_cast<void**>(&oCull3)) == MH_OK) {
            MH_QueueEnableHook(fn);
            BVR_LOG("camera: CULL3 main-link culler hooked at %p (tangent x%.2f)", fn, g_cull3K);
        } else BVR_LOG("camera: CULL3 culler NOT hooked (bytes %s)", ok ? "ok, hook failed" : "differ - another build");
    }
    const MH_STATUS ap = MH_ApplyQueued(); // one freeze for all (HOOKBATCH, VR_ESSENTIALS section 10)
    g_installed = ap == MH_OK;
    // HOOKLIVE: the call sites we re-pointed are outside the Arxan guard - these bytes are read back by `ds camstat`.
    __try { memcpy(g_outAfterHook, reinterpret_cast<const void*>(g_dirSite[0]), 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const bool known = g_outSite == addr::rebase(addr::ChrCamCopyOutCall) && g_director == addr::rebase(addr::ChrCamDirector);
    snprintf(g_status, sizeof(g_status), "camera: %s copy-out=%p director=%p (%s), wrapped call sites %d, cull builders %d",
             g_installed ? "HOOKED" : "apply FAILED", reinterpret_cast<void*>(g_outSite), reinterpret_cast<void*>(g_director),
             known ? "1.15.2.0 addresses" : "UNKNOWN BUILD", g_dirSites, found);
    BVR_LOG("%s", g_status);
    return g_installed;
}

bool installed() { return g_installed; }
const char* status() { return g_status; }
uint64_t updates() { return static_cast<uint64_t>(g_updates); }

bool latest(int eye, Cam& out, uint32_t* generation) {
    const int slot = eye < 0 ? 0 : (eye == 0 ? 1 : 2);
    for (int tries = 0; tries < 8; ++tries) {
        const LONG s0 = g_seq;
        if (s0 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        out = g_pub[slot];
        const uint32_t gen = g_pubGen;
        MemoryBarrier();
        if (g_seq == s0) { if (generation) *generation = gen; return out.valid; }
    }
    return false;
}

bool game_camera(Cam& out) {
    for (int tries = 0; tries < 8; ++tries) {
        const LONG s0 = g_seq;
        if (s0 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        out = g_pub[3];
        MemoryBarrier();
        if (g_seq == s0) return out.valid;
    }
    return false;
}

bool generation_back(int back, uint32_t& gen) {
    const LONG64 n = g_genN;
    if (back < 0 || back >= kGenRing || n <= back) return false;
    gen = g_genRing[(n - 1 - back) % kGenRing];
    return gen != 0;
}

void recenter() { InterlockedExchange(&g_recenterReq, 1); }
void set_head(bool on) { g_headOn = on; settings::set_i("head", on ? 1 : 0); if (on) recenter(); }
bool head_on() { return g_headOn; }

int repose(unsigned long long notBefore) {
    unsigned char* c = g_lastCam;
    if (!g_installed || !c) return 0;
    int r = 1;
    if (notBefore && g_lastCamMs <= notBefore) r = -1;
    else {
        __try {
            if (*reinterpret_cast<const uintptr_t*>(c) != g_lastCamVt) r = -2;
            else inject(c, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) { r = -2; }
    }
    if (r < 0) InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_lastCam), nullptr, c);
    return r;
}

void draw_ui() {
    bool head = g_headOn;
    if (ImGui::Checkbox("Head tracking", &head)) set_head(head);
    if (ImGui::SliderFloat("world scale (eye distance)", &g_ipdScale, 0.5f, 2.0f, "%.2f"))
        settings::set_f("ipdscale", g_ipdScale);
    if (ImGui::SliderFloat("keep the game camera's tilt", &g_pitchKeep, 0.0f, 1.0f, "%.2f"))
        settings::set_f("pitchkeep", g_pitchKeep);
    if (ImGui::SliderFloat("head movement (lean) amount", &g_posScale, 0.0f, 2.0f, "%.2f"))
        settings::set_f("posscale", g_posScale);
}

bool handle_command(const char* cmd, const char* args) {
    if (_stricmp(cmd, "recenter") == 0) { recenter(); return true; }
    if (_stricmp(cmd, "head") == 0) { set_head(atoi(args) != 0); return true; }
    if (_stricmp(cmd, "fovlock") == 0) { g_fovLock = atoi(args) != 0; settings::set_i("fovlock", g_fovLock ? 1 : 0); return true; }
    if (_stricmp(cmd, "ipd") == 0) {   // live eye-separation multiplier, for bisecting what the separation changes
        g_ipdScale = static_cast<float>(atof(args));
        settings::set_f("ipdscale", g_ipdScale);
        BVR_LOG("camera: eye separation x%.2f", g_ipdScale);
        return true;
    }
    if (_stricmp(cmd, "fwdsign") == 0) { g_fwdSign = atoi(args) >= 0 ? 1 : -1; return true; }
    if (_stricmp(cmd, "lens") == 0) { // M4 test: 2 turned eyes, 0 one symmetric lens, -1 pre-M4 (live; the launch's size stays)
        g_lensMode = atoi(args);
        if (g_lensMode != 0 && g_lensMode != 1 && g_lensMode != 2) g_lensMode = -1;
        else settings::set_i("lens", g_lensMode);   // V13d: kept for the next launch (eyesize sizes the picture from it)
        g_lensShortLogged = false;
        BVR_LOG("lens: mode %d (%s)", g_lensMode, g_lensMode == 2 ? "turned eyes" : g_lensMode == 1 ? "true off-centre lens" :
                g_lensMode == 0 ? "one symmetric lens" : "pre-M4");
        return true;
    }
    if (_stricmp(cmd, "lensstat") == 0) {
        const LensNow L = g_lens;
        BVR_LOG("lens: mode %d ok %d picture %ux%u half-tangents %.3f x %.3f (%.1f x %.1f deg) engine fov %.4f cull fov %.4f aspect %.3f "
                "| L turn %.4f %.4f %.4f %.4f | R turn %.4f %.4f %.4f %.4f | %s", g_lensMode, L.ok ? 1 : 0, L.w, L.h, L.hx, L.vy,
                2 * atanf(L.hx) * kRad2Deg, 2 * atanf(L.vy) * kRad2Deg, L.fov, L.cullFov, L.cullAspect, L.q[0][0], L.q[0][1],
                L.q[0][2], L.q[0][3], L.q[1][0], L.q[1][1], L.q[1][2], L.q[1][3], eyesize::status());
        return true;
    }
    if (_stricmp(cmd, "cullany") == 0) {
        g_cullAny = atoi(args) != 0;
        BVR_LOG("cullany %d: other narrow cull sources widened %lld so far", g_cullAny ? 1 : 0, g_cullAnyWidened);
        return true;
    }
    if (_stricmp(cmd, "cull3") == 0) {   // "cull3 K": the main-link culler's frustum tangent x K (1 = the game's own)
        g_cull3K = static_cast<float>(atof(args));
        settings::set_f("cull3_k", g_cull3K);
        BVR_LOG("cull3 x%.2f: calls %lld, widened %lld", g_cull3K, g_cull3Calls, g_cull3Widened);
        return true;
    }
    if (_stricmp(cmd, "cullmargin") == 0) {
        g_cullMargin = static_cast<float>(atof(args));
        settings::set_f("cull_margin", g_cullMargin);
        BVR_LOG("cullmargin %.2f: the views cull %.0f %% wider than both eye windows", g_cullMargin, (g_cullMargin - 1.0f) * 100.0f);
        return true;
    }
    if (_stricmp(cmd, "cullright") == 0) {
        g_cullRight = atoi(args) != 0;
        BVR_LOG("cullright %d: %s", g_cullRight ? 1 : 0, g_cullRight ? "both eyes' cameras get the culling frustum around both windows"
                                                                    : "old exact match (left eye only)");
        return true;
    }
    if (_stricmp(cmd, "camstat") == 0) {
        BVR_LOG("camera: %s | updates %lld injected %lld | head %d fovlock %d ipd x%.2f", g_status, g_updates, g_injected,
                g_headOn ? 1 : 0, g_fovLock ? 1 : 0, g_ipdScale);
        // HOOKLIVE: which of our hooks actually run, and whether the game's code still holds our patch.
        uint8_t now8[8] = {};
        bool read = false;
        if (g_dirSite[0])
            __try { memcpy(now8, reinterpret_cast<const void*>(g_dirSite[0]), 8); read = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        BVR_LOG("camera: CULL3 main-link culler calls %lld, widened %lld (x%.2f) | other narrow cull sources widened %lld", g_cull3Calls,
                g_cull3Widened, g_cull3K, g_cullAnyWidened);
        BVR_LOG("camera: culling widened for the left-eye camera %lld, right-eye camera %lld (cullright %d)", g_cullWidened[0],
                g_cullWidened[1], g_cullRight ? 1 : 0);
        BVR_LOG("camera HOOKLIVE: camera writes %lld, director wrapper entered %lld, cull builders %lld | call-site "
                "bytes after patching %02X %02X %02X %02X %02X, now %02X %02X %02X %02X %02X (%s)",
                g_updates, g_dirCalls, g_cullCalls, g_outAfterHook[0], g_outAfterHook[1], g_outAfterHook[2],
                g_outAfterHook[3], g_outAfterHook[4], now8[0], now8[1], now8[2], now8[3], now8[4],
                !read ? "unreadable" : (memcmp(now8, g_outAfterHook, 5) == 0 ? "unchanged - our wrapper is still in place"
                                                                            : "CHANGED - the game's own code was put back"));
        return true;
    }
    return false;
}

} // namespace bvr::ds3::camera

// DS3VR2 native stereo - STEREOPROBE (read-only census) + STEREOVIEW-1 (second engine view, zero-difference control,
// off by default: settings stereo_view / panel TESTS / `ds view 1`) + EYEMAP (textures of each view). See stereo.h.
#include "stereo.h"

#include "addresses.h"
#include "camera.h"
#include "hwwatch.h"
#include "dlss.h"
#include "eyesize.h"
#include "markers.h"
#include "sunrays.h"
#include "offaxis.h"
#include "lightgrid.h"
#include "tilefix.h"
#include "afw.h"
#include "focus.h"
#include "picsplit.h"
#include "envtime.h"
#include "settings.h"
#include "shadowres.h"
#include "contact.h"
#include "loadscreen.h"

#include "core/gfx/blit.h"
#include "core/gfx/gfx_hud.h"
#include "core/gfx/hud_capture.h"
#include "core/util/log.h"
#include "core/vr/black_watch.h"
#include "core/vr/eye_burst.h"
#include "core/vr/openxr_runtime.h"

#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <d3d11.h>
#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <d3d11_1.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace bvr::ds3::stereo {
namespace {

const uintptr_t kBase = addr::kPreferredBase;
inline uintptr_t rva(const void* p) { return reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool g_on = true;
bool g_installed = false;

// ---- RTTI (MSVC x64): vtable[-1] -> CompleteObjectLocator {sig, off, cdOff, typeDesc RVA, classDesc RVA, self RVA} ----
const char* class_name(const void* obj, char* buf, size_t n) {
    strcpy_s(buf, n, "?");
    __try {
        if (!obj) { strcpy_s(buf, n, "null"); return buf; }
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        const uintptr_t col = *reinterpret_cast<const uintptr_t*>(vt - 8);
        const uint32_t* c = reinterpret_cast<const uint32_t*>(col);
        if (c[0] != 1) return buf;
        const uintptr_t img = col - c[5];                    // self RVA -> image base
        const char* name = reinterpret_cast<const char*>(img + c[3] + 0x10);
        size_t k = 0;
        for (; k + 1 < n && name[k] && k < 120; ++k) buf[k] = name[k];
        buf[k] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(buf, n, "<unreadable>"); }
    return buf;
}

uintptr_t vtable_of(const void* obj) {
    __try { return obj ? *reinterpret_cast<const uintptr_t*>(obj) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uint32_t dword_at(const void* obj, size_t off) {
    __try { return obj ? *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(obj) + off) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xDEADDEAD; }
}

// ---- view list snapshot ----
struct ListSnap { void* list = nullptr; int n = 0; void* views[16] = {}; };
bool read_list(void* list, ListSnap& out) {
    out = ListSnap{};
    out.list = list;
    __try {
        void** b = *reinterpret_cast<void***>(static_cast<uint8_t*>(list) + addr::kListBegin);
        void** e = *reinterpret_cast<void***>(static_cast<uint8_t*>(list) + addr::kListEnd);
        const intptr_t n = e - b;
        if (n < 0 || n > 64) return false;
        out.n = static_cast<int>(n);
        for (int i = 0; i < out.n && i < 16; ++i) out.views[i] = b[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void log_list(const char* why, void* list) {
    ListSnap s;
    if (!read_list(list, s)) { BVR_LOG("views [%s]: list %p unreadable", why, list); return; }
    BVR_LOG("views [%s]: list %p count %d", why, list, s.n);
    for (int i = 0; i < s.n && i < 16; ++i) {
        char nm[128];
        BVR_LOG("views   [%d] %p vt rva 0x%llX %s prio(+0x10)=0x%08X ref(+8)=%u", i, s.views[i],
                static_cast<unsigned long long>(vtable_of(s.views[i]) ? vtable_of(s.views[i]) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) : 0),
                class_name(s.views[i], nm, sizeof(nm)), dword_at(s.views[i], 0x10), dword_at(s.views[i], 0x08));
    }
}

// ---- counters ----
std::atomic<uint32_t> g_rallCalls{0}, g_rallThisPresent{0}, g_rallTid{0};
std::atomic<int> g_lastCount{-1};
std::atomic<void*> g_lastList{nullptr};
std::atomic<uint64_t> g_rallTicks{0};
uint32_t g_multiFrames = 0, g_presents = 0, g_maxPerPresent = 0;
std::atomic<uint32_t> g_adds{0}, g_removes{0}, g_ctors{0};

// The main view's construction (OWNVIEW input). RendMan init builds the desc as 0x58 bytes on its stack (RE_NOTES 4).
constexpr size_t kDescBytes = 0x60;
struct CtorRec { void* ctxPtr = nullptr; uint64_t ctxVal = 0; uint8_t desc[kDescBytes] = {}; void* result = nullptr; uintptr_t callerRva = 0; };
CtorRec g_mainCtor;
bool g_haveMainCtor = false;
std::atomic<void*> g_mainView{nullptr};       // the game's own view (GXSgCompositeScene), while listed

// ---- OWNVIEW (M3 step 3, build STEREOVIEW-1): a second engine view built the way RendMan init builds the main one ----
// Step A (this build) = the ZERO-DIFFERENCE control (playbook RE-08 / 17 #side-effect-gate capture B): our view gets the
// main view's own scene (+0x08), camera link (+0x28) and +0x30 object, read from the main view's fields +0x40 / +0x48 /
// +0xE0 (the setters store them there). Gate (AC6 LISTGATE): listed only while the engine's list is exactly [main] (+ ours)
// and main has been listed 2 s; removed with main (fast travel rebuilds it) and on any other list.
// STEREOVIEW-2 (JJ run 2026-10-28 23:59: off then on 0.6 s later = heap corruption + a hang): the view's destructor frees
// the camera link it was given (AC6 noted the same: "the view destructor frees its camera") - the MAIN view's link. So:
//   - our view is built ONCE and never destroyed; off / gate = only taken out of the list, on = relinked and listed again;
//   - it gets its OWN camera link (DS3: 0x120 bytes, no vtable, two 0x90 camera records - RendMan init 0x140DFF04C),
//     refreshed from the main view's link every frame before render-all, then (test) shifted;
//   - no SEH around engine calls: a fault inside the engine's allocator held its lock and hung the game (a crash with a
//     dump is better than a hang).
std::atomic<bool> g_wantView{false};
void* g_ownHolder = nullptr;                   // our reference (engine ref-counted assign 0x140FBF870), never released
void* g_ownObj = nullptr;                      // the built view (kept for the session)
std::atomic<void*> g_ownView{nullptr};         // non-null while LISTED
uint8_t* g_ownLink = nullptr;                  // our camera link (16-aligned, kCamLinkBytes)
std::atomic<bool> g_ownCam{true};              // our own camera link (false = share main's - the run-1 control)
std::atomic<float> g_shiftM{0.0f};             // TEST: move our view's camera along its right row by this many metres
std::atomic<int> g_linkPosOff{0x30};           // where the link's position row is (to confirm from CAMDUMP)
std::atomic<bool> g_poisoned{false};
ULONGLONG g_mainSinceMs = 0;
std::atomic<uint32_t> g_ownBuilds{0}, g_ownRemovals{0}, g_framesTwo{0}, g_ownListings{0};
using AssignFn = void**(__fastcall*)(void** holder, void* obj);
using VFn1 = void(__fastcall*)(void* self, void* arg);
using VGet = void*(__fastcall*)(void* self);
inline void** vt(void* o) { return *reinterpret_cast<void***>(o); }
inline void* field(void* o, size_t off) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(o) + off); }

// ---- hooks ----
using RenderAllFn = void(__fastcall*)(void* list, void* ctx);
using AddViewFn = uint64_t(__fastcall*)(void* list, void* view, void* a3, void* a4);
using RemoveViewFn = uint64_t(__fastcall*)(void* list, void* view);
using SceneCtorFn = void*(__fastcall*)(void* ctx, void* desc);
using RendManDrawFn = void(__fastcall*)(void* rendMan);
void hdr_swap_now();   // HDRSWAP probe, defined with the DRAWTWICE block below
RenderAllFn oRenderAll = nullptr;
RendManDrawFn oRendManDraw = nullptr;
AddViewFn oAddView = nullptr;
RemoveViewFn oRemoveView = nullptr;
SceneCtorFn oSceneCtor = nullptr;

// ---- STEREO3D (build STEREOVIEW-3): the main view = LEFT eye, ours = RIGHT eye ----
// Camera link layout (CAMDUMP 2026-10-29, LIVE): record 0 = this frame, record 1 at +0x90 = the previous frame (motion
// vectors); each record = 3 direction rows (+0x00/+0x10/+0x20) + position row +0x30 (w 1), then +0x50 fov (rad, the
// value ChrCam got), +0x54 near 0.08, +0x58 far 10000, +0x64 aspect. The eyes are moved by translation only (VD's eye views
// are parallel): the world offset of each eye from the head comes from camera::latest (the same composed frame as ChrCam).
std::atomic<bool> g_stereo{false};
std::atomic<int> g_eyeSep{1};
// AFW (V7, afw.h): the eye the game's view draws this frame, and what the warp needs about the other eye. Render thread.
struct AfwFrame { int eye = -1; float src[3] = {}, dst[3] = {}; bool parallel = true; };
AfwFrame g_afwFrame, g_afwPresent;        // set before render-all / taken at the Present that shows it
int g_afwNext = 0;
float g_afwFovBase[2] = {}, g_afwFovWrit[2] = {};
bool g_afwFovSet = false;
ID3D11Resource* g_afwDepth = nullptr;     // this Present's depth copy (eye_capture -> eye_synth)
unsigned g_afwRW = 0, g_afwRH = 0;
std::atomic<uint32_t> g_afwNoCam{0}, g_afwTurned{0};                 // TEST: 0 = both views at the head (zero-difference control, `ds eyesep 0`); 2 = each eye TURNED but at the head (pure rotation: exact eye comparisons)
float g_mainOrig[16] = {}, g_mainWrit[16] = {};  // the main link's rows + position before / after our eye write
bool g_mainShifted = false;
std::atomic<bool> g_lensShiftOn{false};           // M4 test LENSSHIFT: candidate engine lens-shift floats (link +0x6C/+0x70)
std::atomic<float> g_lensShift[2][2] = {};        // [eye][x, y]
std::atomic<uint32_t> g_stereoFrames{0};

// RTCOPY (AC6 SCENEPARAMS: "without the copy ours used the default sets: darker, no teal haze"; JJ 2026-10-29: "the
// second view is darker in the distance ... different shadows"): byte ranges of the main view's render-target set copied
// into ours every frame before render-all. Found with RTDIFF; set live with `ds rtcopy 0xA-0xB,...` / `auto` / `off`.
struct Range { uint32_t off, len; };
Range g_copy[1024]; std::atomic<int> g_copyN{0};
// RTPRIME (L1-3b, LIVE (sim) 2026-10-29): the copy only has to PRIME our settings object - once its values (and its
// arrays' headers) are right the engine maintains them itself, proven by switching the copy off and reading both
// objects four minutes later. So the shipped fix copies for a few frames at listing, not every frame. `ds rtcopy` /
// `ds rtclass apply` still copy every frame (tests).
std::atomic<int> g_primeFrames{0};
std::atomic<bool> g_copyAlways{false};
Range g_diff[256]; int g_diffN = 0;

// RTCLASS (L1 2026-10-29): instead of copying every differing byte (which made the eye DARKER), every 8-byte field of the
// two settings objects is watched for a few seconds WHILE THE CAMERA MOVES and put in a class:
//   P  either side looks like a pointer                    -> never copied (ACOVR P13: records point at their own view's data)
//   C  differs and the engine rewrites it for OUR view too -> per-view (camera, matrices, timers) -> never copied
//   A  differs, neither side changes                       -> an area setting our view never got   -> copy
//   B  differs, only the main view's changes               -> engine state our view never gets      -> copy
// Moving the camera is what makes the test valid: it puts every camera-derived field into class C.
constexpr uint32_t kRtSize = addr::kRtSize;   // DS3: 0x4170, allocated by 0x1400C3D90 inside the view ctor
constexpr uint32_t kRtQ = kRtSize / 8;
std::atomic<int> g_classState{0};       // 0 idle, 1 sampling, 2 classified
std::atomic<uint32_t> g_classWant{0}, g_classGot{0};
std::vector<uint64_t> g_firstM, g_firstO;
std::vector<uint8_t> g_chgM, g_chgO;
int g_classCount[4] = {};               // A, B, C, P
void rtclass_sample(void* mainView, void* ownView);
void rtclass_finish();

void copy_rt_params(void* mainView, void* ownView) {
    const int n = g_copyN.load();
    if (!n || !mainView || !ownView) return;
    const int prime = g_primeFrames.load();
    if (!g_copyAlways.load() && prime <= 0) return;
    uint8_t* src = static_cast<uint8_t*>(reinterpret_cast<VGet>(vt(mainView)[3])(mainView));
    uint8_t* dst = static_cast<uint8_t*>(reinterpret_cast<VGet>(vt(ownView)[3])(ownView));
    if (!src || !dst) return;
    for (int i = 0; i < n; ++i) memcpy(dst + g_copy[i].off, src + g_copy[i].off, g_copy[i].len);
    if (prime > 0 && g_primeFrames.fetch_sub(1) == 1)
        BVR_LOG("rtprime: our view's settings primed from the main view's (%d ranges); the engine maintains them from here", n);
}

// Each eye = the link moved by the eye's offset from the head and turned by the eye's rotation relative to the head (M4
// TURNED eyes; identity when the eyes are parallel). Relative, so a link the game set up itself keeps its own aim:
// new row = sum_i (row . head_i) eye_i.
void apply_eye(float* rec, const camera::Cam& h, const camera::Cam& e) {
    float out[3][3];
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) {
            float acc = 0.0f;
            for (int i = 0; i < 3; ++i) {
                const float d = rec[k * 4 + 0] * h.r[i][0] + rec[k * 4 + 1] * h.r[i][1] + rec[k * 4 + 2] * h.r[i][2];
                acc += d * e.r[i][j];
            }
            out[k][j] = acc;
        }
    for (int k = 0; k < 3; ++k) for (int j = 0; j < 3; ++j) rec[k * 4 + j] = out[k][j];
    if (g_eyeSep.load() != 2) for (int i = 0; i < 3; ++i) rec[12 + i] += e.r[3][i] - h.r[3][i];
}

// Refresh our camera link from the main view's (render thread, before render-all), then the eye offsets / test shift.
void refresh_link(void* mainView) {
    if (!g_ownLink || !mainView || !g_ownCam.load()) return;
    uint8_t* mainLink = static_cast<uint8_t*>(field(mainView, 0x48));
    if (!mainLink) return;
    float* mrec = reinterpret_cast<float*>(mainLink);      // rows +0x00/+0x10/+0x20, position +0x30
    // The engine may not rewrite the link every frame: if it still holds what we wrote, put its own values back first.
    if (g_mainShifted && memcmp(mrec, g_mainWrit, sizeof(g_mainWrit)) == 0) memcpy(mrec, g_mainOrig, sizeof(g_mainOrig));
    g_mainShifted = false;
    memcpy(g_ownLink, mainLink, addr::kCamLinkBytes);
    float* orec = reinterpret_cast<float*>(g_ownLink);
    // OFFAXIS: tell the eyes apart in the engine's camera copies - our (right-eye) link's fov, in both of its 0x90 camera
    // records, 0.001 rad above the game's (left-eye) value. The hooks match within 0.0004: the engine's own copies drift
    // by a few float steps (an exact match missed ~7000 builds a second), and the eye's projection is rebuilt from its
    // window anyway, so the offset itself is never seen.
    if (offaxis::on()) {
        const float left = *reinterpret_cast<const float*>(mainLink + 0x50);
        *reinterpret_cast<float*>(g_ownLink + 0x50) = left + 0.001f;
        *reinterpret_cast<float*>(g_ownLink + 0x90 + 0x50) = *reinterpret_cast<const float*>(mainLink + 0x90 + 0x50) + 0.001f;
        offaxis::set_tags(left, left + 0.001f);
    }
    if (g_stereo.load() && g_eyeSep.load()) {
        camera::Cam h{}, l{}, r{};
        if (camera::latest(-1, h) && camera::latest(0, l) && camera::latest(1, r)) {
            memcpy(g_mainOrig, mrec, sizeof(g_mainOrig));
            // Each eye = the link moved by the eye's offset from the head and turned by the eye's rotation relative to the
            // head (M4 TURNED eyes; identity when the eyes are parallel). Relative, so a link the game set up itself keeps
            // its own aim: new row = sum_i (row . head_i) eye_i.
            apply_eye(mrec, h, l);
            apply_eye(orec, h, r);
            picsplit::set_eye_pos(0, mrec);   // SHADOWX: tells the game view's scene-buffer write from ours
            picsplit::set_eye_pos(1, orec);
            offaxis::set_eye_link(0, mrec);   // MIRRORFLIP: the reflection camera is told from the eye's own by geometry
            offaxis::set_eye_link(1, orec);
            memcpy(g_mainWrit, mrec, sizeof(g_mainWrit));
            g_mainShifted = true;
            g_stereoFrames.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        const float s = g_shiftM.load();
        if (s != 0.0f) {
            const float* row0 = reinterpret_cast<const float*>(g_ownLink); // right row (CAMDUMP)
            for (int i = 0; i < 3; ++i) orec[12 + i] += row0[i] * s;
        }
    }
    // LENSSHIFT (M4 test): AC6's camera packet carries a lens shift beside its aspect (ac6vr HANDOVER "+0x98/+0x9c"); Dark Souls III's
    // link has two zero floats right after its aspect (+0x6C/+0x70) that the camera command copies along (0x1402F5CE0).
    // Written into OUR view's link only, so the selfcheck's eye shift shows whether the picture moves.
    if (g_lensShiftOn.load()) {
        *reinterpret_cast<float*>(g_ownLink + 0x6C) = g_lensShift[1][0].load();
        *reinterpret_cast<float*>(g_ownLink + 0x70) = g_lensShift[1][1].load();
    }
}

// AFW (V7): no second view - the game's own view is moved to THIS frame's eye (left, right, left ...). The other eye's
// link is computed the same way (not written anywhere): its position is where the warp moves the picture to.
void afw_link(void* mainView) {
    g_afwFrame.eye = -1;
    uint8_t* mainLink = mainView ? static_cast<uint8_t*>(field(mainView, 0x48)) : nullptr;
    if (!mainLink) return;
    float* mrec = reinterpret_cast<float*>(mainLink);
    if (g_mainShifted && memcmp(mrec, g_mainWrit, sizeof(g_mainWrit)) == 0) memcpy(mrec, g_mainOrig, sizeof(g_mainOrig));
    g_mainShifted = false;
    float* fov1 = reinterpret_cast<float*>(mainLink + 0x50);
    float* fov2 = reinterpret_cast<float*>(mainLink + 0x90 + 0x50);
    if (g_afwFovSet && *fov1 == g_afwFovWrit[0] && *fov2 == g_afwFovWrit[1]) { *fov1 = g_afwFovBase[0]; *fov2 = g_afwFovBase[1]; }
    g_afwFovSet = false;
    camera::Cam h{}, l{}, r{};
    if (!camera::latest(-1, h) || !camera::latest(0, l) || !camera::latest(1, r)) { g_afwNoCam.fetch_add(1); return; }
    const int e = g_afwNext;
    const camera::Cam& me = e ? r : l;
    const camera::Cam& other = e ? l : r;
    memcpy(g_mainOrig, mrec, sizeof(g_mainOrig));
    float orec[16];
    memcpy(orec, mrec, sizeof(orec));
    apply_eye(mrec, h, me);
    apply_eye(orec, h, other);
    memcpy(g_mainWrit, mrec, sizeof(g_mainWrit));
    g_mainShifted = true;
    // OFFAXIS: the view's camera copies carry the drawn eye's tag (left = the game's fov, right = + 0.001)
    if (offaxis::on()) {
        const float base1 = *fov1, base2 = *fov2;
        offaxis::set_tags(base1, base1 + 0.001f);
        if (e == 1) {
            *fov1 = base1 + 0.001f; *fov2 = base2 + 0.001f;
            g_afwFovBase[0] = base1; g_afwFovBase[1] = base2; g_afwFovWrit[0] = *fov1; g_afwFovWrit[1] = *fov2;
            g_afwFovSet = true;
        }
    }
    picsplit::set_eye_pos(0, mrec);   // the view's camera writes count as view 0 (VP capture, SHADOWX)
    picsplit::set_eye_pos(1, orec);
    offaxis::set_eye_link(e, mrec);
    offaxis::set_eye_link(e ^ 1, orec);
    AfwFrame f;
    f.eye = e;
    for (int i = 0; i < 3; ++i) { f.src[i] = mrec[12 + i]; f.dst[i] = orec[12 + i]; }
    // the warp's destination camera = the drawn one moved, which is exact only while both eyes look the same way
    float d = 0.0f;
    for (int i = 0; i < 3; ++i) d += l.r[2][i] * r.r[2][i];
    f.parallel = d > 0.99999f;
    if (!f.parallel) g_afwTurned.fetch_add(1, std::memory_order_relaxed);
    g_afwFrame = f;
    g_afwNext = e ^ 1;
    afw::note_drawn(e);
}

// CULLCAMSCAN (V13h, JJ: gravestones / candles / brick pop at the right eye's outer edge; it is culled with the LEFT eye's
// window - lens 0 removes it, no plane widening helps; the GXSR draw plan has separate TargetCamera and CullingCamera
// commands): read-only scan of our view and its sub-objects (two levels, 0x600 bytes each) for pointers that still hold the
// MAIN view's camera link or the main view itself - the left eye's camera leaking into our culling.
// CULLCAMFIX (V13i): CULLCAMSCAN 2026-10-31 21:4x found ONE pointer to the MAIN link inside our view: [ourView+0xF8]+0x4D0
// (our view's +0x48 is our link; the main view has no such path). Our sub-object's culling camera stayed the left eye's.
// Each frame, after our view is linked: if it still holds the main link, it gets our link. `ds cullcamfix 0|1`.
std::atomic<int> g_cullCamFix{0};   // OFF (JJ 21:4x: world broken after V13i)
std::atomic<uint32_t> g_cullCamFixed{0};
bool write_ptr_if(void* at, uintptr_t expect, uintptr_t value) {
    __try {
        uintptr_t* p = static_cast<uintptr_t*>(at);
        if (*p != expect) return false;
        *p = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool read_ptr(const void* at, uintptr_t& out) {
    __try { out = *static_cast<const uintptr_t*>(at); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// V13k: EVERY frame (the V13i one-time swap at listing never fired - the engine fills [+0xF8]+0x4D0 later). On: main link ->
// ours. Off: ours -> main link (undone at once). Only ever exchanges those two exact values.
std::atomic<uint32_t> g_cullCamRestored{0};
void cullcam_apply(void* own, void* mainView) {
    if (!own || !mainView || !g_ownLink || !g_ownCam.load()) return;
    const uintptr_t mainLink = reinterpret_cast<uintptr_t>(field(mainView, 0x48)), ours = reinterpret_cast<uintptr_t>(g_ownLink);
    uintptr_t sub = 0;
    if (!read_ptr(static_cast<uint8_t*>(own) + 0xF8, sub) || sub < 0x10000 || (sub & 7)) return;
    void* at = reinterpret_cast<uint8_t*>(sub) + 0x4D0;
    if (g_cullCamFix.load(std::memory_order_relaxed)) {
        if (write_ptr_if(at, mainLink, ours) && g_cullCamFixed.fetch_add(1) == 0)
            BVR_LOG("cullcamfix: our view's culling camera ([+0xF8]+0x4D0) moved from the main link %p to ours %p", (void*)mainLink, (void*)ours);
    } else if (write_ptr_if(at, ours, mainLink)) {
        g_cullCamRestored.fetch_add(1, std::memory_order_relaxed);
    }
}
void cullcam_scan() {
    void* mv = g_mainView.load(); void* ov = g_ownView.load();
    if (!mv || !ov) { BVR_LOG("cullcamscan: views not listed"); return; }
    const uintptr_t mainLink = reinterpret_cast<uintptr_t>(field(mv, 0x48)), ownLink = reinterpret_cast<uintptr_t>(g_ownLink);
    const uintptr_t targets[3] = {mainLink, reinterpret_cast<uintptr_t>(mv), ownLink};
    const char* names[3] = {"MAIN link", "MAIN view", "our link"};
    int hits = 0;
    for (int which = 0; which < 2; ++which) {   // our view, then the main view for comparison
        const uint8_t* root = static_cast<const uint8_t*>(which ? mv : ov);
        for (uint32_t o1 = 0; o1 < 0x600 && hits < 120; o1 += 8) {
            uintptr_t v1 = 0;
            if (!read_ptr(root + o1, v1)) break;
            for (int t = 0; t < 3; ++t)
                if (v1 == targets[t]) { ++hits; BVR_LOG("cullcamscan: %s view +0x%X = %s", which ? "MAIN" : "OUR", o1, names[t]); }
            if (v1 < 0x10000 || v1 == reinterpret_cast<uintptr_t>(root) || (v1 & 7)) continue;
            for (uint32_t o2 = 0; o2 < 0x600 && hits < 120; o2 += 8) {
                uintptr_t v2 = 0;
                if (!read_ptr(reinterpret_cast<const uint8_t*>(v1) + o2, v2)) break;
                for (int t = 0; t < 3; ++t)
                    if (v2 == targets[t]) { ++hits; BVR_LOG("cullcamscan: %s view +0x%X -> +0x%X = %s", which ? "MAIN" : "OUR", o1, o2, names[t]); }
            }
        }
    }
    BVR_LOG("cullcamscan: done - main link %p, main view %p, our link %p, our view %p (%d hits)", (void*)mainLink, mv, (void*)ownLink, ov, hits);
}

// (Re)link our view to the CURRENT main view's scene / camera / +0x30 and list it. Render thread, before render-all.
bool list_own(void* list, void* mainView) {
    if (g_poisoned.load()) return false;
    if (!g_ownObj) {
        uint64_t ctxHolder = g_mainCtor.ctxVal;                // *ctx = the GXSgSceneFactory vtable RendMan init passes
        uint8_t desc[kDescBytes];
        memcpy(desc, g_mainCtor.desc, sizeof(desc));
        shadowres::own_view_build(true);    // SHADOWRES: our view's (empty, unread) shadow maps keep the game's size
        void* v = oSceneCtor(&ctxHolder, desc);
        shadowres::own_view_build(false);
        if (!v) { BVR_LOG("ownview: the engine's view constructor returned null"); g_poisoned.store(true); return false; }
        reinterpret_cast<AssignFn>(addr::rebase(addr::ViewHolderAssign))(&g_ownHolder, v); // our reference - kept for the session
        g_ownObj = v;
        g_ownBuilds.fetch_add(1);
        if (!g_ownLink) {
            g_ownLink = static_cast<uint8_t*>(_aligned_malloc(addr::kCamLinkBytes, 16));
            if (g_ownLink) memset(g_ownLink, 0, addr::kCamLinkBytes);
        }
    }
    void* v = g_ownObj;
    void* scene = field(mainView, addr::kViewScene);
    void* camLink = field(mainView, addr::kViewCamLink);
    void* e0 = field(mainView, addr::kViewExtra);
    refresh_link(mainView);
    void* useLink = (g_ownCam.load() && g_ownLink) ? static_cast<void*>(g_ownLink) : camLink;
    reinterpret_cast<VFn1>(vt(v)[addr::kViewVfSetScene])(v, scene);   // +0x08 (ref-counted)
    reinterpret_cast<VFn1>(vt(v)[addr::kViewVfSetCam])(v, useLink);   // +0x28 camera link (forwarded to its sub-scenes)
    reinterpret_cast<VFn1>(vt(v)[addr::kViewVfSetExtra])(v, e0);      // +0x30
    oAddView(list, v, nullptr, nullptr);
    uint8_t* rt = static_cast<uint8_t*>(reinterpret_cast<VGet>(vt(v)[addr::kViewVfGetRt])(v)); // its render-target set
    if (rt) {
        // Exactly the five fields RendMan init writes for the main view (DS3 0x140DFF43D..0x140DFF48E, 0x140DFF55F).
        *reinterpret_cast<uint32_t*>(rt + addr::kRtWindowFlag) = 1;
        rt[addr::kRtFlagA] = 1;
        *reinterpret_cast<uint32_t*>(rt + addr::kRtClearColour) = 0xFF000000u;
        rt[addr::kRtFlagB] = 1;
        if (rt[addr::kRtCondByte] & addr::kRtCondBit) rt[addr::kRtCondTarget] = 0;
        if (void* sub = field(rt, addr::kRtSubObject))
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(sub) + 0x6C) = 0xFF000000u;
    }
    g_ownView.store(v);
    if (g_copyN.load() && !g_copyAlways.load()) g_primeFrames.store(30);   // RTPRIME: also after travel relists us
    g_ownListings.fetch_add(1);
    char nm[128];
    BVR_LOG("ownview: SECOND VIEW listed %p %s (rt set %p) - scene %p, camera link %s %p, +0x30 %p (main %p)", v,
            class_name(v, nm, sizeof(nm)), rt, scene, useLink == camLink ? "SHARED" : "OWN", useLink, e0, mainView);
    return true;
}

// Put the main view's camera back where the engine left it (if the engine has not rewritten it since).
void unshift_main() {
    void* m = g_mainView.load();
    if (!g_mainShifted || !m) { g_mainShifted = false; return; }
    float* mrec = reinterpret_cast<float*>(static_cast<uint8_t*>(field(m, 0x48)));
    if (mrec && memcmp(mrec, g_mainWrit, sizeof(g_mainWrit)) == 0) memcpy(mrec, g_mainOrig, sizeof(g_mainOrig));
    g_mainShifted = false;
}

void remove_own(void* list, const char* why) {
    unshift_main();
    void* v = g_ownView.exchange(nullptr);
    if (!v) return;
    if (list) oRemoveView(list, v);   // only out of the list: the view (and its link) stay alive for the session
    g_ownRemovals.fetch_add(1);
    BVR_LOG("ownview: second view taken out of the list (%s)", why);
}

// CAMDUMP: the main view's camera link as floats beside our own head camera, to confirm the link layout.
void camdump(const char* why) {
    void* m = g_mainView.load();
    if (!m) return;
    const float* f = static_cast<const float*>(field(m, 0x48));
    if (!f) return;
    char line[1600]; int n = sprintf_s(line, "camdump [%s] main link %p:", why, f);
    for (int i = 0; i < (int)addr::kCamLinkBytes / 4 && n < 1500; ++i) n += sprintf_s(line + n, sizeof(line) - n, "%s%.4g", i % 4 ? " " : " | ", f[i]);
    BVR_LOG("%s", line);
}
// MENUVIEW (2026-11-01, JJ: Status "doesn't revert back to 3D", System does after a couple of seconds). AUTOREC log 01:01:51:
// opening those menus makes the game ADD views from rva 0xD64B70 (the character preview); the exactly-[main] rule (an AC6
// lesson) then took our second view out = the room screen. System removes its preview again, Status keeps it. Views added
// from that call site are menu previews: they no longer count against our view. `ds menuview 0|1` (settings menu_view).
constexpr uintptr_t kMenuViewAdd = 0xD64B70;
std::atomic<int> g_menuViewOk{1};
std::atomic<void*> g_menuViews[8] = {};
bool is_menu_view(void* v) { for (auto& m : g_menuViews) if (v && m.load(std::memory_order_relaxed) == v) return true; return false; }
void gate_own(void* list, const ListSnap& s);

void gate_own(void* list, const ListSnap& s) {
    void* main = g_mainView.load();
    void* own = g_ownView.load();
    const ULONGLONG now = GetTickCount64();
    int menus = 0;   // MENUVIEW: menu-preview views do not count
    const bool menuOk = g_menuViewOk.load(std::memory_order_relaxed) != 0;
    for (int i = 0; i < s.n && i < 16; ++i) if (menuOk && is_menu_view(s.views[i])) ++menus;
    bool onlyMain = main && s.n - menus >= 1 && s.n - menus <= 2;
    for (int i = 0; i < s.n && i < 16; ++i)
        if (s.views[i] != main && s.views[i] != own && !(menuOk && is_menu_view(s.views[i]))) onlyMain = false;
    if (!onlyMain || !main) g_mainSinceMs = 0;
    else if (!g_mainSinceMs) g_mainSinceMs = now;
    if (afw::on() && g_stereo.load()) {   // AFW: only the game's view, moved to this frame's eye
        if (own) { remove_own(list, "AFW: one eye drawn per frame"); return; }
        if (main && s.n == 1 && s.views[0] == main && g_mainSinceMs && now - g_mainSinceMs >= 2000) afw_link(main);
        else g_afwFrame.eye = -1;
        return;
    }
    g_afwFrame.eye = -1;
    if (g_afwFovSet && main) {   // AFW just switched off: the game's own fov back before anything else reads it
        uint8_t* ml = static_cast<uint8_t*>(field(main, 0x48));
        if (ml) {
            float* f1 = reinterpret_cast<float*>(ml + 0x50); float* f2 = reinterpret_cast<float*>(ml + 0x90 + 0x50);
            if (*f1 == g_afwFovWrit[0] && *f2 == g_afwFovWrit[1]) { *f1 = g_afwFovBase[0]; *f2 = g_afwFovBase[1]; }
        }
        g_afwFovSet = false;
        unshift_main();
    }
    if (own && (!g_wantView.load() || !onlyMain)) { remove_own(list, !g_wantView.load() ? "switched off" : "the list is not [main]"); return; }
    if (own) { refresh_link(main); rtclass_sample(main, own); copy_rt_params(main, own); cullcam_apply(own, main); return; }
    if (g_wantView.load() && onlyMain && g_haveMainCtor && now - g_mainSinceMs >= 2000) list_own(list, main);
}

// TWODRAW (2026-10-29): instead of adding a second view and hand-feeding it everything the engine fills in for its own
// view (fog, shadows, cascades...), draw the GAME'S OWN view twice, with the camera moved to each eye between the two
// draws - the recipe that worked in Batman Arkham City, BioShock Infinite and Mass Effect LE (VR_ESSENTIALS §2). Both
// eyes then share every setting by construction. Our second view must NOT be listed while this runs.
std::atomic<bool> g_twoDraw{false};
std::atomic<uint32_t> g_twoDrawFrames{0};
float g_twoSaved[3] = {};
bool g_twoShifted = false;

// Move the main view's camera link to eye `e` (0 left, 1 right), from the same composed frame as the eye cameras.
bool shift_main_to_eye(void* mainView, int e) {
    uint8_t* link = static_cast<uint8_t*>(field(mainView, 0x48));
    if (!link) return false;
    float* pos = reinterpret_cast<float*>(link + 0x30);
    if (!g_twoShifted) { for (int i = 0; i < 3; ++i) g_twoSaved[i] = pos[i]; g_twoShifted = true; }
    camera::Cam h{}, c{};
    if (!camera::latest(-1, h) || !camera::latest(e, c)) return false;
    for (int i = 0; i < 3; ++i) pos[i] = g_twoSaved[i] + (c.r[3][i] - h.r[3][i]);
    return true;
}
void unshift_main_two(void* mainView) {
    if (!g_twoShifted) return;
    uint8_t* link = static_cast<uint8_t*>(field(mainView, 0x48));
    if (link) { float* pos = reinterpret_cast<float*>(link + 0x30); for (int i = 0; i < 3; ++i) pos[i] = g_twoSaved[i]; }
    g_twoShifted = false;
}

void __fastcall hkRenderAll(void* list, void* ctx) {
    g_rallCalls.fetch_add(1, std::memory_order_relaxed);
    g_rallThisPresent.fetch_add(1, std::memory_order_relaxed);
    g_rallTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_lastList.store(list, std::memory_order_relaxed);
    ListSnap s;
    if (read_list(list, s)) {
        gate_own(list, s);
        read_list(list, s);
        if (s.n == 2) g_framesTwo.fetch_add(1, std::memory_order_relaxed);
        // ENGINESCENE (V5): the game's world view is in this frame's list = a 3D scene, whatever the HUD draws (the
        // lock-on marker + enemy bar took the Scaleform count from 19 to 21 and tripped the loading-screen detector).
        if (void* mv0 = g_mainView.load())
            for (int i = 0; i < s.n && i < 16; ++i) if (s.views[i] == mv0) { bvr::hud::note_engine_scene(); break; }
    }
    if (g_on && s.list) {
        const int prev = g_lastCount.exchange(s.n, std::memory_order_relaxed);
        if (prev != s.n) {
            char why[64];
            sprintf_s(why, "count %d -> %d, render thread %u", prev, s.n, GetCurrentThreadId());
            log_list(why, list);
        }
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    envtime::render_all_begin();
    envtime::mark("render-all BEGIN");
    void* mv = g_mainView.load();
    const bool two = g_twoDraw.load() && g_stereo.load() && mv && !g_ownView.load() && s.n == 1;
    if (two) {
        shift_main_to_eye(mv, 0);
        oRenderAll(list, ctx);          // left eye
        shift_main_to_eye(mv, 1);
        oRenderAll(list, ctx);          // right eye - everything the engine computes per view is computed again
        unshift_main_two(mv);
        g_twoDrawFrames.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (g_twoShifted && mv) unshift_main_two(mv);
        oRenderAll(list, ctx);
    }
    hdr_swap_now();   // between the view loop and the frame's post chain
    envtime::mark("render-all end");
    QueryPerformanceCounter(&b);
    g_rallTicks.fetch_add(static_cast<uint64_t>(b.QuadPart - a.QuadPart), std::memory_order_relaxed);
}

uint64_t __fastcall hkAddView(void* list, void* view, void* a3, void* a4) {
    const uintptr_t caller = rva(_ReturnAddress());
    if (caller == kMenuViewAdd && view)
        for (auto& m : g_menuViews) { void* e = nullptr; if (m.compare_exchange_strong(e, view)) break; }
    const uint64_t r = oAddView(list, view, a3, a4);
    if (g_on && g_adds.fetch_add(1) < 200) {
        char nm[128];
        BVR_LOG("views: AddView(list %p, view %p %s) from rva 0x%llX -> %llu", list, view, class_name(view, nm, sizeof(nm)),
                static_cast<unsigned long long>(caller), static_cast<unsigned long long>(r));
    }
    return r;
}

uint64_t __fastcall hkRemoveView(void* list, void* view) {
    const uintptr_t caller = rva(_ReturnAddress());
    char nm[128];
    class_name(view, nm, sizeof(nm)); // before the call: the view may be destroyed by it
    for (auto& m : g_menuViews) { void* e = view; m.compare_exchange_strong(e, nullptr); }   // MENUVIEW
    const uint64_t r = oRemoveView(list, view);
    // The game drops its main view on fast travel / quit (P0 run 2): ours goes with it, on the same thread, at once.
    if (view && view == g_mainView.load()) {
        g_mainView.store(nullptr);
        remove_own(list, "the game removed its main view");
    }
    if (g_on && g_removes.fetch_add(1) < 200)
        BVR_LOG("views: RemoveView(list %p, view %p %s) from rva 0x%llX -> %llu", list, view, nm,
                static_cast<unsigned long long>(caller), static_cast<unsigned long long>(r));
    return r;
}

void* __fastcall hkSceneCtor(void* ctx, void* desc) {
    const uintptr_t caller = rva(_ReturnAddress());
    CtorRec rec;
    rec.ctxPtr = ctx;
    rec.callerRva = caller;
    __try {
        if (ctx) rec.ctxVal = *static_cast<uint64_t*>(ctx);
        if (desc) memcpy(rec.desc, desc, sizeof(rec.desc));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    void* r = oSceneCtor(ctx, desc);
    rec.result = r;
    if (caller == addr::RendManInitMainRet - kBase) { g_mainCtor = rec; g_haveMainCtor = true; g_mainView.store(r); g_mainSinceMs = 0; }
    if (g_on && g_ctors.fetch_add(1) < 100) {
        char nm[128], hex[64 * 3 + 1] = {};
        for (int i = 0; i < 64; ++i) sprintf_s(hex + i * 3, 4, "%02X ", rec.desc[i]);
        BVR_LOG("views: scene view built %p %s from rva 0x%llX%s; ctx %p (*ctx 0x%llX); desc %s", r,
                class_name(r, nm, sizeof(nm)), static_cast<unsigned long long>(caller),
                caller == addr::RendManInitMainRet - kBase ? " = THE MAIN VIEW (RendMan init)" : "", ctx,
                static_cast<unsigned long long>(rec.ctxVal), hex);
    }
    return r;
}

// ---- EYEMAP (AC6 EYEMAP): walk a view's render-target set for the D3D11 textures it owns ----
// Every qword that points at a live ID3D11Texture2D / RTV / SRV (recognised by d3d11.dll's own vtables, read from objects we
// create) is listed with its offset path, size and format; other pointers are followed 2 more levels (0x200 bytes each).
// The finished eye picture (the view's last colour target before the HUD composite) and its depth are in that list.
uintptr_t g_vtTex = 0, g_vtRtv = 0, g_vtSrv = 0;
struct Found { ID3D11Texture2D* tex; char path[64]; };
// PICHUNT (run 2026-10-29 00:19: the eyes were black - candidate rt+10B08+40+18 is all zero, while the window is fine): the
// main view's textures from the last EYEMAP, kept with their offset chains so a capture can save every window-size
// RGBA8 / R10G10B10A2 one and the right path can be set live (`ds picpath 0xA 0xB 0xC`).
struct FoundPath { ID3D11Texture2D* tex; uint32_t off[4]; int depth; bool viaView; char path[64]; };
std::vector<FoundPath> g_mainFound;
std::mutex g_foundMutex;
// EYEMAP2: the 18 s freeze of run 2026-10-28 23:59 was this walk (12,096 objects, one VirtualQuery each) on the Present
// thread. Now it runs on its own thread, with the last few readable regions cached.
thread_local uintptr_t t_regLo[8], t_regHi[8];
thread_local int t_regN = 0;
bool readable_ptr(uintptr_t p, size_t n) {
    if (p < 0x10000 || p > 0x7FFFFFFFFFFF || (p & 7)) return false;
    for (int i = 0; i < t_regN; ++i) if (p >= t_regLo[i] && p + n <= t_regHi[i]) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
    const int slot = t_regN < 8 ? t_regN++ : static_cast<int>(p >> 12) & 7;
    t_regLo[slot] = lo; t_regHi[slot] = hi;
    return p + n <= hi;
}
// Each of these touches memory that only LOOKS like a D3D11 object, so each gets its own guard.
ID3D11Texture2D* view_texture(uintptr_t q) {
    ID3D11Texture2D* t = nullptr;
    __try {
        ID3D11Resource* r = nullptr;
        reinterpret_cast<ID3D11View*>(q)->GetResource(&r);
        if (r) {
            if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t)))) t = nullptr;
            r->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { t = nullptr; }
    return t;
}
bool safe_desc(ID3D11Texture2D* t, D3D11_TEXTURE2D_DESC& d) {
    __try { t->GetDesc(&d); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void scan(uintptr_t base, size_t size, int depth, const char* path, Found* out, int& n, int cap, int& visits) {
    if (!readable_ptr(base, size) || ++visits > 40000) return;
    for (size_t off = 0; off + 8 <= size && n < cap; off += 8) {
        const uintptr_t q = *reinterpret_cast<const uintptr_t*>(base + off);
        if (!readable_ptr(q, 16)) continue;
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(q);
        char p[64];
        sprintf_s(p, "%s+%zX", path, off);
        if (v == g_vtTex) {
            bool dup = false;
            for (int i = 0; i < n; ++i) if (out[i].tex == reinterpret_cast<ID3D11Texture2D*>(q)) dup = true;
            if (!dup) { out[n].tex = reinterpret_cast<ID3D11Texture2D*>(q); strcpy_s(out[n].path, p); ++n; }
        } else if ((v == g_vtRtv || v == g_vtSrv) && n < cap) {
            // A qword whose first field matches d3d11's RTV / SRV vtable can still be a dead or half-built object,
            // and GetResource then faults. Its own guard, so one bad pointer does not lose the whole walk (DS3 2026-10-30).
            ID3D11Texture2D* t = view_texture(q);
            if (t) {
                bool dup = false;
                for (int i = 0; i < n; ++i) if (out[i].tex == t) dup = true;
                if (!dup) { out[n].tex = t; sprintf_s(out[n].path, "%s(%s)", p, v == g_vtRtv ? "rtv" : "srv"); ++n; }
                t->Release();
            }
        } else if (depth > 0) {
            scan(q, 0x200, depth - 1, p, out, n, cap, visits);
        }
    }
}
std::vector<FoundPath> g_ownFound;
void store_own_found(const Found* f, int n) {
    std::lock_guard<std::mutex> lk(g_foundMutex);
    g_ownFound.clear();
    for (int i = 0; i < n; ++i) { FoundPath fp{}; fp.tex = f[i].tex; strcpy_s(fp.path, f[i].path); g_ownFound.push_back(fp); }
}
void store_main_found(const Found* f, int n) {
    std::lock_guard<std::mutex> lk(g_foundMutex);
    g_mainFound.clear();
    for (int i = 0; i < n; ++i) { FoundPath fp{}; fp.tex = f[i].tex; strcpy_s(fp.path, f[i].path); g_mainFound.push_back(fp); }
}
void eyemap_walk(void* view, Found* f, int& n, int& visits, uint8_t*& rt) {
    __try {
        rt = static_cast<uint8_t*>(reinterpret_cast<VGet>(vt(view)[3])(view));
        scan(reinterpret_cast<uintptr_t>(rt), addr::kRtSize, 2, "rt", f, n, 96, visits);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void eyemap_view(const char* label, void* view) {
    if (!view || !g_vtTex) return;
    Found f[96]; int n = 0, visits = 0; uint8_t* rt = nullptr;
    eyemap_walk(view, f, n, visits, rt);     // a fault in here keeps whatever was found so far
    BVR_LOG("eyemap [%s]: view %p rt set %p - %d textures (%d objects visited)", label, view, rt, n, visits);
    if (!strcmp(label, "main")) store_main_found(f, n);
    else store_own_found(f, n);
    for (int i = 0; i < n; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        if (!safe_desc(f[i].tex, d)) { BVR_LOG("eyemap [%s]   %-40s %p (unreadable)", label, f[i].path, f[i].tex); continue; }
        BVR_LOG("eyemap [%s]   %-40s %p %ux%u fmt %u bind 0x%X mips %u arr %u", label, f[i].path, f[i].tex, d.Width, d.Height,
                d.Format, d.BindFlags, d.MipLevels, d.ArraySize);
    }
}
void learn_vtables(IDXGISwapChain* sc) {
    if (g_vtTex) return;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = 4; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr; ID3D11RenderTargetView* r = nullptr; ID3D11ShaderResourceView* s = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) && t) {
        g_vtTex = *reinterpret_cast<uintptr_t*>(t);
        if (SUCCEEDED(dev->CreateRenderTargetView(t, nullptr, &r)) && r) { g_vtRtv = *reinterpret_cast<uintptr_t*>(r); r->Release(); }
        if (SUCCEEDED(dev->CreateShaderResourceView(t, nullptr, &s)) && s) { g_vtSrv = *reinterpret_cast<uintptr_t*>(s); s->Release(); }
        t->Release();
    }
    dev->Release();
}
std::atomic<bool> g_eyemapReq{false};
bool g_learnReq = false;          // PAIRLEARN: this recorded frame is for learning, not for saving
std::atomic<bool> g_eyemapBusy{false};
struct EyemapJob { void* main; void* own; };
DWORD WINAPI eyemap_thread(LPVOID p) {
    EyemapJob* j = static_cast<EyemapJob*>(p);
    t_regN = 0;
    if (j->main) eyemap_view("main", j->main);
    if (j->own) eyemap_view("second", j->own);
    delete j;
    g_eyemapBusy.store(false);
    return 0;
}
void eyemap_async(void* main, void* own) {
    if (g_eyemapBusy.exchange(true)) return;
    HANDLE t = CreateThread(nullptr, 0, eyemap_thread, new EyemapJob{main, own}, 0, nullptr);
    if (t) CloseHandle(t); else g_eyemapBusy.store(false);
}

// The view's finished picture (EYEMAP 2026-10-28: rt+10B08 -> +0x40 -> +0x18, 2048x1152 R10G10B10A2, the same format as the
// window) - to confirm in captures; M3 eye feed reads the same path.
// The view's finished picture: an offset chain from its render-target set, as EYEMAP prints it ("rt+368+40+18", an
// "(rtv)" / "(srv)" end = take the view's texture). PICHUNT picks it by matching the window; `ds picpath rt+...` sets it.
std::mutex g_picMutex;
uint32_t g_picOff[4] = {0x10B08, 0x40, 0x18, 0};
int g_picDepth = 3;
char g_picPath[64] = "rt+10B08+40+18";
bool parse_path(const char* s, uint32_t off[4], int& depth) {
    if (_strnicmp(s, "rt", 2) != 0) return false;
    depth = 0;
    const char* p = s + 2;
    while (*p == '+' && depth < 4) { char* e = nullptr; off[depth++] = static_cast<uint32_t>(strtoul(p + 1, &e, 16)); p = e; }
    return depth > 0;
}
ID3D11Texture2D* resolve_tex(uintptr_t obj) {
    const uintptr_t v = *reinterpret_cast<const uintptr_t*>(obj);
    if (v == g_vtTex) return reinterpret_cast<ID3D11Texture2D*>(obj);
    if (v == g_vtRtv || v == g_vtSrv) {
        ID3D11Resource* r = nullptr;
        reinterpret_cast<ID3D11View*>(obj)->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t)); r->Release(); }
        if (t) t->Release(); // the view keeps it alive
        return t;
    }
    return nullptr;
}
// SPLITTARGET (fake-headset run 2026-10-29 01:25): the final pass (one command list per frame) reads t0 = rt+40+40+18
// (2048x1152 R10G10B10A2, SHARED by both views), t1 = rt+40+40+1F8 (RGBA8, the HUD) and a 256x1 R16 table (brightness),
// and writes the window picture. Each view writes its finished scene into t0 in turn, so PICSPLIT splits t0.
uint32_t g_splitOff[4] = {0x40, 0x40, 0x18, 0};
int g_splitDepth = 3;
ID3D11Texture2D* resolve_path(void* view, const uint32_t* off, int depth) {
    if (!view || !g_vtTex) return nullptr;
    uintptr_t p = reinterpret_cast<uintptr_t>(reinterpret_cast<VGet>(vt(view)[3])(view));
    for (int i = 0; i < depth; ++i) {
        if (!readable_ptr(p + off[i], 8)) return nullptr;
        p = *reinterpret_cast<const uintptr_t*>(p + off[i]);
        if (!readable_ptr(p, 16)) return nullptr;
    }
    return resolve_tex(p);
}
// ---- EXPOSHARE: one adapted brightness for both eyes (VR_ESSENTIALS section 2, STR-009 / FAIL-STR-016) ----
// Dark Souls III measures the scene's brightness down to a 1x1 texture and tone-maps from it. The reduction chain
// itself is shared by both views (EYEMAP: rt+2EA0.. / rt+2EC0.. are the same objects), but the ADAPTED value each
// view tone-maps with is per view - rt+1318+1E0+158, 1x1 R32G32_FLOAT, a different object in each render-target
// set. Our second view's copy never adapts, so its tone-map saw ~0 and its picture came out black with only the
// bonfire's brightest pixels showing (splitdump 2026-10-30: writes 5-8, max red 14 of 255, 1 % of pixels).
// A 1x1 copy of the game view's value into ours every frame costs nothing and makes both eyes tone-map alike,
// which is what a stereo pair needs anyway (an eye that adapts on its own pumps against the other).
std::atomic<int> g_expoShare{1};
std::atomic<uint32_t> g_expoCopies{0};
ID3D11Texture2D* resolve_path(void* view, const uint32_t* off, int depth);
void exposure_share(IDXGISwapChain* sc) {
    if (!g_expoShare.load()) return;
    void* mainV = g_mainView.load();
    void* ownV = g_ownView.load();
    if (!mainV || !ownV) return;
    static const uint32_t kExpoPath[3] = {0x1318, 0x1E0, 0x158};
    ID3D11Texture2D* from = resolve_path(mainV, kExpoPath, 3);
    ID3D11Texture2D* to = resolve_path(ownV, kExpoPath, 3);
    if (!from || !to || from == to) return;
    D3D11_TEXTURE2D_DESC a{}, b{};
    from->GetDesc(&a); to->GetDesc(&b);
    if (a.Width != b.Width || a.Height != b.Height || a.Format != b.Format) return;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* imm = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    dev->GetImmediateContext(&imm);
    if (imm) { imm->CopyResource(to, from); g_expoCopies.fetch_add(1, std::memory_order_relaxed); imm->Release(); }
    dev->Release();
}

ID3D11Texture2D* split_target(void* view) {
    uint32_t off[4]; int depth;
    { std::lock_guard<std::mutex> lk(g_picMutex); memcpy(off, g_splitOff, sizeof(off)); depth = g_splitDepth; }
    return resolve_path(view, off, depth);
}
ID3D11Texture2D* view_picture(void* view) {
    if (!view || !g_vtTex) return nullptr;
    uint32_t off[4]; int depth;
    { std::lock_guard<std::mutex> lk(g_picMutex); memcpy(off, g_picOff, sizeof(off)); depth = g_picDepth; }
    uintptr_t p = reinterpret_cast<uintptr_t>(reinterpret_cast<VGet>(vt(view)[3])(view));
    for (int i = 0; i < depth; ++i) {
        if (!readable_ptr(p + off[i], 8)) return nullptr;
        p = *reinterpret_cast<const uintptr_t*>(p + off[i]);
        if (!readable_ptr(p, 16)) return nullptr;
    }
    return resolve_tex(p);
}
void set_pic_path(const char* s) {
    uint32_t off[4] = {}; int depth = 0;
    char clean[64]; strncpy_s(clean, s, _TRUNCATE);
    if (char* paren = strchr(clean, '(')) *paren = 0;
    if (!parse_path(clean, off, depth)) { BVR_LOG("picpath: '%s' not understood (want rt+X+Y+Z)", s); return; }
    std::lock_guard<std::mutex> lk(g_picMutex);
    memcpy(g_picOff, off, sizeof(off)); g_picDepth = depth; strcpy_s(g_picPath, clean);
    settings::set_i("pic_off0", static_cast<int>(off[0])); settings::set_i("pic_off1", static_cast<int>(off[1]));
    settings::set_i("pic_off2", static_cast<int>(off[2])); settings::set_i("pic_off3", static_cast<int>(off[3]));
    settings::set_i("pic_depth", depth);
    BVR_LOG("picpath: the eyes now read %s", clean);
}

// Read a texture back (staging copy) as 8-bit RGB samples every `step` pixels; formats 24 (R10G10B10A2) and 28 (RGBA8).
bool read_rgb(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT step, std::vector<uint8_t>& out, UINT& w, UINT& h) {
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
    const bool ten = d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    const bool eight = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                       d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!ten && !eight) return false;
    ID3D11Device* dev = nullptr; tex->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc = {1, 0};
    ID3D11Texture2D* st = nullptr;
    bool ok = false;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            w = (d.Width + step - 1) / step; h = (d.Height + step - 1) / step;
            out.resize(static_cast<size_t>(w) * h * 3);
            for (UINT y = 0; y < h; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * step * m.RowPitch);
                for (UINT x = 0; x < w; ++x) {
                    const uint32_t p = row[x * step];
                    uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 3];
                    if (ten) { o[0] = static_cast<uint8_t>((p & 0x3FF) >> 2); o[1] = static_cast<uint8_t>(((p >> 10) & 0x3FF) >> 2); o[2] = static_cast<uint8_t>(((p >> 20) & 0x3FF) >> 2); }
                    else { o[0] = static_cast<uint8_t>(p); o[1] = static_cast<uint8_t>(p >> 8); o[2] = static_cast<uint8_t>(p >> 16); }
                }
            }
            ctx->Unmap(st, 0);
            ok = true;
        }
        st->Release();
    }
    if (dev) dev->Release();
    return ok;
}

// DEPTHCHECK (DLSS M6): read an eye's depth copy back and say which end of its range is NEAR. DLSS must be told
// (DepthInverted) or its closest-depth motion-vector dilation breaks moving edges (playbook 19 #dlss-in-vr). The test
// is objective: the bottom of the picture is the ground at the player's feet, the middle band is far away, so whichever
// of the two is larger names the convention. Present thread, on request only.
void depth_check(ID3D11DeviceContext* ctx, int eye) {
    ID3D11Texture2D* t = picsplit::eye_depth(eye);
    if (!t) { BVR_LOG("depthcheck: eye %d has no depth copy yet (%s)", eye, picsplit::dlss_status()); return; }
    D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
    ID3D11Device* dev = nullptr; t->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc = {1, 0};
    ID3D11Texture2D* st = nullptr;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
        ctx->CopyResource(st, t);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const bool f32 = d.Format == DXGI_FORMAT_R32_TYPELESS || d.Format == DXGI_FORMAT_D32_FLOAT ||
                             d.Format == DXGI_FORMAT_R32_FLOAT;
            const bool f24 = d.Format == DXGI_FORMAT_R24G8_TYPELESS || d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
            const bool f3224 = d.Format == DXGI_FORMAT_R32G8X24_TYPELESS || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            double lo = 1e30, hi = -1e30, bandMid = 0, bandBot = 0;
            int nMid = 0, nBot = 0;
            for (UINT y = 0; y < d.Height; y += 8) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                for (UINT x = 0; x < d.Width; x += 8) {
                    double v = 0;
                    if (f32) v = reinterpret_cast<const float*>(row)[x];
                    else if (f3224) v = reinterpret_cast<const float*>(row)[x * 2];
                    else if (f24) v = static_cast<double>(reinterpret_cast<const uint32_t*>(row)[x] & 0xFFFFFF) / 16777215.0;
                    else v = static_cast<double>(reinterpret_cast<const uint16_t*>(row)[x]) / 65535.0;
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                    if (y > d.Height * 4 / 10 && y < d.Height * 6 / 10) { bandMid += v; ++nMid; }
                    if (y > d.Height * 9 / 10) { bandBot += v; ++nBot; }
                }
            }
            ctx->Unmap(st, 0);
            const double mid = nMid ? bandMid / nMid : 0, bot = nBot ? bandBot / nBot : 0;
            BVR_LOG("depthcheck eye %d: %ux%u format %d | range %.6f .. %.6f | middle band %.6f, bottom band %.6f -> "
                    "near is %s, so DepthInverted should be %d", eye, d.Width, d.Height, (int)d.Format, lo, hi, mid, bot,
                    bot > mid ? "the LARGE end (reversed Z)" : "the SMALL end (classic Z)", bot > mid ? 1 : 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
}

// PICHUNT: every window-size picture of the main view against the window itself (the HUD is the only difference
// expected); the closest non-black one becomes the eye picture. Present thread, once per 3D start (a short hitch).
void pichunt(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    std::vector<uint8_t> ref; UINT rw = 0, rh = 0;
    D3D11_TEXTURE2D_DESC bd{}; if (bb) bb->GetDesc(&bd);
    if (bb && read_rgb(ctx, bb, 16, ref, rw, rh)) {
        std::vector<FoundPath> cands;
        { std::lock_guard<std::mutex> lk(g_foundMutex); cands = g_mainFound; }
        double best = 1e9; char bestPath[64] = "";
        for (const auto& c : cands) {
            if (!readable_ptr(reinterpret_cast<uintptr_t>(c.tex), 16) || *reinterpret_cast<const uintptr_t*>(c.tex) != g_vtTex) continue;
            D3D11_TEXTURE2D_DESC d{}; c.tex->GetDesc(&d);
            if (d.Width != bd.Width || d.Height != bd.Height) continue;
            std::vector<uint8_t> s; UINT w = 0, h = 0;
            if (!read_rgb(ctx, c.tex, 16, s, w, h) || w != rw || h != rh) continue;
            double diff = 0, mean = 0;
            for (size_t i = 0; i < s.size(); ++i) { diff += std::abs(static_cast<int>(s[i]) - static_cast<int>(ref[i])); mean += s[i]; }
            diff /= static_cast<double>(s.size()); mean /= static_cast<double>(s.size());
            BVR_LOG("pichunt: %-34s fmt %u mean %.1f, difference from the window %.1f", c.path, d.Format, mean, diff);
            if (mean > 4.0 && diff < best) { best = diff; strcpy_s(bestPath, c.path); }
        }
        if (bestPath[0] && best < 25.0) { BVR_LOG("pichunt: best match %s (difference %.1f)", bestPath, best); set_pic_path(bestPath); }
        else BVR_LOG("pichunt: no picture matches the window (best %.1f %s) - eye picture unchanged", best, bestPath);
    }
    if (bb) bb->Release();
    ctx->Release(); dev->Release();
}
// OWNHUNT (M3b, after AC6's SAMEFRAME: there the second view had a finished picture of its OWN, `stereoTexture`).
// Every full-size picture of OUR second view against the window (which shows the game view's finished picture). A
// close match would be our own finished eye; a far one says our view stops at its HDR, as the split dump suggests.
void ownhunt(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    std::vector<uint8_t> ref; UINT rw = 0, rh = 0;
    D3D11_TEXTURE2D_DESC bd{}; if (bb) bb->GetDesc(&bd);
    if (bb && read_rgb(ctx, bb, 16, ref, rw, rh)) {
        std::vector<FoundPath> cands;
        { std::lock_guard<std::mutex> lk(g_foundMutex); cands = g_ownFound; }
        BVR_LOG("ownhunt: %zu textures of our second view against the window (%ux%u)", cands.size(), rw, rh);
        double best = 1e9; char bestPath[64] = "";
        for (const auto& c : cands) {
            if (!readable_ptr(reinterpret_cast<uintptr_t>(c.tex), 16) || *reinterpret_cast<const uintptr_t*>(c.tex) != g_vtTex) continue;
            D3D11_TEXTURE2D_DESC d{}; c.tex->GetDesc(&d);
            if (d.Width != bd.Width || d.Height != bd.Height) continue;
            std::vector<uint8_t> s2; UINT w = 0, h = 0;
            if (!read_rgb(ctx, c.tex, 16, s2, w, h) || w != rw || h != rh) continue;
            double diff = 0, mean = 0;
            for (size_t i = 0; i < s2.size(); ++i) { diff += std::abs(static_cast<int>(s2[i]) - static_cast<int>(ref[i])); mean += s2[i]; }
            diff /= static_cast<double>(s2.size()); mean /= static_cast<double>(s2.size());
            BVR_LOG("ownhunt: %-34s fmt %u mean %.1f, difference from the finished picture %.1f", c.path, d.Format, mean, diff);
            if (mean > 4.0 && diff < best) { best = diff; strcpy_s(bestPath, c.path); }
        }
        BVR_LOG("ownhunt: closest %s (difference %.1f) - under about 25 would mean our view finishes a picture of its own",
                bestPath[0] ? bestPath : "none", best);
    }
    if (bb) bb->Release();
    ctx->Release(); dev->Release();
}
std::atomic<bool> g_hunted{false}, g_huntReq{false}, g_ownHuntReq{false}, g_checkReq{false};

// SELFCHECK (memory self-check-before-jj): are the two eye pictures different, how far apart sideways (a real eye
// separation moves near things by many pixels, the far distance by ~0), and how bright each is.
void eyes_check(IDXGISwapChain* sc, const char* why) {
    ID3D11Texture2D* L = picsplit::left();
    ID3D11Texture2D* R = picsplit::right();
    if (!L || !R) { BVR_LOG("selfcheck [%s]: no eye pair yet (%s)", why, picsplit::status()); return; }
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    std::vector<uint8_t> a, b; UINT aw = 0, ah = 0, bw = 0, bh = 0;
    if (read_rgb(ctx, L, 4, a, aw, ah) && read_rgb(ctx, R, 4, b, bw, bh) && aw == bw && ah == bh && aw > 64) {
        double ma = 0, mb = 0, diff = 0;
        for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; diff += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])); }
        ma /= a.size(); mb /= b.size(); diff /= a.size();
        // horizontal offset of the right picture against the left (middle third of rows, -24..24 samples = +-96 px)
        int bestDx = 0; double bestE = 1e9;
        for (int dx = -24; dx <= 24; ++dx) {
            double e = 0; size_t n = 0;
            for (UINT y = ah / 3; y < 2 * ah / 3; ++y)
                for (UINT x = 32; x + 32 < aw; ++x) {
                    const int xb = static_cast<int>(x) + dx;
                    const uint8_t* pa = &a[(static_cast<size_t>(y) * aw + x) * 3];
                    const uint8_t* pb = &b[(static_cast<size_t>(y) * aw + xb) * 3];
                    e += std::abs(static_cast<int>(pa[1]) - static_cast<int>(pb[1])); ++n;
                }
            e /= n ? n : 1;
            if (e < bestE) { bestE = e; bestDx = dx; }
        }
        // sky (top sixth) and ground (bottom sixth) on their own: fog and sky settings show there first
        double skyA = 0, skyB = 0, grA = 0, grB = 0; size_t ns = 0, ng = 0;
        for (UINT y = 0; y < ah / 6; ++y) for (UINT x = 0; x < aw; ++x) {
            const size_t i = (static_cast<size_t>(y) * aw + x) * 3; skyA += a[i]; skyB += b[i]; ++ns;
        }
        for (UINT y = 5 * ah / 6; y < ah; ++y) for (UINT x = 0; x < aw; ++x) {
            const size_t i = (static_cast<size_t>(y) * aw + x) * 3; grA += a[i]; grB += b[i]; ++ng;
        }
        if (ns) { skyA /= ns; skyB /= ns; }
        if (ng) { grA /= ng; grB /= ng; }
        int pl = 0, pr = 0, pw = 0;
        picsplit::pair_index(pl, pr, pw);
        BVR_LOG("selfcheck [%s]: left brightness %.1f, right %.1f (ratio %.3f), mean difference %.2f, best sideways match %+d px "
                "(error %.2f) -> %s", why, ma, mb, ma > 0.1 ? mb / ma : 0.0, diff, bestDx * 4, bestE,
                diff < 0.05 ? "IDENTICAL eyes (no 3D)" : (ma < 2 || mb < 2 ? "an eye is BLACK" : "eyes differ"));
        BVR_LOG("selfcheck [%s]: sky left %.1f right %.1f (ratio %.3f), ground left %.1f right %.1f (ratio %.3f); "
                "pictures taken after writes %d and %d of %d%s (frame structure changed %u times)", why, skyA, skyB, skyA > 0.1 ? skyB / skyA : 0.0,
                grA, grB, grA > 0.1 ? grB / grA : 0.0, pl, pr, pw,
                pr == pl + 1 ? "  <-- NEIGHBOURING writes: the second eye may not have drawn in between" : "",
                picsplit::pair_unstable());
    } else {
        BVR_LOG("selfcheck [%s]: eye pictures could not be read back", why);
    }
    ctx->Release(); dev->Release();
}

// ---- RTCLASS: sample both settings objects every frame, then classify (see the class table above) ----
const uint8_t* rt_of(void* view) {
    return view ? static_cast<const uint8_t*>(reinterpret_cast<VGet>(vt(view)[3])(view)) : nullptr;
}

void rtclass_sample(void* mainView, void* ownView) {
    if (g_classState.load() != 1) return;
    const uint64_t* a = reinterpret_cast<const uint64_t*>(rt_of(mainView));
    const uint64_t* b = reinterpret_cast<const uint64_t*>(rt_of(ownView));
    if (!a || !b) return;
    const uint32_t got = g_classGot.fetch_add(1);
    if (got == 0) {                       // the first sample is the reference
        g_firstM.assign(a, a + kRtQ);
        g_firstO.assign(b, b + kRtQ);
        g_chgM.assign(kRtQ, 0);
        g_chgO.assign(kRtQ, 0);
        return;
    }
    for (uint32_t i = 0; i < kRtQ; ++i) {
        if (a[i] != g_firstM[i]) g_chgM[i] = 1;
        if (b[i] != g_firstO[i]) g_chgO[i] = 1;
    }
    if (got + 1 >= g_classWant.load()) rtclass_finish();
}

// Merge the fields of the wanted classes (A, or A and B) into byte ranges. Two runs join ONLY across fields that are
// already EQUAL in both views (class 0, so copying them changes nothing). A per-view field (C) or a pointer (P) always
// ends the run: merging across one would copy exactly what must never be copied (fixed 2026-10-29).
void build_ranges(const std::vector<uint8_t>& cls, bool onlyA, std::vector<Range>& out) {
    out.clear();
    bool open = false;
    uint32_t start = 0, last = 0;
    for (uint32_t i = 0; i < cls.size(); ++i) {
        const uint8_t c = cls[i];
        const bool take = c == 1 || (!onlyA && c == 2);
        if (take) {
            if (!open) { start = last = i; open = true; }
            else last = i;                                  // the gap since `last` was all class 0
        } else if (open && (c != 0 || i - last > 2)) {      // C/P, or a long equal gap: close the run here
            out.push_back({start * 8, (last - start + 1) * 8});
            open = false;
        }
    }
    if (open) out.push_back({start * 8, (last - start + 1) * 8});
}

// After sampling: the class of every field, the ranges to copy, and a readable log.
std::vector<uint8_t> g_classOf;           // 0 equal, 1 A, 2 B, 3 C, 4 P
// A second measurement may only make a field LESS copyable: a field that moves with the camera in one area but stood
// still in the area we measured before must end up per-view (C), never copied. 0 equal < A < B < C < P.
std::vector<uint8_t> g_classPrev;      // the table before this measurement (empty = start fresh)

void rtclass_finish() {
    g_classState.store(2);
    void* m = g_mainView.load(); void* o = g_ownView.load();
    const uint64_t* a = reinterpret_cast<const uint64_t*>(rt_of(m));
    const uint64_t* b = reinterpret_cast<const uint64_t*>(rt_of(o));
    if (!a || !b) { BVR_LOG("rtclass: both views must still be listed"); return; }
    g_classOf.assign(kRtQ, 0);
    for (int& c : g_classCount) c = 0;
    for (uint32_t i = 0; i < kRtQ; ++i) {
        if (a[i] == b[i]) continue;
        if (readable_ptr(static_cast<uintptr_t>(a[i]), 8) || readable_ptr(static_cast<uintptr_t>(b[i]), 8)) { g_classOf[i] = 4; ++g_classCount[3]; continue; }
        if (g_chgO[i]) { g_classOf[i] = 3; ++g_classCount[2]; continue; }   // the engine maintains it for our view too
        if (g_chgM[i]) { g_classOf[i] = 2; ++g_classCount[1]; }             // only the main view's changes
        else           { g_classOf[i] = 1; ++g_classCount[0]; }             // nothing changes it: a settings value
    }
    if (!g_classPrev.empty() && g_classPrev.size() == g_classOf.size()) {   // merge with the table we started from
        int kept = 0;
        for (uint32_t i = 0; i < kRtQ; ++i) {
            const uint8_t was = g_classPrev[i], now = g_classOf[i];
            const uint8_t merged = was > now ? was : now;   // 0 < A(1) < B(2) < C(3) < P(4): the stricter one wins
            if (merged != now) { ++kept; g_classOf[i] = merged; }
        }
        BVR_LOG("rtclass: merged with the table already loaded - %d fields kept their stricter class", kept);
        for (int& k : g_classCount) k = 0;
        for (uint32_t i = 0; i < kRtQ; ++i) if (g_classOf[i]) ++g_classCount[g_classOf[i] - 1];
        g_classPrev.clear();
    }
    BVR_LOG("rtclass: %u samples. A (area settings ours never got) %d, B (engine state only the main view gets) %d, "
            "C (per view - never copied) %d, P (pointers - never copied) %d",
            g_classGot.load(), g_classCount[0], g_classCount[1], g_classCount[2], g_classCount[3]);
    // the A/B ranges, merged, with their values
    std::vector<Range> rs;
    build_ranges(g_classOf, false, rs);
    int shown = 0;
    for (const Range& r : rs) {
        if (shown++ >= 70) break;
        const float* fa = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(a) + r.off);
        const float* fb = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(b) + r.off);
        BVR_LOG("rtclass   +0x%05X len 0x%X  class %c  main %.5g %.5g  ours %.5g %.5g", r.off, r.len,
                g_classOf[r.off / 8] == 1 ? 'A' : 'B', fa[0], fa[1], fb[0], fb[1]);
    }
    BVR_LOG("rtclass: %d ranges to copy (%d listed). `ds rtclass apply` turns the copy on, `ds rtclass applya` only class A.",
            static_cast<int>(rs.size()), shown);
}

// Build the live copy list from the classification. onlyA = the safest set (settings that never change).
void rtclass_apply(bool onlyA) {
    if (g_classState.load() != 2 || g_classOf.empty()) { BVR_LOG("rtclass: measure first (`ds rtclass 240` while walking)"); return; }
    std::vector<Range> rs;
    build_ranges(g_classOf, onlyA, rs);
    int n = 0;
    for (const Range& r : rs) { if (n < 1024) g_copy[n++] = r; }
    g_copyN.store(n);
    g_copyAlways.store(true);
    g_primeFrames.store(0);
    BVR_LOG("rtlight: copying %d measured ranges (%s) from the main view into ours every frame", n, onlyA ? "class A only" : "classes A and B");
}

// RTCLASS save / load: the measured classes live in `<game>\DS3VR2\rtclass.txt`, so a normal launch primes the view
// without a measuring run. One line per field run: "<A|B|C|P> <offset> <length>".
void rtclass_path(wchar_t* out, size_t n) { swprintf_s(out, n, L"%s\\rtclass.txt", bvr::log::data_dir()); }

// Runs of ONE class, for the saved table. Only fields right next to each other join: merging across a gap would put a
// field of another class inside the run, and a saved A-run holding one per-view (C) field copies the eye's own camera
// into our view (seen 2026-10-29: the eyes stood 96 px apart at zero eye separation).
void ranges_of_class(uint8_t want, std::vector<Range>& out) {
    out.clear();
    bool open = false; uint32_t start = 0, last = 0;
    for (uint32_t i = 0; i < g_classOf.size(); ++i) {
        if (g_classOf[i] == want) {
            if (open && i == last + 1) last = i;
            else { if (open) out.push_back({start * 8, (last - start + 1) * 8}); start = last = i; open = true; }
        }
    }
    if (open) out.push_back({start * 8, (last - start + 1) * 8});
}

void rtclass_save() {
    if (g_classState.load() != 2 || g_classOf.empty()) { BVR_LOG("rtclass: nothing measured to save"); return; }
    wchar_t path[MAX_PATH]; rtclass_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") || !f) { BVR_LOG("rtclass: cannot write rtclass.txt"); return; }
    fprintf(f, "# DS3VR2 settings-object classes, size 0x%X, %u samples (A area setting, B engine state, C per view, P pointer)\n",
            kRtSize, g_classGot.load());
    int n = 0;
    for (uint8_t cls = 1; cls <= 4; ++cls) {
        std::vector<Range> rs; ranges_of_class(cls, rs);
        for (const Range& r : rs) { fprintf(f, "%c 0x%X 0x%X\n", "ABCP"[cls - 1], r.off, r.len); ++n; }
    }
    fclose(f);
    BVR_LOG("rtclass: saved %d runs to DS3VR2\\rtclass.txt", n);
}

bool rtclass_load(bool quiet) {
    wchar_t path[MAX_PATH]; rtclass_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") || !f) { if (!quiet) BVR_LOG("rtclass: no DS3VR2\\rtclass.txt yet (measure with `ds rtclass 240`)"); return false; }
    g_classOf.assign(kRtQ, 0);
    for (int& c : g_classCount) c = 0;
    char line[160];
    int runs = 0;
    while (fgets(line, sizeof line, f)) {
        char c = 0; unsigned off = 0, len = 0;
        if (line[0] == '#' || sscanf_s(line, "%c 0x%X 0x%X", &c, 1, &off, &len) != 3) continue;
        const char* k = strchr("ABCP", c);
        if (!k || off + len > kRtSize) continue;
        const uint8_t cls = static_cast<uint8_t>(k - "ABCP" + 1);
        for (unsigned i = off / 8; i < (off + len) / 8 && i < kRtQ; ++i) { g_classOf[i] = cls; ++g_classCount[cls - 1]; }
        ++runs;
    }
    fclose(f);
    g_classState.store(2);
    BVR_LOG("rtclass: loaded %d runs from DS3VR2\\rtclass.txt (A %d, B %d, C %d, P %d fields)", runs,
            g_classCount[0], g_classCount[1], g_classCount[2], g_classCount[3]);
    std::vector<Range> rs;
    build_ranges(g_classOf, false, rs);
    int n = 0;
    for (const Range& r : rs) if (n < 1024) g_copy[n++] = r;
    g_copyN.store(n);
    g_copyAlways.store(true);     // every frame: the area's settings arrive AFTER our view is listed, and change with
    g_primeFrames.store(0);       // the area and the time of day - one priming copy at listing is not enough (L3)
    BVR_LOG("rtlight: %d measured ranges copied from the main view into ours every frame", n);
    return true;
}

// RTDIFF: the main view's render-target set against ours, dword by dword over addr::kRtSize bytes; pointers (qwords that point
// into readable memory) are skipped; neighbouring differences (gap <= 8 bytes) merge into ranges. Present thread.
void rtdiff() {
    void* m = g_mainView.load(); void* o = g_ownView.load();
    if (!m || !o) { BVR_LOG("rtdiff: both views must be listed"); return; }
    const uint8_t* a = static_cast<const uint8_t*>(reinterpret_cast<VGet>(vt(m)[3])(m));
    const uint8_t* b = static_cast<const uint8_t*>(reinterpret_cast<VGet>(vt(o)[3])(o));
    if (!a || !b) return;
    t_regN = 0;
    g_diffN = 0;
    int total = 0;
    uint32_t start = 0, last = 0;
    bool open = false;
    for (uint32_t off = 0; off + 8 <= addr::kRtSize; off += 8) {
        const uint64_t qa = *reinterpret_cast<const uint64_t*>(a + off), qb = *reinterpret_cast<const uint64_t*>(b + off);
        if (qa == qb) continue;
        if (readable_ptr(static_cast<uintptr_t>(qa), 8) || readable_ptr(static_cast<uintptr_t>(qb), 8)) continue; // pointers
        ++total;
        if (open && off - last <= 16) { last = off; continue; }
        if (open && g_diffN < 256) g_diff[g_diffN++] = {start, last + 8 - start};
        start = last = off; open = true;
    }
    if (open && g_diffN < 256) g_diff[g_diffN++] = {start, last + 8 - start};
    BVR_LOG("rtdiff: %d differing non-pointer qwords in %d ranges (main rt %p, ours %p)", total, g_diffN, a, b);
    for (int i = 0; i < g_diffN && i < 120; ++i) {
        const Range& r = g_diff[i];
        const float* fa = reinterpret_cast<const float*>(a + r.off); const float* fb = reinterpret_cast<const float*>(b + r.off);
        const uint32_t* ua = reinterpret_cast<const uint32_t*>(a + r.off); const uint32_t* ub = reinterpret_cast<const uint32_t*>(b + r.off);
        BVR_LOG("rtdiff   +0x%05X len 0x%X  main %08X %08X (%.4g %.4g)  ours %08X %08X (%.4g %.4g)", r.off, r.len, ua[0], ua[1],
                fa[0], fa[1], ub[0], ub[1], fb[0], fb[1]);
    }
}

// ---- M7 HUD, first step (JJ 2026-10-29 "we need to bring back the in-game HUD"): the eyes are taken from the scene
// picture BEFORE the final pass adds the HUD, so the 3D eyes carry none. The game's HUD sheet t1 = rt+40+40+1F8 (RGBA8,
// SPLITTARGET; drawn once a frame, premultiplied with coverage - v1 sk_hudsplit.cpp) goes to the core's room-fixed HUD
// panel (per-eye quads under Virtual Desktop = real depth; HUD drawer: distance / size / height). The core copies it
// into the panel's image at submission, after the frame's command lists ran. VR_HUD_GUIDE; HUDINEYE (drawn into each eye)
// is the next step if VD's linear blending makes the see-through parts milky (VR_ESSENTIALS §6).
std::atomic<ID3D11Texture2D*> g_hudSheet{nullptr};
std::atomic<bool> g_hudOn{true};
std::atomic<uint32_t> g_hudGiven{0};
// HUDALPHA (JJ 2026-10-29, first headset run of M7: "a big black square in the middle of the field of view"): the panel
// was black and opaque everywhere the HUD draws nothing, so the sheet's alpha is not coverage. Dark Souls III's HUD blends build
// no coverage (v1 run 46); the sheet's empty pixels hold alpha ~1 = the share of the scene the final pass KEEPS. The panel
// gets our own copy with coverage = 1 - alpha (and at least the pixel's brightness, so nothing bright can vanish).
// hud_alpha / "ds hudalpha": -1 = decided by measuring the sheet's empty pixels (hud_measure), 0 = alpha as stored, 1 = inverted.
std::atomic<int> g_hudAlpha{-1};
std::atomic<int> g_hudAlphaSeen{0};   // the measurement's verdict. DS3 (V12): starts "as stored" - every DS3 measurement said so
                                      // (the inverted start was Sekiro's, and showed a black panel until the first measurement)
ID3D11Texture2D* g_hudOut = nullptr;
ID3D11RenderTargetView* g_hudOutRtv = nullptr;
ID3D11ShaderResourceView* g_hudSrcSrv = nullptr;
ID3D11Texture2D* g_hudSrcOf = nullptr;
// FADESHEET (2026-11-01, JJ: "when traveling there's a black window that appears before the loading screen"). The game's
// fade to black is drawn on its HUD (a 16:9 stage), which in VR is our floating panel = a dark box over the world (Sekiro had
// the same, memory game-fade-lands-on-hud-panel). DS3's fade pieces cannot be named (no movie names, the recorder missed
// them), so the SHEET itself is measured: a 16 x 9 grid of cells over the stage band, mean alpha and colour per cell (GPU,
// read back 2-3 frames late without waiting). A fade = most of the stage covered by a dark, see-through layer: the 15th
// percentile of (alpha - brightness) over the cells. Our full-view fade follows it; while almost nothing coloured is on the
// sheet (a pure fade, not a menu over a dimmed world) the HUD panel is hidden, so no box. The subtitle backdrop covers only
// the bottom, so it never counts. `ds fadesheet 0|1` (settings fade_sheet, default 1).
namespace fs {
std::atomic<int> g_on{-1};
std::atomic<float> g_fade{0.0f};
std::atomic<bool> g_hidePanel{false};
ID3D11ComputeShader* g_cs = nullptr; ID3D11Buffer* g_cb = nullptr; ID3D11Buffer* g_buf = nullptr;
ID3D11UnorderedAccessView* g_uav = nullptr; ID3D11ShaderResourceView* g_srv = nullptr; ID3D11Texture2D* g_srvOf = nullptr;
ID3D11Buffer* g_st[3] = {}; bool g_pend[3] = {}; int g_next = 0; int g_state = 0;
ID3DDeviceContextState* g_own = nullptr;
constexpr int kCells = 16 * 9;
bool build(ID3D11Device* dev) {
    if (g_state) return g_state == 1;
    g_state = -1;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) return false;
    static const char cs[] =
        "Texture2D<float4> T : register(t0); RWByteAddressBuffer O : register(u0);"
        "cbuffer C : register(b0) { uint w, h, y0, bandH; };"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
        " uint2 p = uint2(id.x * 4 + 2, y0 + id.y * 4 + 2); if (p.x >= w || p.y >= y0 + bandH || p.y >= h) return;"
        " float4 v = T.Load(int3(p, 0)); uint cx = min(15u, p.x * 16 / w), cy = min(8u, (p.y - y0) * 9 / bandH);"
        " uint base = (cy * 16 + cx) * 12; float lum = max(v.r, max(v.g, v.b));"
        " O.InterlockedAdd(base, (uint)(saturate(v.a) * 255 + 0.5)); O.InterlockedAdd(base + 4, (uint)(saturate(lum) * 255 + 0.5));"
        " O.InterlockedAdd(base + 8, 1); }";
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(compile(cs, sizeof(cs) - 1, "fadesheet", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, &err)) || !blob) {
        BVR_LOG("fadesheet: shader failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    const bool ok = SUCCEEDED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_cs));
    blob->Release();
    D3D11_BUFFER_DESC cd{}; cd.ByteWidth = 16; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = kCells * 12; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (!ok || FAILED(dev->CreateBuffer(&cd, nullptr, &g_cb)) || FAILED(dev->CreateBuffer(&bd, nullptr, &g_buf))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kCells * 3; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(dev->CreateUnorderedAccessView(g_buf, &ud, &g_uav))) return false;
    for (auto& s : g_st) {
        D3D11_BUFFER_DESC sd{}; sd.ByteWidth = kCells * 12; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &s))) return false;
    }
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1))) && d1) {
        const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_own);
        d1->Release();
    }
    g_state = 1;
    BVR_LOG("fadesheet: the HUD sheet is measured every frame for a full-stage fade");
    return true;
}
// once a frame at the Present, after the HUD of this frame is on the sheet
void measure(ID3D11DeviceContext* ctx, ID3D11Texture2D* sheet, bool world3d) {
    if (g_on.load() < 0) g_on.store(settings::i("fade_sheet", 1));
    if (!g_on.load() || !sheet || !world3d) {
        g_fade.store(0.0f);
        if (g_hidePanel.exchange(false)) BVR_LOG("fadesheet: HUD panel back (3D ended or off)");
        return;
    }
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = build(dev);
    if (ok && g_srvOf != sheet) {
        if (g_srv) { g_srv->Release(); g_srv = nullptr; }
        dev->CreateShaderResourceView(sheet, nullptr, &g_srv);
        g_srvOf = sheet;
    }
    dev->Release();
    if (!ok || !g_srv) return;
    for (int k = 0; k < 3; ++k) {   // the finished measurements, oldest first
        const int i = (g_next + k) % 3;
        if (!g_pend[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(g_st[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        uint32_t u[kCells * 3]; memcpy(u, m.pData, sizeof(u)); ctx->Unmap(g_st[i], 0);
        g_pend[i] = false;
        float dark[kCells]; int coloured = 0, n = 0;
        for (int c = 0; c < kCells; ++c) {
            if (!u[c * 3 + 2]) continue;
            const float cnt = static_cast<float>(u[c * 3 + 2]);
            const float a = u[c * 3] / cnt / 255.0f, l = u[c * 3 + 1] / cnt / 255.0f;
            dark[n++] = a - l > 0.0f ? a - l : 0.0f;
            if (l > 0.1f) ++coloured;
        }
        if (n < kCells / 2) continue;
        std::sort(dark, dark + n);
        const float f = dark[n * 15 / 100];
        const float prev = g_fade.exchange(f);
        const bool hide = f > 0.04f && coloured * 20 < n;   // under 5 % of the cells carry anything bright: a pure fade
        const bool was = g_hidePanel.exchange(hide);
        if ((f > 0.04f) != (prev > 0.04f) || hide != was)
            BVR_LOG("fadesheet: the game's fade %s (darkness %.2f, coloured cells %d of %d)%s", f > 0.04f ? "IS ON over the whole stage" : "ended",
                    f, coloured, n, hide ? " - HUD panel hidden, the whole view fades" : "");
    }
    int slot = -1;
    for (int k = 0; k < 3 && slot < 0; ++k) if (!g_pend[(g_next + k) % 3]) slot = (g_next + k) % 3;
    if (slot < 0) return;
    g_next = (slot + 1) % 3;
    D3D11_TEXTURE2D_DESC d{}; sheet->GetDesc(&d);
    const UINT bandH = d.Width * 9 / 16 < d.Height ? d.Width * 9 / 16 : d.Height, y0 = (d.Height - bandH) / 2;
    const UINT cb[4] = {d.Width, d.Height, y0, bandH};
    ID3D11DeviceContext1* c1 = nullptr; ID3DDeviceContextState* prev = nullptr;
    if (g_own && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1)
        c1->SwapDeviceContextState(g_own, &prev);   // our own state: the game's bindings are untouched
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    const UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(g_uav, zero);
    ctx->CSSetShader(g_cs, nullptr, 0); ctx->CSSetShaderResources(0, 1, &g_srv); ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->Dispatch((d.Width / 4 + 7) / 8, (bandH / 4 + 7) / 8, 1);
    ID3D11ShaderResourceView* nsv = nullptr; ID3D11UnorderedAccessView* nuv = nullptr;
    ctx->CSSetShaderResources(0, 1, &nsv); ctx->CSSetUnorderedAccessViews(0, 1, &nuv, nullptr);
    if (c1) { if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); } c1->Release(); }
    ctx->CopyResource(g_st[slot], g_buf);
    g_pend[slot] = true;
}
} // namespace fs

ID3D11Texture2D* hud_provider(ID3D11DeviceContext* ctx) {
    // AFW (V11, JJ: "HUD in AFW mode is non-existent"): the panel was handed the sheet only while OUR view was listed;
    // in AFW there is no second view, so the HUD panel never showed.
    if (!g_hudOn.load() || !g_stereo.load() || (!g_ownView.load() && g_afwPresent.eye < 0)) return nullptr;
    if (fs::g_hidePanel.load(std::memory_order_relaxed)) return nullptr;   // FADESHEET: a pure fade - the whole view fades instead
    ID3D11Texture2D* t = g_hudSheet.load();
    if (!t) return nullptr;
    g_hudGiven.fetch_add(1, std::memory_order_relaxed);
    if (!ctx) return t;
    D3D11_TEXTURE2D_DESC sd{};
    t->GetDesc(&sd);
    if (g_hudSrcOf != t) {
        if (g_hudSrcSrv) { g_hudSrcSrv->Release(); g_hudSrcSrv = nullptr; }
        D3D11_TEXTURE2D_DESC od{};
        if (g_hudOut) g_hudOut->GetDesc(&od);
        if (g_hudOut && (od.Width != sd.Width || od.Height != sd.Height)) {
            g_hudOutRtv->Release(); g_hudOutRtv = nullptr; g_hudOut->Release(); g_hudOut = nullptr;
        }
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return t;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : sd.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(t, &vd, &g_hudSrcSrv);
        if (!g_hudOut) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sd.Width; td.Height = sd.Height; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_hudOut))) dev->CreateRenderTargetView(g_hudOut, nullptr, &g_hudOutRtv);
        }
        dev->Release();
        g_hudSrcOf = t;
        BVR_LOG("hud: panel copy with repaired alpha %s (%ux%u)", g_hudOutRtv && g_hudSrcSrv ? "ready" : "FAILED - raw sheet",
                sd.Width, sd.Height);
    }
    if (!g_hudOutRtv || !g_hudSrcSrv) return t;
    const int mode = g_hudAlpha.load() < 0 ? g_hudAlphaSeen.load() : g_hudAlpha.load();
    const bool ok = mode == 1 ? blit::alpha_inverted(ctx, g_hudOutRtv, g_hudSrcSrv, sd.Width, sd.Height)
                              : blit::alpha_repair_full(ctx, g_hudOutRtv, g_hudSrcSrv, sd.Width, sd.Height);
    return ok ? g_hudOut : t;
}

// HUDALPHA measurement: the sheet read back without stalling (copy now, map with DO_NOT_WAIT on later presents), every
// 8th pixel. "Empty" pixels (no colour) show what the sheet is cleared to: alpha high there = it stores what the scene
// keeps -> inverted. Logged 3 times 5 s apart, then every 60 s while the HUD is shown.
void hud_measure(ID3D11DeviceContext* ctx) {
    static ID3D11Texture2D* stg = nullptr;
    static bool copied = false;
    static ULONGLONG next = 0;
    static int runs = 0;
    ID3D11Texture2D* t = g_hudSheet.load();
    const ULONGLONG now = GetTickCount64();
    if (!copied) {
        // V12 (JJ: "a big black window in front of my field of view" in AFW): measured only while OUR view was listed - never in
        // AFW, so the panel kept the inverted default and its empty (clear) areas showed solid black
        if (!t || !g_hudOn.load() || !g_stereo.load() || (!g_ownView.load() && g_afwPresent.eye < 0) || now < next) return;
        D3D11_TEXTURE2D_DESC sd{}, gd{};
        t->GetDesc(&sd);
        if (stg) stg->GetDesc(&gd);
        if (stg && (gd.Width != sd.Width || gd.Height != sd.Height || gd.Format != sd.Format)) { stg->Release(); stg = nullptr; }
        if (!stg) {
            gd = sd; gd.MipLevels = 1; gd.ArraySize = 1; gd.SampleDesc = {1, 0}; gd.Usage = D3D11_USAGE_STAGING;
            gd.BindFlags = 0; gd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; gd.MiscFlags = 0;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) { dev->CreateTexture2D(&gd, nullptr, &stg); dev->Release(); }
            if (!stg) { next = now + 60000; return; }
        }
        ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, t, 0, nullptr);
        copied = true;
        return;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (ctx->Map(stg, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return; // still drawing: next present
    D3D11_TEXTURE2D_DESC gd{};
    stg->GetDesc(&gd);
    uint64_t n = 0, empty = 0, emptyA = 0, opaque = 0, clear = 0, coloured = 0;
    for (UINT y = 0; y < gd.Height; y += 8) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < gd.Width; x += 8) {
            const uint8_t* p = row + x * 4;
            const uint8_t mx = (std::max)(p[0], (std::max)(p[1], p[2]));
            ++n;
            if (mx <= 4) { ++empty; emptyA += p[3]; }
            if (mx > 25) ++coloured;
            if (p[3] >= 250) ++opaque;
            if (p[3] <= 5) ++clear;
        }
    }
    ctx->Unmap(stg, 0);
    copied = false;
    ++runs;
    next = now + (runs < 3 ? 5000 : 60000);
    if (!n) return;
    const double ea = empty ? static_cast<double>(emptyA) / static_cast<double>(empty) / 255.0 : -1.0;
    const int was = g_hudAlphaSeen.load();
    if (empty * 4 >= n) g_hudAlphaSeen.store(ea > 0.5 ? 1 : 0);
    BVR_LOG("hud: sheet measured - empty pixels %.0f%% with mean alpha %.2f, coloured %.1f%%, alpha >= 0.98 on %.0f%%, <= 0.02 "
            "on %.0f%% -> alpha %s%s (mode %d)",
            100.0 * empty / n, ea, 100.0 * coloured / n, 100.0 * opaque / n, 100.0 * clear / n,
            g_hudAlphaSeen.load() ? "INVERTED (stores what the scene keeps)" : "as stored (coverage)",
            was != g_hudAlphaSeen.load() ? " - CHANGED" : "", g_hudAlpha.load());
}

// ---- eye feed (STEREO3D): one present carries the pair - left = the main view's picture, right = ours ----
std::vector<std::pair<ID3D11Texture2D*, ID3D11ShaderResourceView*>> g_picSrv;
std::vector<std::pair<ID3D11Texture2D*, ID3D11RenderTargetView*>> g_eyeRtv;
std::atomic<uint32_t> g_fedL{0}, g_fedR{0}, g_feedMiss{0};
ID3D11RenderTargetView* eye_rtv(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst) {
    for (auto& p : g_eyeRtv) if (p.first == dst) return p.second;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateRenderTargetView(dst, &rd, &rtv))) dev->CreateRenderTargetView(dst, nullptr, &rtv);
    dev->Release();
    if (rtv) g_eyeRtv.push_back({dst, rtv});
    return rtv;
}
// M8: this eye's marker layer over an eye image that is already finished (AFW: after the warp)
void markers_over(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, int eye) {
    ID3D11ShaderResourceView* mk = markers::layer(eye);
    ID3D11RenderTargetView* rtv = mk ? eye_rtv(ctx, dst) : nullptr;
    if (!rtv) return;
    D3D11_TEXTURE2D_DESC dd{}; dst->GetDesc(&dd);
    bvr::blit::alpha_premul(ctx, rtv, mk, dd.Width, dd.Height);
}
bool copy_picture(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, int eye = -1) {
    if (!src || !dst) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    for (auto& p : g_picSrv) if (p.first == src) srv = p.second;
    if (!srv) {
        D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = sd.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM : sd.Format;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        if (SUCCEEDED(dev->CreateShaderResourceView(src, &v, &srv)) && srv) g_picSrv.push_back({src, srv});
    }
    ID3D11RenderTargetView* rtv = nullptr;
    for (auto& p : g_eyeRtv) if (p.first == dst) rtv = p.second;
    if (!rtv) {
        // The game's picture holds display (gamma) values: written as they are through a UNORM view of the headset's
        // typeless image, the runtime's sRGB reading gives the same brightness (no double encode, FAIL-STR-033).
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(dst, &rd, &rtv))) dev->CreateRenderTargetView(dst, nullptr, &rtv);
        if (rtv) g_eyeRtv.push_back({dst, rtv});
    }
    dev->Release();
    if (!srv || !rtv) return false;
    D3D11_TEXTURE2D_DESC dd{}; dst->GetDesc(&dd);
    const bool ok = bvr::blit::shift_copy(ctx, rtv, dd.Width, dd.Height, srv, 0.0f);
    // M8: this eye's world markers, drawn as cards standing at their objects, laid over the picture. The layer holds
    // premultiplied colour and real coverage (markers.cpp sets that blend), so ONE / INV_SRC_ALPHA is the right over.
    if (ok && eye >= 0 && markers::any_this_frame())
        if (ID3D11ShaderResourceView* mk = markers::layer(eye))
            bvr::blit::alpha_premul(ctx, rtv, mk, dd.Width, dd.Height);
    if (ok && eye >= 0) markers::probe_eye_image(ctx, dst, eye);   // V12v EYEPROBE
    return ok;
}
// DLSS (M6): the jitter this frame's render was given, and the one the finished pair was rendered with (a frame
// older - the pair in hand at Present was drawn with the offset set at the PREVIOUS Present).
float g_jitNext[2] = {}, g_jitPair[2] = {};
std::atomic<bool> g_depthCheckReq{false};
std::atomic<int> g_depthCheck{0};
unsigned long long g_jitFrame = 0;
std::atomic<uint32_t> g_dlssUp[2] = {}, g_dlssPlain[2] = {};
// Each eye's finished picture through its own DLSS feature; null (and the plain picture) until the inputs are there.
// DLSSSTABLE (V13z14, JJ: left eye black except for the HUD, again; log 01:31:17.8 / 20.3 - the LEFT feature was built and
// evaluated 2.5 s before the right eye had a picture, i.e. while the second view was still settling). DLSS starts only once
// the pair has been up for 3 s (both features built together, histories reset at that moment) - the MVREADY rule extended
// to "never before the stereo inputs are steady".
std::atomic<bool> g_dlssReady{false};
ID3D11Texture2D* upscaled(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* pic) {
    if (!pic || dlss::mode() <= 0 || !g_dlssReady.load(std::memory_order_relaxed)) return nullptr;
    dlss::note_depth(eye, picsplit::eye_depth(eye));
    float vp[16]; float screen[4];
    const bool vpOk = picsplit::view_proj(eye, vp, screen);
    ID3D11Texture2D* up = dlss::eval(ctx, eye, pic, g_jitPair[0], g_jitPair[1], vpOk ? vp : nullptr, vpOk);
    (up ? g_dlssUp[eye] : g_dlssPlain[eye]).fetch_add(1, std::memory_order_relaxed);
    return up;
}
// AFW: the frame's one view drew `eye`; its matrix is view 0's, its depth the Present's copy, its DLSS feature that eye's own.
ID3D11Texture2D* upscaled_afw(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* pic, ID3D11Resource* depth) {
    if (!pic || dlss::mode() <= 0) return nullptr;
    dlss::note_depth(eye, depth);
    float vp[16]; float screen[4];
    const bool vpOk = picsplit::view_proj(0, vp, screen);
    ID3D11Texture2D* up = dlss::eval(ctx, eye, pic, g_jitPair[0], g_jitPair[1], vpOk ? vp : nullptr, vpOk);
    (up ? g_dlssUp[eye] : g_dlssPlain[eye]).fetch_add(1, std::memory_order_relaxed);
    return up;
}
// The eye pictures come from PICSPLIT (picsplit.h): the shared final picture copied in GPU order after each view.
std::atomic<int> g_drawTwice{0};
std::atomic<uint32_t> g_drawTwiceFrames{0}, g_drawTwiceFaults{0};
ID3D11Texture2D* g_drawPic[2] = {nullptr, nullptr};
std::mutex g_drawPicMutex;
ID3D11Texture2D* draw_pic(int slot) {
    std::lock_guard<std::mutex> lk(g_drawPicMutex);
    return g_drawTwice.load() ? g_drawPic[slot] : nullptr;
}
void mul4d(const double a[16], const double b[16], double o[16]) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { double v = 0; for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = v; }
}
bool eye_capture(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* dst, ID3D11Texture2D*) {
    if (g_stereo.load() && g_afwPresent.eye >= 0) {   // AFW: this Present shows the eye the game's view drew
        if (eye != g_afwPresent.eye) return false;
        ID3D11Resource* depth = nullptr;
        ID3D11Texture2D* pic = picsplit::afw_grab(ctx, &depth);
        if (!pic) return false;
        g_afwDepth = depth;
        D3D11_TEXTURE2D_DESC pd{}; pic->GetDesc(&pd);
        g_afwRW = pd.Width; g_afwRH = pd.Height;
        ID3D11Texture2D* up = upscaled_afw(ctx, eye, pic, depth);
        const bool ok = copy_picture(ctx, up ? up : pic, dst, -1);   // markers go on after the warp (afw_synth)
        (ok ? (eye ? g_fedR : g_fedL) : g_feedMiss).fetch_add(1, std::memory_order_relaxed);
        return ok;
    }
    if (!g_stereo.load() || eye != 0 || !g_ownView.load()) return false;
    ID3D11Texture2D* pic = draw_pic(0);
    if (!pic) pic = picsplit::left();
    // N1 (2026-11-01): never evaluate the left eye alone - on a room-screen frame the core still copies into the left eye
    // (its picture is then replaced by the room screen), which gave the LEFT feature extra evaluations the right never got
    // (warm-up 205 vs 102). While the room screen shows, LOADFADE2's warm-up evaluates both eyes evenly instead.
    if (!vr::screen_shown_now())
        if (ID3D11Texture2D* up = upscaled(ctx, 0, pic)) pic = up;
    const bool ok = copy_picture(ctx, pic, dst, 0);
    (ok ? g_fedL : g_feedMiss).fetch_add(1, std::memory_order_relaxed);
    return ok;
}
// AFW: the other eye's camera = the drawn one's, moved by the eye separation (world shift T) and given its own lens window
// (a clip-space shift C): VP_other = T * VP_drawn * C in the row-vector form. Exact while both eyes look the same way.
// The drawn eye's matrix (U, and J with this pair's jitter) and the other eye's (D), for a frame `f`.
bool afw_matrices(const AfwFrame& f, double U[16], double J[16], double D[16]) {
    if (f.eye < 0 || !f.parallel) return false;
    float vpf[16], screen[4];
    if (!picsplit::view_proj(0, vpf, screen)) return false;
    double T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, Cm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) U[i] = J[i] = vpf[i];
    const double rw = g_afwRW ? double(g_afwRW) : 1.0, rh = g_afwRH ? double(g_afwRH) : 1.0;
    const double jxN = 2.0 * g_jitPair[0] / rw, jyN = -2.0 * g_jitPair[1] / rh;
    for (int i = 0; i < 4; ++i) { J[i * 4 + 0] += jxN * U[i * 4 + 3]; J[i * 4 + 1] += jyN * U[i * 4 + 3]; }
    for (int i = 0; i < 3; ++i) T[12 + i] = double(f.src[i]) - double(f.dst[i]);
    float ws[4], wd[4];
    offaxis::window(f.eye, ws);
    offaxis::window(f.eye ^ 1, wd);
    const double hxs = (ws[1] - ws[0]) * 0.5, cxs = (ws[1] + ws[0]) * 0.5, hys = (ws[3] - ws[2]) * 0.5, cys = (ws[3] + ws[2]) * 0.5;
    const double hxd = (wd[1] - wd[0]) * 0.5, cxd = (wd[1] + wd[0]) * 0.5, hyd = (wd[3] - wd[2]) * 0.5, cyd = (wd[3] + wd[2]) * 0.5;
    if (offaxis::on() && hxd > 1e-6 && hyd > 1e-6) {
        Cm[0] = hxs / hxd; Cm[12] = (cxs - cxd) / hxd;
        Cm[5] = hys / hyd; Cm[13] = (cys - cyd) / hyd;
    }
    double TU[16];
    mul4d(T, U, TU);
    mul4d(TU, Cm, D);
    return true;
}
bool afw_synth(ID3D11DeviceContext* ctx, int drawnEye, ID3D11Texture2D* drawn, ID3D11Texture2D* otherDst) {
    const AfwFrame& f = g_afwPresent;
    if (f.eye != drawnEye || !f.parallel || !g_afwDepth || !g_afwRW) return false;
    double U[16], J[16], D[16];
    if (!afw_matrices(f, U, J, D)) return false;
    // the other eye's depth terms: ndc z = za + zb / w for every point (a perspective lens), measured on two points
    // straight ahead of it (its forward = the matrix's w column)
    double fw[3] = {D[3], D[7], D[11]};
    const double fl = sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
    if (fl < 1e-6) return false;
    double zc[2], wc[2];
    for (int k = 0; k < 2; ++k) {
        const double dist = k ? 50.0 : 1.0;
        const double p[4] = {f.dst[0] + fw[0] / fl * dist, f.dst[1] + fw[1] / fl * dist, f.dst[2] + fw[2] / fl * dist, 1.0};
        zc[k] = wc[k] = 0;
        for (int i = 0; i < 4; ++i) { zc[k] += p[i] * D[i * 4 + 2]; wc[k] += p[i] * D[i * 4 + 3]; }
    }
    if (fabs(wc[0] - wc[1]) < 1e-6) return false;
    const double za = (zc[0] - zc[1]) / (wc[0] - wc[1]), zb = zc[0] - za * wc[0];
    const bool ok = afw::warp(ctx, drawnEye, drawn, otherDst, g_afwDepth, J, U, D, float(za), float(zb), g_afwRW, g_afwRH);
    // M8 in AFW: each eye's world markers laid over its OWN finished image after the warp (never warped: they would be
    // moved with the depth behind them, and the warp's kept picture must stay marker-free)
    if (ok && markers::any_this_frame()) { markers_over(ctx, drawn, drawnEye); markers_over(ctx, otherDst, drawnEye ^ 1); }
    return ok;
}
bool eye_synth(ID3D11DeviceContext* ctx, int drawnEye, ID3D11Texture2D* drawn, ID3D11Texture2D* otherDst) {
    if (g_stereo.load() && g_afwPresent.eye >= 0) {   // AFW: the drawn eye's picture moved into the other eye
        const bool ok = afw_synth(ctx, drawnEye, drawn, otherDst);
        (ok ? (drawnEye ? g_fedL : g_fedR) : g_feedMiss).fetch_add(1, std::memory_order_relaxed);
        return ok;
    }
    if (!g_stereo.load() || drawnEye != 0) return false;
    ID3D11Texture2D* pic = draw_pic(1);
    if (!pic) pic = picsplit::right();
    if (ID3D11Texture2D* up = upscaled(ctx, 1, pic)) pic = up;
    const bool ok = copy_picture(ctx, pic, otherDst, 1);
    (ok ? g_fedR : g_feedMiss).fetch_add(1, std::memory_order_relaxed);
    return ok;
}

// ---- DRAWTWICE (M3b): make Dark Souls III finish a picture for EACH eye ----
//
// DS3's per-view render stops at the view's own HDR; the tone-map / post / HUD chain that makes the picture you see
// is issued ONCE after the view loop and finishes whichever view rendered LAST (measured: moving our second view
// 6 m changes the window picture by 42 of 255, while the game view's own HDR does not move). That is exactly the
// playbook's "draws issued once after the eye loop reach only the last eye" (14 #two-pass-ownership), whose fix is
// "replay it into the other eye".
//
// So RendMan::Draw - the engine's own "draw everything", one level ABOVE the view loop - is called twice per frame
// with the two views' sort order swapped. Each call is a complete frame, so each produces a finished picture:
//   pass 1: our view last  -> the target holds OUR eye      -> copied to g_drawPic[1]
//   pass 2: the game view last -> the target holds ITS eye  -> copied to g_drawPic[0]
// The order is the engine's own (render-all sorts by ~priority, so a HIGHER priority renders FIRST); nothing is
// taken out of the list, which keeps the engine's own book-keeping intact.
//
// Cost note: both views are drawn in BOTH passes, so this proof costs four view renders a frame. Once it is proven,
// listing only the eye that each pass is for halves that - but that is a list change, and the list is the one thing
// AC6 and Sekiro both punished us for touching carelessly, so it comes second.

// A copy of the shared final target, made on the immediate context right after a Draw returns (its command buffers
// have been replayed by then, so the copy is correctly ordered behind them).
void grab_picture(int slot) {
    void* mv = g_mainView.load();
    ID3D11Texture2D* T = mv ? split_target(mv) : nullptr;
    if (!T) return;
    ID3D11Device* dev = nullptr;
    T->GetDevice(&dev);
    if (!dev) return;
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);
    if (imm) {
        D3D11_TEXTURE2D_DESC d{};
        T->GetDesc(&d);
        std::lock_guard<std::mutex> lk(g_drawPicMutex);
        if (g_drawPic[slot]) {
            D3D11_TEXTURE2D_DESC o{};
            g_drawPic[slot]->GetDesc(&o);
            if (o.Width != d.Width || o.Height != d.Height || o.Format != d.Format) { g_drawPic[slot]->Release(); g_drawPic[slot] = nullptr; }
        }
        if (!g_drawPic[slot]) {
            D3D11_TEXTURE2D_DESC c = d;
            c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.MiscFlags = 0; c.CPUAccessFlags = 0; c.Usage = D3D11_USAGE_DEFAULT;
            c.MipLevels = 1; c.ArraySize = 1;
            dev->CreateTexture2D(&c, nullptr, &g_drawPic[slot]);
        }
        if (g_drawPic[slot]) imm->CopyResource(g_drawPic[slot], T);
        imm->Release();
    }
    dev->Release();
}

// HDRSWAP (M3b probe): the post chain is issued ONCE after the view loop and finishes whichever view rendered
// last. Is it driven by the HDR CONTENTS it reads, or by that view's own state? Copying one view's HDR into the
// other's between the view loop and the post answers it in one run - and if the contents decide, then replaying
// the post with the other eye's HDR in place is the fix (playbook 14 #two-pass-ownership "replay it into the
// other eye").  1 = the game view's HDR into ours, 2 = ours into the game view's.
std::atomic<int> g_hdrSwap{0};
std::atomic<uint32_t> g_hdrSwaps{0};
void hdr_swap_now() {
    const int m = g_hdrSwap.load();
    void* mv = g_mainView.load(); void* ov = g_ownView.load();
    if (!m || !mv || !ov) { picsplit::set_hdr_swap(nullptr, nullptr); return; }
    static const uint32_t kHdr[3] = {0x498, 0x40, 0x18};
    ID3D11Texture2D* a = resolve_path(mv, kHdr, 3);   // the game view's HDR
    ID3D11Texture2D* b = resolve_path(ov, kHdr, 3);   // ours
    if (!a || !b || a == b) { picsplit::set_hdr_swap(nullptr, nullptr); return; }
    // The copy itself is made on the IMMEDIATE context from picsplit's replay tap: a copy issued here would land
    // before the engine's own recorded work (DS3 records on deferred contexts and replays afterwards - measured
    // 2026-10-30: a copy from this point changed the picture by 0.08 of 255, i.e. not at all).
    picsplit::set_hdr_swap(m == 1 ? a : b, m == 1 ? b : a);
    g_hdrSwaps.store(picsplit::hdr_swap_copies(), std::memory_order_relaxed);
}

void set_own_priority(uint32_t p) {
    void* v = g_ownObj;
    if (v) *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(v) + addr::kViewPriority) = p;
}

void __fastcall hkRendManDraw(void* rendMan) {
    if (!g_drawTwice.load() || !g_ownView.load() || !g_mainView.load()) { oRendManDraw(rendMan); return; }
    // pass 1: priority 0 = the same as the game view, so ours sorts LAST and the post finishes OUR eye
    set_own_priority(0);
    oRendManDraw(rendMan);
    grab_picture(1);
    // pass 2: a higher priority sorts ours FIRST, so the game view is last and the post finishes ITS eye
    set_own_priority(1);
    oRendManDraw(rendMan);
    grab_picture(0);
    set_own_priority(0);
    g_drawTwiceFrames.fetch_add(1, std::memory_order_relaxed);
}

void set_3d(bool on) {
    if (g_stereo.load() == on) return;
    g_stereo.store(on);
    if (on) {
        g_wantView.store(true);
        vr::set_eye_capture_hook(&eye_capture);
        vr::set_eye_synth_hook(&eye_synth);
        vr::sr_use_render_thread_eyes(true);
        vr::set_exact_pose_tag(true);
        vr::set_asym_fov(false); // the views draw a symmetric lens until M4 (declared = rendered, FAIL-STR-009)
        vr::set_flat_screen(false);
        vr::set_camera_mode(true);
    } else {
        vr::set_eye_capture_hook(nullptr);
        vr::set_eye_synth_hook(nullptr);
        vr::sr_use_render_thread_eyes(false);
        vr::set_camera_mode(false);
        vr::set_flat_screen(true);
    }
    BVR_LOG("stereo3d: %s", on ? "ON - left eye = the game's view, right eye = the second view" : "off");
}

// R10G10B10A2 / RGBA8 texture -> 24-bit BMP (CPU conversion through a staging copy).
// Save a texture as a 24-bit BMP. Colour targets (4 bytes a pixel) go straight out; DEPTH / shadow maps (2 or 4 bytes,
// typeless or float) are normalised between their own smallest and largest value so the picture is readable - without
// the per-format pixel size we read past the end of the row and crashed the game (2026-10-29, `ds picpath` at a
// 1024x1024 R16 shadow map).
int bmp_pixel_bytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UINT: return 2;
        default: return 4;     // every colour target we read is 32 bits a pixel
    }
}
bool bmp_is_depth(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return true;
        default: return false;
    }
}
float bmp_depth_value(const uint8_t* row, UINT x, DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM:
            return reinterpret_cast<const uint16_t*>(row)[x] / 65535.0f;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return (reinterpret_cast<const uint32_t*>(row)[x] & 0xFFFFFF) / 16777215.0f;
        default:
            return reinterpret_cast<const float*>(row)[x];
    }
}

bool save_bmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc = {1, 0};
    ID3D11Texture2D* st = nullptr;
    bool ok = false;
    const int pxBytes = bmp_pixel_bytes(d.Format);
    const bool depth = bmp_is_depth(d.Format);
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const UINT w = d.Width, h = d.Height, rowBytes = (w * 3 + 3) & ~3u;
            if (m.RowPitch < w * static_cast<UINT>(pxBytes)) {   // never read past a row
                BVR_LOG("capture: %ux%u fmt %u has a %u-byte row - not saved", w, h, d.Format, m.RowPitch);
                ctx->Unmap(st, 0); st->Release(); if (dev) dev->Release();
                return false;
            }
            float lo = 1e30f, hi = -1e30f;
            if (depth) {                                        // first pass: the range actually used
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* src = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch;
                    for (UINT x = 0; x < w; ++x) {
                        const float v = bmp_depth_value(src, x, d.Format);
                        if (v < lo) lo = v;
                        if (v > hi) hi = v;
                    }
                }
                if (hi <= lo) hi = lo + 1.0f;
            }
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"wb") == 0 && f) {
                BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + rowBytes * h;
                ih.biSize = sizeof(ih); ih.biWidth = static_cast<LONG>(w); ih.biHeight = static_cast<LONG>(h); ih.biPlanes = 1; ih.biBitCount = 24;
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
                uint8_t* row = static_cast<uint8_t*>(malloc(rowBytes));
                for (UINT y = 0; y < h && row; ++y) {
                    const uint8_t* src = static_cast<const uint8_t*>(m.pData) + (h - 1 - y) * m.RowPitch;
                    memset(row, 0, rowBytes);
                    for (UINT x = 0; x < w; ++x) {
                        uint8_t r, g, b;
                        if (depth) {
                            const float t = (bmp_depth_value(src, x, d.Format) - lo) / (hi - lo);
                            r = g = b = static_cast<uint8_t>(t < 0 ? 0 : t > 1 ? 255 : t * 255.0f);
                        } else {
                            const uint32_t p = reinterpret_cast<const uint32_t*>(src)[x];
                            if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) {
                                r = static_cast<uint8_t>((p & 0x3FF) >> 2); g = static_cast<uint8_t>(((p >> 10) & 0x3FF) >> 2); b = static_cast<uint8_t>(((p >> 20) & 0x3FF) >> 2);
                            } else { r = static_cast<uint8_t>(p); g = static_cast<uint8_t>(p >> 8); b = static_cast<uint8_t>(p >> 16); }
                        }
                        row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
                    }
                    fwrite(row, rowBytes, 1, f);
                }
                free(row);
                fclose(f);
                ok = true;
                if (depth) BVR_LOG("capture: %ux%u depth fmt %u saved, values %.6g .. %.6g", w, h, d.Format, lo, hi);
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    if (dev) dev->Release();
    return ok;
}

// `at` is a preferred-base address from addresses.h; DS3 has ASLR, so every one of them is rebased here.
bool hook(uintptr_t at, void* detour, void** orig, const char* name) {
    void* live = reinterpret_cast<void*>(addr::rebase(at));
    const MH_STATUS s = MH_CreateHook(live, detour, orig);
    if (s != MH_OK) { BVR_LOG("stereo: hook %s at %p FAILED (%s)", name, live, MH_StatusToString(s)); return false; }
    MH_QueueEnableHook(live);
    return true;
}

void* rendman() {
    __try {
        void* rm = *addr::ptr<void*>(addr::RendManSlot);
        if (rm && *reinterpret_cast<uintptr_t*>(rm) == addr::rebase(addr::RendManVtable)) return rm;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

} // namespace

void hang_watch();
std::atomic<int> g_loadClean{1};
ULONGLONG g_feWasArmedMs = 0;
ULONGLONG g_fadeInMs = 0;      // LOADFADE: when the fade into 3D began (0 = none)
std::atomic<int> g_selfCheck{-1};   // 10-s stereo self-check (`ds selfcheck 0|1`, settings self_check, default 0)
std::atomic<int> g_autoRec{0};     // LOADREC + AUTOREC recorders (`ds autorec 0|1`, settings auto_rec, default 0)
std::atomic<int> g_loadFade2{1};   // LOADFADE2 (`ds loadfade2 0|1`, settings load_fade2)
int g_fadeOutMs = 800, g_fadeBlackMs = 250, g_fadeHoldMs = 250, g_dlssWarmEvals = 30;
int g_fadeLenMs = 1200;        // LOADFADE length (settings load_fade_ms)   // the last time the loading screen counted (TRAVELCLEAN black for 400 ms after it)   // LOADSCREEN / TRAVELCLEAN2 (`ds loadclean 0|1`, settings load_clean)
bool install() {
    { static bool hw = false; if (!hw) { hw = true; hang_watch(); } }
    g_on = settings::i("stereo_probe", 1) != 0;
    if (const int d = settings::i("pic_depth", 0); d > 0 && d <= 4) {
        std::lock_guard<std::mutex> lk(g_picMutex);
        g_picDepth = d;
        g_picOff[0] = static_cast<uint32_t>(settings::i("pic_off0", 0)); g_picOff[1] = static_cast<uint32_t>(settings::i("pic_off1", 0));
        g_picOff[2] = static_cast<uint32_t>(settings::i("pic_off2", 0)); g_picOff[3] = static_cast<uint32_t>(settings::i("pic_off3", 0));
        sprintf_s(g_picPath, "rt+%X+%X+%X", g_picOff[0], g_picOff[1], g_picOff[2]);
        BVR_LOG("stereo3d: eye picture path from settings: %s (depth %d)", g_picPath, d);
    }
    int ok = 0;
    ok += hook(addr::RenderAllViews, reinterpret_cast<void*>(&hkRenderAll), reinterpret_cast<void**>(&oRenderAll), "render-all-views");
    ok += hook(addr::AddView, reinterpret_cast<void*>(&hkAddView), reinterpret_cast<void**>(&oAddView), "AddView");
    ok += hook(addr::RemoveView, reinterpret_cast<void*>(&hkRemoveView), reinterpret_cast<void**>(&oRemoveView), "RemoveView");
    ok += hook(addr::SceneViewCtor, reinterpret_cast<void*>(&hkSceneCtor), reinterpret_cast<void**>(&oSceneCtor), "scene-view ctor");
    // DRAWTWICE (M3b): the engine's own "draw everything", one level above the view loop
    hook(addr::RendManDraw, reinterpret_cast<void*>(&hkRendManDraw), reinterpret_cast<void**>(&oRendManDraw), "RendMan::Draw");
    // DRAWTWICE is a dead end, kept only as a command for a one-off test: calling RendMan::Draw twice in a frame
    // faults inside a fixed per-frame array that has run out (DarkSoulsIII.exe+0xCF0DC, `mov [rcx], rdx` right after
    // `jge` past the bounds check) within a second - the same non-re-entrancy AC6 and Sekiro both hit. It is never
    // restored from settings, so a test can never arm it for the next launch.
    g_drawTwice.store(0);
    markers::install();   // M8: the marker projection, queued with the others
    envtime::install();   // L7: queued with the others
    envtime::share(settings::i("env_share", 0));
    envtime::once(settings::i("env_once", 1));   // L7: ON by default - fixes the left eye flickering between two lightings
    loadscreen::install();   // LOADSCREEN: the game's loading screen drawn = keep the room screen
    g_loadClean.store(settings::i("load_clean", 1));
    g_fadeLenMs = settings::i("load_fade_ms", 1200);
    g_loadFade2.store(settings::i("load_fade2", 1));
    g_autoRec.store(settings::i("auto_rec", 0));
    g_fadeOutMs = settings::i("load_fadeout_ms", 800);
    picsplit::hazefill(settings::f("haze_fill", 0.0f));   // V13z8: OFF - the vs it targets draws the loading screen, not the bonfire smoke
    g_menuViewOk.store(settings::i("menu_view", 1));
    picsplit::smokefill(settings::f("smoke_fill", 0.0f));
    picsplit::smokelock(settings::i("smoke_lock", 0));   // V13z12: OFF - the per-eye shift sheared (JJ)
    picsplit::smokeface(settings::i("smoke_face", 2));   // V13z16: smoke cards face the game camera (JJ: "the smoke stays put now")
    picsplit::smokescale(settings::f("smoke_scale", 2.5f));   // V13z17: and larger
    vr::set_screen_band(settings::f("screen_band", 16.0f / 9.0f));   // SCREENBAND: the loading screen's own 16:9 band only
    const MH_STATUS ap = MH_ApplyQueued();
    g_installed = ok == 4 && ap == MH_OK;
    void* rm = rendman();
    // L5 + SHADOWX (2026-10-30): our view's lighting uses the main view's shadows - at Shadow Quality MAX the main view's own
    // 4096 shadow copy with every cascade matrix re-based (mode 3), below MAX the sun maps at t32 (mode 1). New key, because
    // test runs had saved shadow_lend=1.
    picsplit::hud_redirect_set(settings::i("hud_sheet", 1));   // HUDSHEET: the HUD on the floating panel by default
    g_expoShare.store(settings::i("exposure_share", 1));   // EXPOSHARE on by default: without it the second eye is black
    picsplit::set_pair_mode(settings::i("pair_mode", 4));   // DS3: left = after the last view-tagged write, right = after the last
    picsplit::shadow_lend(settings::i("shadow_share", 0));   // DS3: the sun atlas is shared by both views - nothing to lend (M4L)
    picsplit::shadow_sharp(settings::f("shadow_sharp", 1.0f));   // SHADOWSHARP: panel "shadow edges"
    contact::load();   // CONTACT: ray-traced contact shadows (settings contact_shadows / contact_len / contact_strength)
    picsplit::vol_lend(settings::i("vol_lend", 0));         // L7: ... and its volumetric light (compute t3) too
    picsplit::hist_swap(settings::i("hist_swap", 0));       // L7: each view keeps its own frame-to-frame buffers
    rtclass_load(true);   // RTPRIME: the measured classes, if a run has saved them; our view is primed at every listing
    // ALWAYS3D (JJ 2026-10-29: "there's no need for the true 3D option anymore ... it's now working"): on from the start;
    // the second view is built when gameplay lists the main view, menus and loads stay on the screen in the room.
    // stereo3d=0 in settings.ini / "ds 3d 0" remain for tests.
    // DS3 (2026-10-30): a second engine view renders a complete scene of its own, but Dark Souls III's post /
    // composite chain runs ONCE a frame and only ever finishes the game view's picture (measured: splitdump with the
    // second view moved 4 m shows no change in the shared target). Until that is solved (DS3VR2_PLAN "M3 - where DS3
    // differs from Sekiro") the second view only costs half the frame rate, so it is OFF by default.
    // 3D is ON by default since the second view's lighting was matched (RTCLASS) and parallax was measured to grow
    // with the eye separation (2026-10-30). stereo3d = 0 in settings.ini remains for tests.
    if (g_installed && settings::i("stereo3d", 1)) set_3d(true);
    BVR_LOG("stereo: STEREOPROBE-1 %s (%d of 4 hooks, apply %s), RendMan %p%s, probe logging %s",
            g_installed ? "installed" : "INCOMPLETE", ok, MH_StatusToString(ap), rm,
            rm ? "" : " (not created yet or slot wrong - checked again each 2 s)", g_on ? "on" : "off");
    return g_installed;
}

// HANGWATCH (V9, JJ 2026-10-31: "game just hung on me when I went into the VR overlay panel" - presents 0 after the core's
// HUD capture target was made, no dump: a hang is not a crash). If gameplay stops presenting for 8 s, one dump of every
// thread (stacks + the memory they point at) goes to DS3VR2\crash\hang_<time>.dmp, so the next hang names its place.
std::atomic<uint64_t> g_hwPresents{0};
std::atomic<bool> g_hwArmed{false};
void hang_watch() {
    std::thread([] {
        uint64_t last = 0; int still = 0; bool dumped = false;
        for (;;) {
            Sleep(1000);
            const uint64_t now = g_hwPresents.load(std::memory_order_relaxed);
            if (now != last || !g_hwArmed.load(std::memory_order_relaxed)) { last = now; still = 0; continue; }
            if (++still < 8 || dumped) continue;
            dumped = true;
            wchar_t path[MAX_PATH];
            SYSTEMTIME st{}; GetLocalTime(&st);
            swprintf_s(path, L"%s\\crash", bvr::log::data_dir());
            CreateDirectoryW(path, nullptr);
            swprintf_s(path, L"%s\\crash\\hang_%02u%02u%02u.dmp", bvr::log::data_dir(), st.wHour, st.wMinute, st.wSecond);
            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            BOOL ok = FALSE;
            if (f != INVALID_HANDLE_VALUE) {
                ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                       static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                                  MiniDumpWithUnloadedModules), nullptr, nullptr, nullptr);
                CloseHandle(f);
            }
            BVR_LOG("hangwatch: no Present for 8 s in gameplay - dump %ls %s", path, ok ? "written" : "FAILED");
        }
    }).detach();
}

void on_present(IDXGISwapChain* sc) {
    g_hwPresents.fetch_add(1, std::memory_order_relaxed);
    g_hwArmed.store(g_ownView.load() != nullptr || g_afwFrame.eye >= 0, std::memory_order_relaxed);   // gameplay only (loads may pause)
    learn_vtables(sc);
    {   // KEEPFOCUS: the game's own window, once
        static bool focusDone = false;
        DXGI_SWAP_CHAIN_DESC d{};
        if (!focusDone && sc && SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow) { focus::attach(d.OutputWindow); focusDone = true; }
    }
    if (g_stereo.load()) {
        // This present carries the pair (left drawn = the main view, right = ours, built in the synth hook). While our view
        // is not listed (loads, travel, title) the game is shown on the screen in the room instead.
        // AFW: no second view; the Present carries the eye the game's view drew this frame and the warp builds the other.
        g_afwPresent = afw::on() ? g_afwFrame : AfwFrame{};
        const bool afwPair = g_afwPresent.eye >= 0;
        {   // M8 in AFW: the drawn eye's matrix is the game view's, the other eye's the warp's derived camera
            double U[16], J[16], D[16];
            if (afwPair && afw_matrices(g_afwPresent, U, J, D)) {
                float u[16], d[16];
                for (int i = 0; i < 16; ++i) { u[i] = float(U[i]); d[i] = float(D[i]); }
                markers::set_eye_vp(g_afwPresent.eye, u);
                markers::set_eye_vp(g_afwPresent.eye ^ 1, d);
            } else {
                markers::set_eye_vp(-1, nullptr);
            }
        }
        const bool pair = g_ownView.load() != nullptr || afwPair;
        // LOADSCREEN + TRAVELCLEAN2 (ported from SKVR2, `ds loadclean 0|1`): while the game DRAWS its loading screen (incl. its
        // fade-out) the world behind it is half streamed - keep the room screen until it has not been drawn for 150 ms, then
        // 3D in one switch; the room screen shows BLACK (not the game window in a box) whenever it is up with the 3D world
        // already active and the loading screen not drawn this frame (it ended < 400 ms ago, or the camera is stale).
        loadscreen::retry();
        // V13z8: the movie-draw hook is impossible on DS3 (never executable); the loading signal is FESIGNAL - the game's
        // full-screen Scaleform screen drawn into the picture - counted only when it was ALREADY drawing before the 3D pair
        // came back (the loading screen fading out over the newly built world), never one that starts during play.
        const bool clean = g_loadClean.load(std::memory_order_relaxed) != 0;
        // LOADSTRICT (V13z9, JJ: "the 3D game is still coming in before the loading screen has disappeared"; log 01:09:38-45:
        // the loading screen is NOT drawn every frame - gaps up to 2 s, when the room screen showed the half-built world).
        // From the loading screen's first frame (no 3D yet) until 3D: the room screen is held, and BLACK in every frame the
        // loading screen is not drawn; 3D only once it has been gone for a full second.
        static bool loadSeq = false;
        const ULONGLONG feSince = picsplit::fe_ms_since();
        const bool feFrame = feSince < 40;
        // V13z10 (JJ: "the main menu screen is now going black after pressing the first button" - the title menu is drawn by
        // the same full-screen shader): the sequence only starts once the game's WORLD view exists but our second eye is not
        // listed yet (the world being built behind the loading screen) and the loading screen was drawn in the last 3 s. The
        // title / main menu has no world view, so it is never touched.
        if (!pair && g_mainView.load() && feSince < 3000) loadSeq = true;
        // V13z11 (JJ: "when the game appears, there's a small black window still briefly ... stay full screen black for another
        // second or two"): the sequence ends 2.5 s after the loading screen's last frame (black all along), then straight to 3D.
        // LOADFADE OUT (V13z16, JJ: the loading screen "disappeared too quickly without fading out to a black screen"): once the
        // loading screen has been gone 2.5 s with the pair up, the frozen loading screen first fades to black (g_fadeLenMs / 2),
        // then 3D starts under full black and fades in (LOADFADE).
        // V13z17 (JJ: "the loading screen is disappearing too quickly ... a black screen for a little bit too long ... then very
        // briefly I see the loading screen appear for a frame or two before it enters into the game"): the game fades its own
        // loading screen out, so ours is dropped; the hold after its last frame is 1 s (was 2.5 s + 0.6 s fade); at the end
        // the room screen is BLACK for the last frames (LOADEND) so no live frame of the window shows before 3D takes over.
        static ULONGLONG endMs = 0;
        // LOADFADE2 (2026-11-01 overnight, from the eye-burst 01:55:57 frame by frame): (1) the game CUTS its loading screen
        // (its own picture 4.2 -> 2.0 in one frame, no fade), and the 40 ms grace let 2-3 of those dark frames through before
        // the freeze - so the freeze held a DARK frame: "disappears too abruptly". Now every frame without a loading-screen
        // draw keeps the last frame that had one. (2) Our own fade-out over that frozen loading screen (g_fadeOutMs), a short
        // black, then 3D under full black. (3) The game's first world frame is WHITE (its exposure not adapted, brightness 207)
        // and the fade layer was only created 29 ms after 3D began, so that frame showed: the layer is now made at the first
        // frame of the session and 3D stays under full black for g_fadeHoldMs before fading in. (4) DLSS is warmed up on the
        // pair while the room screen is still up (both eyes evaluated every frame, results not shown) and the fade-out only
        // starts once both eyes have g_dlssWarmEvals evaluations (or 3 s passed) - no visible switch after 3D appears.
        static ULONGLONG goneMs = 0;
        bool dlssWarm = true;
        if (g_loadFade2.load(std::memory_order_relaxed) && loadSeq && pair && dlss::mode() > 0) {
            if (!goneMs && feSince > 1000) goneMs = GetTickCount64();
            dlssWarm = (dlss::evals(0) >= g_dlssWarmEvals && dlss::evals(1) >= g_dlssWarmEvals) ||
                       (goneMs && GetTickCount64() - goneMs > 3000);
        }
        static float vigLoad = 0.0f;   // the loading fades' value; TRAVELFADE's is combined with it below
        if (!loadSeq) goneMs = 0;
        if (g_loadFade2.load(std::memory_order_relaxed)) {
            if (loadSeq && pair && feSince > 1000 && (dlssWarm || endMs)) {
                if (!endMs) { endMs = GetTickCount64(); BVR_LOG("loadseq: the loading screen has been gone 1 s - fading it out (DLSS evaluations L %d R %d)", dlss::evals(0), dlss::evals(1)); }
                const ULONGLONG el = GetTickCount64() - endMs;
                vr::set_vignette_look(1.0f, -30.0f, 1.0f);
                vigLoad = (el >= static_cast<ULONGLONG>(g_fadeOutMs) ? 1.0f : static_cast<float>(el) / static_cast<float>(g_fadeOutMs));
                if (el >= static_cast<ULONGLONG>(g_fadeOutMs + g_fadeBlackMs)) {
                    loadSeq = false; endMs = 0; g_fadeInMs = GetTickCount64();
                    BVR_LOG("loadseq: 3D starts under black (DLSS evaluations L %d R %d), fading in after %d ms", dlss::evals(0), dlss::evals(1), g_fadeHoldMs);
                }
            } else if (!(loadSeq && pair)) { if (endMs) vigLoad = (0.0f); endMs = 0; }
        } else if (loadSeq && pair && feSince > 1000) {
            if (!endMs) { endMs = GetTickCount64(); BVR_LOG("loadseq: the loading screen has been gone 1 s - black, then 3D"); }
            if (GetTickCount64() - endMs >= 150) { loadSeq = false; endMs = 0; g_fadeInMs = GetTickCount64(); BVR_LOG("loadseq: 3D starts (fading in)"); }
        } else endMs = 0;
        // LOADFADE (V13z12, JJ: "quite abrupt ... a black screen straight into the game, maybe it should fade into the game"):
        // after the hold, a full-view black layer (the core's vignette with no clear centre) fades out over g_fadeLenMs.
        if (g_fadeInMs) {
            const ULONGLONG el = GetTickCount64() - g_fadeInMs;
            const ULONGLONG hold = g_loadFade2.load(std::memory_order_relaxed) ? static_cast<ULONGLONG>(g_fadeHoldMs) : 0;
            if (el >= hold + static_cast<ULONGLONG>(g_fadeLenMs) || !pair) { vigLoad = (0.0f); g_fadeInMs = 0; }
            else {   // clear centre -30 deg = none: solid black
                vr::set_vignette_look(1.0f, -30.0f, 1.0f);
                vigLoad = (el < hold ? 1.0f : 1.0f - static_cast<float>(el - hold) / static_cast<float>(g_fadeLenMs));
            }
        }
        // TRAVELFADE (2026-11-01, JJ: "when traveling there's a black window that appears before the loading screen"; Sekiro had
        // the same - the game's fade to black is a 16:9 box on the HUD sheet = a dark window on our panel, memory
        // game-fade-lands-on-hud-panel). Pieces named by `ds hudfade <hex>` are kept off the panel (picsplit); while they draw
        // in 3D our full-view fade follows to black (400 ms), stays black through the room screen until the loading screen is
        // drawn, then lets it fade in (500 ms). If the game's fade stops while the world stays up, the view comes back (300 ms).
        {
            static float tf = 0.0f; static bool tOn = false; static ULONGLONG tLast = 0;
            const ULONGLONG now = GetTickCount64();
            const float dt = static_cast<float>(tLast ? (now - tLast > 100 ? 100 : now - tLast) : 0); tLast = now;
            const ULONGLONG fs = picsplit::fade_ms_since();
            const float sheetFade = fs::g_fade.load(std::memory_order_relaxed);   // FADESHEET (3D only)
            if ((fs < 200 || sheetFade > 0.04f) && pair && !loadSeq && !tOn) { tOn = true; BVR_LOG("travelfade: the game is fading - the whole view fades instead of a box on the panel"); }
            if (tOn) {
                if (sheetFade > 0.04f && pair && !loadSeq) tf = sheetFade;   // follow the game's own darkness, both ways
                else if (fs < 200) tf = tf + dt / 400.0f > 1.0f ? 1.0f : tf + dt / 400.0f;
                else if (!pair || loadSeq) { if (feSince < 100) tf = tf - dt / 500.0f < 0.0f ? 0.0f : tf - dt / 500.0f; }
                else tf = tf - dt / 300.0f < 0.0f ? 0.0f : tf - dt / 300.0f;
                if (tf <= 0.0f && fs >= 200) { tOn = false; BVR_LOG("travelfade: done (%s)", pair && !loadSeq ? "back to the world" : "the loading screen shows"); }
            }
            const float v = tf > vigLoad ? tf : vigLoad;
            if (v > 0.0f) vr::set_vignette_look(1.0f, -30.0f, 1.0f);
            vr::set_vignette(v);
        }
        const bool feArmed = loadSeq;
        const bool loading = clean && loadSeq;
        {
            static bool was = false;
            if (loading != was) { was = loading; BVR_LOG("loadscreen: the game's loading screen %s", loading ? "IS DRAWN - room screen held" : "gone - 3D allowed"); }
        }
        vr::set_flat_screen(!pair || loading);
        const bool loadNow = feFrame;
        // LOADFREEZE (V13z15, JJ: "the loading screen is disappearing too quickly ... a few flickers"): during the sequence a
        // frame WITHOUT the loading screen keeps the last frame that had it, so the loading screen stays steadily up until the
        // sequence ends - no black / loading-screen toggling, no world. The fade from black (LOADFADE) follows as before.
        vr::set_screen_black(clean && loadSeq && endMs != 0 && (!g_loadFade2.load(std::memory_order_relaxed) ||
                             GetTickCount64() - endMs >= static_cast<ULONGLONG>(g_fadeOutMs)));   // LOADEND: black for the hand-over frames
        // LOADFADE2: "drawn THIS frame" (the draw counter moved since the last Present), not "within 40 ms" - see above
        static uint32_t feDrawsSeen = 0;
        const uint32_t feDraws = picsplit::fe_draws();
        const bool feThisFrame = feDraws != feDrawsSeen;
        feDrawsSeen = feDraws;
        vr::set_screen_hold(clean && loadSeq && (g_loadFade2.load(std::memory_order_relaxed) ? (!feThisFrame || endMs != 0) : !loadNow));
        // LOADFADE2 (4): DLSS warm-up on the pair while the room screen still shows (results not used)
        if (g_loadFade2.load(std::memory_order_relaxed) && loadSeq && pair && dlss::mode() > 0 && g_dlssReady.load()) {
            ID3D11Device* wd = nullptr; ID3D11DeviceContext* wc = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&wd))) && wd) wd->GetImmediateContext(&wc);
            if (wc) {
                ID3D11Texture2D* l = draw_pic(0); if (!l) l = picsplit::left();
                ID3D11Texture2D* r = draw_pic(1); if (!r) r = picsplit::right();
                if (l && r) { upscaled(wc, 0, l); upscaled(wc, 1, r); }
                wc->Release();
            }
            if (wd) wd->Release();
        }
        {   // LOADREC (V13z17): log every change of what the room screen shows during the sequence, and record the headset
            // images of the first sequence of the session (10 s) - the evidence for the next fix, not JJ's memory.
            static int lastState = -1; static bool recorded = false;
            const int state = !loadSeq ? 0 : endMs ? 3 : loadNow ? 1 : 2;   // 0 off, 1 live loading frame, 2 frozen, 3 black
            if (state != lastState) {
                static const char* kSt[] = {"off", "live loading-screen frame", "frozen last loading frame", "black"};
                if (lastState >= 0 && (state || lastState)) BVR_LOG("loadseq: room screen -> %s (loading screen last drawn %llu ms ago)", kSt[state], feSince);
                lastState = state;
            }
            if (g_autoRec.load(std::memory_order_relaxed) && loadSeq && !recorded && !bvr::vr::eye_burst::active()) { recorded = true; bvr::vr::eye_burst::start(900); BVR_LOG("loadseq: recording the headset for 900 presents"); }
        }
        if (feArmed) g_feWasArmedMs = GetTickCount64();
        // AUTOREC (2026-11-01, JJ: "I don't wanna keep doing individual tests"): the launch records its own evidence.
        // (a) during play, when the headset drops from 3D to the room screen (JJ: Status / System menus "pop into a 2D window
        //     for a couple of seconds"), 360 presents of eye pictures are recorded (first 3 per session) - the core's
        //     "cinematic quad ON (...)" line beside it gives the reason;
        // (b) while the room screen shows with no 3D pair (title, loading), the HUD art drawn is listed (first 3 periods,
        //     1500 frames each) - the loading screen's own art, for a hook-free loading-screen signal.
        // N2 (JJ 11:45: "65 FPS for 5-10 seconds after loading in, then 85"): LOADREC's 900-present recording was exactly that
        // window in two sessions (11:35:27-40, 11:45:43-56; 64 fps until it ended, 86 the next second). Both recorders are
        // evidence tools, now OFF unless asked: `ds autorec 1` (settings auto_rec).
        if (g_autoRec.load(std::memory_order_relaxed)) {
            static bool wasShown = false, everPair = false;
            static int recs = 0, lists = 0;
            const bool shown = vr::screen_shown_now();
            if (pair) everPair = true;
            if (shown && !wasShown) {
                if (everPair && recs < 3 && !bvr::vr::eye_burst::active()) {
                    ++recs;
                    bvr::vr::eye_burst::start(360);
                    BVR_LOG("autorec: the headset dropped to the room screen during play - recording 360 presents (%d of 3)", recs);
                }
                if (!pair && lists < 3) {
                    ++lists;
                    picsplit::hudtex(1500);
                    BVR_LOG("autorec: room screen without 3D - listing the HUD art for 1500 frames (%d of 3)", lists);
                }
            }
            wasShown = shown;
        }
        if (afwPair) { vr::sr_set_present_eye(g_afwPresent.eye ? 1 : -1); vr::sr_set_present_afw(true); }
        else if (pair) { vr::sr_set_present_eye(-1); vr::sr_set_present_afw(true); }
        // PICHUNT once the main view has been mapped (EYEMAP runs 10 s after it is listed), or on request: it finds the
        // SHARED final picture (it matches the window); PICSPLIT then takes each eye's copy of it in GPU order.
        bool have = false;
        { std::lock_guard<std::mutex> lk(g_foundMutex); have = !g_mainFound.empty(); }
        if (have && (g_huntReq.exchange(false) || !g_hunted.exchange(true))) pichunt(sc);
    }
    if (g_ownHuntReq.exchange(false)) ownhunt(sc);   // works with 3D off too (it only reads textures)
    {
        static bool splitInstalled = false;
        // SHADOWRES needs the context hooks whenever a shadow map was made bigger, 3D or not (viewports must follow it).
        if ((g_stereo.load() || shadowres::active()) && !splitInstalled) splitInstalled = picsplit::install(sc) || true;
        picsplit::set_enabled(g_stereo.load());
        picsplit::set_pair(g_stereo.load() && g_ownView.load() != nullptr);
        picsplit::set_hud_on(g_stereo.load() && (g_ownView.load() != nullptr || g_afwPresent.eye >= 0));
        picsplit::set_target(split_target(g_mainView.load()));
        {   // DLSS (M6): the depth both views share, the sub-pixel jitter for the frame about to be drawn, and the
            // picture size that tells a geometry viewport from a shadow cascade's.
            // JITTER14 (SKVR2): the sub-pixel shift only while the pair is up (frames that are really reconstructed);
            // DLSSSTABLE: evaluation only after 3 s of a steady pair, histories reset when it starts.
            const bool pairNow = g_ownView.load() != nullptr;
            static ULONGLONG pairSince = 0;
            if (!pairNow) pairSince = 0; else if (!pairSince) pairSince = GetTickCount64();
            // V13z15: 1.5 s (was 3 s: DLSS then switched on 0.5 s AFTER 3D appeared - a visible change); the loading hold keeps
            // the room screen up for ~2.5 s after the pair, so both eyes now start before 3D is shown.
            const bool ready = pairNow && GetTickCount64() - pairSince >= 1500;
            if (ready && !g_dlssReady.load()) { dlss::reset(); BVR_LOG("DLSS: the pair has been steady for 1.5 s - both eyes start now (histories reset)"); }
            g_dlssReady.store(ready);
            const bool want = dlss::mode() > 0 && g_stereo.load() && pairNow;
            picsplit::dlss_want(want);
            picsplit::want_view_proj(g_stereo.load());   // M8: the world markers need each eye's matrix too
            // DS3 (EYEMAP both views, 2026-10-30): the depth both views share, 2024x2424 fmt 19 bind 0x48.
            static const uint32_t kDepthPath[3] = {0x40, 0x160, 0xC8};
            const bool wantDepth = want || (afw::on() && g_stereo.load());   // AFW warps through it, DLSS or not
            picsplit::set_depth(wantDepth ? resolve_path(g_mainView.load(), kDepthPath, 3) : nullptr);
            picsplit::jitter_size_from_target();
            // The pair we are about to submit was drawn with the offset set at the previous Present.
            g_jitPair[0] = g_jitNext[0]; g_jitPair[1] = g_jitNext[1];
            if (want) {
                dlss::jitter(++g_jitFrame, g_jitNext[0], g_jitNext[1]);
                picsplit::jitter_set(g_jitNext[0], g_jitNext[1]);
            } else if (g_jitNext[0] != 0.0f || g_jitNext[1] != 0.0f) {
                g_jitNext[0] = g_jitNext[1] = 0.0f;
                picsplit::jitter_set(0.0f, 0.0f);
            }
        }
        {   // M7 HUD: the HUD sheet of this frame (shared by both views)
            // Sekiro kept its Scaleform HUD in a sheet of its own at rt+40+40+1F8. Dark Souls III has none - it draws
            // the HUD straight into the final picture - so picsplit re-binds the HUD draws into a sheet of OURS as they
            // are recorded (HUDSHEET), and that sheet is what the room-fixed HUD panel shows.
            static bool provider = false;
            if (!provider) { vr::set_hud_texture_provider(&hud_provider); provider = true; }
            ID3D11Texture2D* hs = nullptr;
            {
                ID3D11Device* hd = nullptr;
                if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&hd))) && hd) {
                    hs = picsplit::hud_redirect_on() ? picsplit::hud_sheet_ensure(hd) : nullptr;
                    hd->Release();
                }
            }
            picsplit::set_hud_sheet(hs);
            if (hs != g_hudSheet.load()) {
                D3D11_TEXTURE2D_DESC hd{};
                if (hs) hs->GetDesc(&hd);
                BVR_LOG("hud: the game's HUD sheet %p %ux%u format %d -> the HUD panel", hs, hd.Width, hd.Height, (int)hd.Format);
                g_hudSheet.store(hs);
            }
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) dev->GetImmediateContext(&ctx);
            picsplit::hud_present(ctx);   // HUDSHEET: once a frame
            if (ctx && hs) hud_measure(ctx);
            // FADESHEET only while the 3D world is what the headset shows: at 11:35:29 (JJ: "fades to black, then the loading
            // screen comes back, then it fades to black again") the pair came up 2 s before the loading screen ended and the
            // loading screen's own dark layer was taken for a travel fade.
            if (ctx) fs::measure(ctx, hs, g_ownView.load() != nullptr && !vr::screen_shown_now());   // FADESHEET
            if (ctx) ctx->Release();
            if (dev) dev->Release();
        }
        exposure_share(sc);
        // VIEWTAG: each view's own textures. DS3 (EYEMAP of BOTH views, 2026-10-30): of the 47 paths that exist in
        // both render-target sets, 22 hold different objects. These six are the full-size ones the final picture
        // pass actually reads (t0 fmt 10, t3 fmt 28, t4 fmt 27, the HDR fmt 26 and the per-view depth), so a command
        // list that writes the shared target while reading one of them belongs to that view.
        static const uint32_t kOwnPaths[6][3] = {{0x318, 0x40, 0x18},  {0x318, 0x40, 0x158}, {0x318, 0x180, 0x158},
                                                 {0x498, 0x40, 0x18},  {0x258, 0x40, 0x18},  {0xE08, 0x180, 0x158}};
        void* views[2] = {g_mainView.load(), g_ownView.load()};
        for (int v = 0; v < 2; ++v) {
            ID3D11Resource* res[6] = {};
            for (int k = 0; k < 6 && views[v]; ++k) res[k] = resolve_path(views[v], kOwnPaths[k], 3);
            picsplit::set_own(v, res, 6);
            // SHADOWLEND was Sekiro's fix for a second view whose own cascade maps came out empty. Dark Souls III
            // puts BOTH views' sun shadows in ONE 8192x8192 atlas (EYEMAP: rt+18+98+B8 is the same object in both
            // render-target sets, 2026-10-30), so there is nothing per-view to lend here. The per-eye shadow
            // behaviour is measured on its own in M4L; until then nothing is swapped.
            picsplit::set_shadow(v, nullptr, 0);
        }
        {   // L7: each view's own textures, to tell which view a watched pass belongs to (shared ones left out)
            // DS3's per-view textures (EYEMAP of both views, 2026-10-30); only paths whose objects really differ
            // are used, which the loop below checks again every frame.
            static const uint32_t kHistPaths[7][3] = {{0x318, 0x40, 0x18},  {0x318, 0x40, 0x158}, {0x318, 0x180, 0x158},
                                                      {0x498, 0x40, 0x18},  {0x258, 0x40, 0x18},  {0x2DC0, 0x40, 0x18},
                                                      {0xFF8, 0x40, 0x18}};
            ID3D11Resource* own[2][7] = {};
            int n = 0;
            for (int k = 0; k < 7 && views[0] && views[1]; ++k) {
                ID3D11Resource* a = resolve_path(views[0], kHistPaths[k], 3);
                ID3D11Resource* b = resolve_path(views[1], kHistPaths[k], 3);
                if (a && b && a != b) { own[0][n] = a; own[1][n] = b; ++n; }
            }
            picsplit::set_hist_own(0, own[0], n);
            picsplit::set_hist_own(1, own[1], n);
        }
        {   // M8: world markers on their objects, in each eye
            static bool wired = false;
            if (!wired && markers::installed()) { picsplit::set_hud_draw(&markers::hud_draw); wired = true; }
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* imm = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) dev->GetImmediateContext(&imm);
            unsigned mw = 0, mh = 0;
            if (!eyesize::full_size(mw, mh)) {
                if (ID3D11Texture2D* L = picsplit::left()) { D3D11_TEXTURE2D_DESC d{}; L->GetDesc(&d); mw = d.Width; mh = d.Height; }
            }
            markers::on_present(dev, imm, mw, mh);
            if (imm) imm->Release();
            if (dev) dev->Release();
        }
        picsplit::on_present();
        envtime::on_present();
        {
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* imm = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) dev->GetImmediateContext(&imm);
            picsplit::measure(imm);   // EYESTAB / HISTAB
            if (imm && g_depthCheckReq.exchange(false)) depth_check(imm, g_depthCheck.load());
            if (imm) imm->Release();
            if (dev) dev->Release();
        }
        // PAIRLEARN runs itself once the pair has been going for a couple of seconds, so no command is needed.
        static int pairFrames = 0, learnedOnce = 0;
        if (g_stereo.load() && g_ownView.load()) {
            // PAIRLEARN (looking for the picture changing) cannot see the boundary at a real eye separation: the two
            // eyes differ by about one part in sixty. DS3 uses the view tags instead (pair mode 4), so this runs only
            // when `ds pairlearn` asks for it.
            (void)pairFrames; (void)learnedOnce;
        } else pairFrames = 0;
        if (const int nd = picsplit::dump_ready(); nd && g_learnReq) {
            // PAIRLEARN: read each write's picture and find where the content CHANGES. The frame holds three stretches:
            // the previous frame's picture, then the game's view, then ours. The last write of the middle stretch is
            // where the left eye must be copied, the last write of all where the right eye must be. Comparing the
            // pictures (not their brightness) catches a pass that only changes the fine detail, which is what made the
            // left eye shimmer. No file is saved: this runs on every launch.
            g_learnReq = false;
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) dev->GetImmediateContext(&ctx);
            std::vector<std::vector<uint8_t>> shot(nd);
            for (int i = 0; ctx && i < nd; ++i) {
                UINT w = 0, h = 0;
                ID3D11Texture2D* t = picsplit::dump_tex(i);
                if (t) read_rgb(ctx, t, 8, shot[i], w, h);
            }
            int change[64] = {}; int nc = 0;
            char line[400]; int k = sprintf_s(line, "pairlearn: picture change per write:");
            for (int i = 1; i < nd && i < 64; ++i) {
                double d = 0;
                const size_t m = (std::min)(shot[i].size(), shot[i - 1].size());
                for (size_t j = 0; j < m; ++j) d += std::abs(int(shot[i][j]) - int(shot[i - 1][j]));
                d = m ? d / m : 0;
                if (k < 360) k += sprintf_s(line + k, sizeof(line) - k, " %d:%.1f", i + 1, d);
                if (d > 2.0 && nc < 8) change[nc++] = i + 1;      // this write replaced the picture
            }
            BVR_LOG("%s", line);
            if (nc >= 2) picsplit::set_pair_indices(change[nc - 1] - 1, nd, nd);
            else BVR_LOG("pairlearn: could not see two picture changes in this frame (%d) - keeping the view labels", nc);
            if (ctx) ctx->Release();
            if (dev) dev->Release();
            picsplit::dump_done();
        } else if (const int nd = picsplit::dump_ready()) {
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t dir[MAX_PATH];
            swprintf_s(dir, L"%s\\captures\\splitdump_%02u%02u%02u", bvr::log::data_dir(), st.wHour, st.wMinute, st.wSecond);
            CreateDirectoryW(dir, nullptr);
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) dev->GetImmediateContext(&ctx);
            for (int i = 0; ctx && i < nd; ++i) {
                ID3D11Texture2D* t = picsplit::dump_tex(i);
                std::vector<uint8_t> s; UINT w = 0, h = 0;
                double mean = 0;
                if (t && read_rgb(ctx, t, 16, s, w, h)) { for (uint8_t v : s) mean += v; mean /= s.empty() ? 1 : s.size(); }
                wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\write%02d.bmp", dir, i + 1);
                const bool ok = t && save_bmp(ctx, t, path);
                BVR_LOG("splitdump: after write %d of %d - brightness %.1f %s", i + 1, nd, mean, ok ? "saved" : "NOT saved");
            }
            if (ctx) ctx->Release();
            if (dev) dev->Release();
            picsplit::dump_done();
        }
        if (const int req = dlss::take_capture_request(); req & 4) {   // DLSSCHECK: both eyes' DLSS colour in / out
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t dir[MAX_PATH];
            swprintf_s(dir, L"%s\\captures\\dlsscheck_%02u%02u%02u", bvr::log::data_dir(), st.wHour, st.wMinute, st.wSecond);
            CreateDirectoryW(dir, nullptr);
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) dev->GetImmediateContext(&ctx);
            for (int eye = 0; ctx && eye < 2; ++eye)
                for (int k : {0, 3}) {
                    ID3D11Texture2D* t = nullptr;
                    if (ID3D11Resource* r = dlss::tex(eye, k)) r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
                    wchar_t path[MAX_PATH];
                    swprintf_s(path, L"%s\\%s_%s.bmp", dir, eye ? L"right" : L"left", k ? L"dlss_out" : L"dlss_in");
                    const bool ok = t && save_bmp(ctx, t, path);
                    if (t) t->Release();
                    BVR_LOG("dlsscheck: %s eye DLSS %s %s", eye ? "right" : "left", k ? "output" : "input", ok ? "saved" : "NOT saved");
                }
            BVR_LOG("dlsscheck: capture in %ls (asked for eye bits %d)", dir, req & 3);
            if (ctx) ctx->Release();
            if (dev) dev->Release();
        }
        static ULONGLONG lastCheck = 0;
        const ULONGLONG now = GetTickCount64();
        // N2: the 10-s stereo self-check copies both eye pictures back and WAITS for the GPU (a hitch every 10 s) - a proof
        // tool for the eye split, now only every 10 s when asked: `ds selfcheck 1` (settings self_check, default 0).
        if (g_selfCheck.load(std::memory_order_relaxed) < 0) g_selfCheck.store(settings::i("self_check", 0));
        if (g_stereo.load() && g_ownView.load() && ((g_selfCheck.load(std::memory_order_relaxed) && now - lastCheck >= 10000) ||
                                                    g_checkReq.exchange(false))) {
            lastCheck = now;
            eyes_check(sc, "10 s");
        }
    }
    // EYEMAP: on request, and once by itself for each view (main 10 s after it is listed, ours 5 s after it is built).
    {
        static void* mappedMain = nullptr; static void* mappedOwn = nullptr;
        static ULONGLONG mainSeen = 0, ownSeen = 0;
        const ULONGLONG now = GetTickCount64();
        void* m = g_mainView.load(); void* o = g_ownView.load();
        if (m != mappedMain) { if (!mainSeen) mainSeen = now; if (now - mainSeen >= 10000) { eyemap_async(m, nullptr); mappedMain = m; mainSeen = 0; } }
        if (o && o != mappedOwn) { if (!ownSeen) ownSeen = now; if (now - ownSeen >= 5000) { eyemap_async(nullptr, o); mappedOwn = o; ownSeen = 0; } }
        if (g_eyemapReq.exchange(false)) eyemap_async(m, o);
        static ULONGLONG lastDump = 0;
        if (o && now - lastDump >= 5000) { lastDump = now; camdump("5 s"); }
    }
    const uint32_t n = g_rallThisPresent.exchange(0, std::memory_order_relaxed);
    ++g_presents;
    if (n > g_maxPerPresent) g_maxPerPresent = n;
    if (g_lastCount.load(std::memory_order_relaxed) > 1) ++g_multiFrames;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (g_on && now - last >= 5000) {
        last = now;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        const double ms = static_cast<double>(g_rallTicks.exchange(0)) * 1000.0 / static_cast<double>(f.QuadPart);
        void* rm = rendman();
        BVR_LOG("stereo: 5 s - presents %u, render-all calls %u (max %u per present, thread %u), views now %d, frames with "
                ">1 view %u, render-all CPU %.2f ms total; RendMan %p list(+8) %p; adds %u removes %u ctors %u; main view "
                "desc %s",
                g_presents, g_rallCalls.exchange(0), g_maxPerPresent, g_rallTid.load(), g_lastCount.load(), g_multiFrames, ms,
                rm, rm ? *reinterpret_cast<void**>(static_cast<uint8_t*>(rm) + 8) : nullptr, g_adds.load(), g_removes.load(),
                g_ctors.load(), g_haveMainCtor ? "captured" : "not seen (built before our hooks?)");
        if (g_wantView.load() || g_ownBuilds.load())
            BVR_LOG("drawtwice %d: complete double draws %u | hdrswap %d: %u copies", g_drawTwice.load(), g_drawTwiceFrames.load(),
            g_hdrSwap.load(), g_hdrSwaps.load());
    {
        uint32_t hd = 0, hh = 0, hs = 0, hl = 0, hw = 0, hi = 0, hn = 0;
        picsplit::hud_sheet_stats(hd, hh, hs, hl);
        picsplit::hud_sheet_stats2(hw, hi, hn);
        BVR_LOG("hudsheet %d: %u HUD draws moved to the panel's sheet (%u by the geo-11 fix's four shaders, %u by the "
                "Scaleform signature; %u on the immediate context, %u instanced), %u sheet clears; %u on-object pieces "
                "(lock-on, enemy bars, crosshair) left in the picture", picsplit::hud_redirect_on(), hd, hh, hs, hi, hn, hl, hw);
    }
    BVR_LOG("ownview: wanted %d, listed %s, frames with two views %u, built %u, removed %u%s | 3D %d: eye cameras set %u, "
                    "eyes fed L %u R %u, misses %u, copied setting ranges %d",
                    g_wantView.load() ? 1 : 0, g_ownView.load() ? "yes" : "no", g_framesTwo.exchange(0), g_ownBuilds.load(),
                    g_ownRemovals.load(), g_poisoned.load() ? ", SWITCHED OFF after a fault" : "", g_stereo.load() ? 1 : 0,
                    g_stereoFrames.exchange(0), g_fedL.exchange(0), g_fedR.exchange(0), g_feedMiss.exchange(0), g_copyN.load());
        hwwatch::rearm();            // threads the game started since the watch was armed
        g_presents = 0; g_maxPerPresent = 0; g_multiFrames = 0;
    }
}

// FINDF / POKEF (2026-10-30, shadows beyond MAX): find the engine's cascade split distances (10 / 50 / 200 m in
// FC_CascadeSelectDist) in writable memory - on a background thread (a whole-process scan must not stall a Present).
bool near_eq(float x, float y) { const float d = x - y; return (d < 0 ? -d : d) <= 1e-4f * (fabsf(y) > 1.0f ? fabsf(y) : 1.0f); }
void findf_thread(float a, float b, float c) {
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uint8_t* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    const HMODULE exe = GetModuleHandleW(nullptr);
    int hits = 0; uint64_t scanned = 0;
    while (p < si.lpMaximumApplicationAddress && hits < 40) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(p, &mi, sizeof(mi))) break;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        if (mi.State == MEM_COMMIT && (mi.Protect & rw) && !(mi.Protect & PAGE_GUARD) && mi.RegionSize < (512ull << 20)) {
            const float* f = static_cast<const float*>(mi.BaseAddress);
            const size_t n = mi.RegionSize / 4;
            __try {
                for (size_t i = 0; i + 2 < n && hits < 40; ++i)
                    if (near_eq(f[i], a) && near_eq(f[i + 1], b) && near_eq(f[i + 2], c)) {   // within 1e-4 (dumps print rounded)
                        const float* q = f + i;
                        const uintptr_t at = reinterpret_cast<uintptr_t>(q);
                        const uintptr_t rva = at - reinterpret_cast<uintptr_t>(exe);
                        BVR_LOG("findf: %p%s | -4..+7: %g %g %g %g [%g %g %g] %g %g %g %g %g", q,
                                rva < 0x8000000 ? " (in DarkSoulsIII.exe)" : "", q[-4], q[-3], q[-2], q[-1], q[0], q[1], q[2], q[3], q[4],
                                q[5], q[6], q[7]);
                        ++hits;
                    }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            scanned += mi.RegionSize;
        }
        p = static_cast<uint8_t*>(mi.BaseAddress) + mi.RegionSize;
    }
    BVR_LOG("findf: %g %g %g - %d hits in %.0f MB", a, b, c, hits, scanned / 1048576.0);
}

void draw_ui() {
    // Status only (N2 tidy, 2026-11-01): the second-view test switches left the panel - unticking them broke 3D.
    ImGui::TextDisabled("Engine views: %d | second view %s, listed %u times%s", g_lastCount.load(), g_ownView.load() ? "drawn" : "not drawn",
                        g_ownListings.load(), g_poisoned.load() ? " - switched off after an error" : "");
}

void draw_stereo_ui() {
    // Status only (the "True 3D" switch left the panel, JJ 2026-10-30: 3D is simply on).
    ImGui::TextDisabled("   3D: %s", !g_stereo.load() ? "off (stereo3d = 0)"
                                     : g_ownView.load() ? "both eyes drawn by the game"
                                                        : "waiting for gameplay (the game is on a screen meanwhile)");
    ImGui::TextDisabled("   right eye lighting: %s", g_copyN.load() ? "matched to the left eye" : "NOT matched (no rtclass.txt)");
    if (g_poisoned.load()) ImGui::TextDisabled("   the second view switched itself off after an error - restart the game");
}

void save_pictures(IDXGISwapChain* sc, const wchar_t* dir) {
    ID3D11Device* dev = nullptr;
    if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    const struct { const char* name; void* view; } views[2] = {{"main", g_mainView.load()}, {"second", g_ownView.load()}};
    for (const auto& v : views) {
        ID3D11Texture2D* t = view_picture(v.view);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\view_%hs.bmp", dir, v.name);
        const bool ok = t && save_bmp(ctx, t, path);
        BVR_LOG("capture: %s view picture %s", v.name, ok ? "saved" : (v.view ? "NOT found at rt+10B08+40+18" : "(view not listed)"));
    }
    const struct { const char* name; ID3D11Texture2D* t; } split[2] = {{"left", picsplit::left()}, {"right", picsplit::right()}};
    for (const auto& s : split) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\split_%hs.bmp", dir, s.name);
        BVR_LOG("capture: split %s eye %s", s.name, s.t && save_bmp(ctx, s.t, path) ? "saved" : "not available");
    }
    for (int e = 0; e < 2; ++e) {   // M8: the cards this frame, alpha kept, so a marker can be seen on its own
        if (ID3D11Texture2D* mt = markers::layer_tex(e)) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\markers_%hs.bmp", dir, e ? "right" : "left");
            gfx_hud::save_texture_bmp(ctx, mt, path);
        }
    }
    if (ID3D11Texture2D* hs = g_hudSheet.load()) { // HUDALPHA: 32-bit, alpha kept
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\hud_sheet.bmp", dir);
        gfx_hud::save_texture_bmp(ctx, hs, path);
        if (g_hudOut) { swprintf_s(path, L"%s\\hud_panel.bmp", dir);gfx_hud::save_texture_bmp(ctx, g_hudOut, path); }
    }
    eyes_check(sc, "capture");
    camdump("capture");
    ctx->Release();
    dev->Release();
}

const char* dlss_status() {
    static char s[420];
    unsigned iw = 0, ih = 0, fw = 0, fh = 0;
    eyesize::internal(iw, ih);
    eyesize::full_size(fw, fh);
    uint32_t sh = 0, kp = 0;
    picsplit::jitter_stats(sh, kp);
    snprintf(s, sizeof(s), "DLSS: game renders %ux%u -> eye %ux%u | jitter %.3f %.3f px (this frame), %.3f %.3f (the pair) "
                           "| viewports jittered %u, kept %u | upscaled L %u R %u, plain L %u R %u | %s",
             iw, ih, fw, fh, g_jitNext[0], g_jitNext[1], g_jitPair[0], g_jitPair[1], sh, kp, g_dlssUp[0].load(),
             g_dlssUp[1].load(), g_dlssPlain[0].load(), g_dlssPlain[1].load(), picsplit::dlss_status());
    return s;
}

// OFFAXIS: the two views' camera links (main = the game view = LEFT eye, own = our view = RIGHT eye), any thread.
void links(void*& mainLink, void*& ownLink) {
    void* mv = g_mainView.load();
    mainLink = mv ? field(mv, 0x48) : nullptr;
    ownLink = g_ownLink;
}

bool handle_command(const char* cmd, const char* args) {
    (void)args;
    if (_stricmp(cmd, "views") == 0) {
        void* l = g_lastList.load();
        if (!l) if (void* rm = rendman()) l = *reinterpret_cast<void**>(static_cast<uint8_t*>(rm) + 8);
        if (l) log_list("command", l); else BVR_LOG("views: no list seen yet");
        return true;
    }
    if (_stricmp(cmd, "probe") == 0) { g_on = atoi(args) != 0; settings::set_i("stereo_probe", g_on ? 1 : 0); return true; }
    if (_stricmp(cmd, "view") == 0) { g_wantView.store(atoi(args) != 0); BVR_LOG("ownview: %s by command", g_wantView.load() ? "ON" : "off"); return true; }
    if (_stricmp(cmd, "eyemap") == 0) { g_eyemapReq.store(true); return true; }
    if (_stricmp(cmd, "viewcam") == 0) { g_ownCam.store(atoi(args) != 0); BVR_LOG("ownview: own camera %d", g_ownCam.load() ? 1 : 0); return true; }
    if (_stricmp(cmd, "viewshift") == 0) { g_shiftM.store(static_cast<float>(atof(args))); BVR_LOG("ownview: shift %.3f m", g_shiftM.load()); return true; }
    if (_stricmp(cmd, "linkpos") == 0) { g_linkPosOff.store(static_cast<int>(strtol(args, nullptr, 0))); BVR_LOG("ownview: link position at +0x%X", g_linkPosOff.load()); return true; }
    if (_stricmp(cmd, "camdump") == 0) { camdump("command"); return true; }
    if (_stricmp(cmd, "3d") == 0) { set_3d(atoi(args) != 0); return true; }
    if (_stricmp(cmd, "eyesep") == 0) { g_eyeSep.store(atoi(args)); BVR_LOG("stereo3d: eye separation %d", g_eyeSep.load()); return true; }
    if (_stricmp(cmd, "check") == 0) { g_checkReq.store(true); return true; }
    if (_stricmp(cmd, "hud") == 0) { // M7: the game's HUD on the HUD panel in 3D (1) or not (0)
        g_hudOn.store(atoi(args) != 0);
        BVR_LOG("hud: %s (given %u times so far)", g_hudOn.load() ? "ON" : "off", g_hudGiven.load());
        return true;
    }
    if (_stricmp(cmd, "hudalpha") == 0) { // HUDALPHA: -1 measured (default), 0 alpha as stored, 1 inverted
        g_hudAlpha.store((std::max)(-1, (std::min)(1, atoi(args))));
        BVR_LOG("hud: alpha mode %d (measured verdict: %s)", g_hudAlpha.load(), g_hudAlphaSeen.load() ? "inverted" : "as stored");
        return true;
    }
    if (_stricmp(cmd, "lensshift") == 0) { // M4 test: "lensshift <x> <y>" into our view's link +0x6C/+0x70; "lensshift off"
        float x = 0, y = 0;
        const bool on = sscanf_s(args, "%f %f", &x, &y) >= 1;
        g_lensShift[1][0].store(x); g_lensShift[1][1].store(y);
        g_lensShiftOn.store(on);
        BVR_LOG("lensshift: %s x %.3f y %.3f (our view's link +0x6C/+0x70; watch the selfcheck's eye shift)", on ? "ON" : "off", x, y);
        g_checkReq.store(true);
        return true;
    }
    if (_stricmp(cmd, "rtpeek") == 0) {
        // "rtpeek <hex offset> <hex length>": both views' render-target sets side by side, 8 bytes per line
        char* e = nullptr;
        const unsigned long off = strtoul(args, &e, 16);
        const unsigned long len = e ? strtoul(e, nullptr, 16) : 0x40;
        void* m = g_mainView.load(); void* o = g_ownObj;
        const uint8_t* a = m ? static_cast<const uint8_t*>(reinterpret_cast<VGet>(vt(m)[3])(m)) : nullptr;
        const uint8_t* b = o ? static_cast<const uint8_t*>(reinterpret_cast<VGet>(vt(o)[3])(o)) : nullptr;
        for (unsigned long k = 0; a && k < len && k < 0x400; k += 8) {
            const uint64_t qa = *reinterpret_cast<const uint64_t*>(a + off + k);
            const uint64_t qb = b ? *reinterpret_cast<const uint64_t*>(b + off + k) : 0;
            BVR_LOG("rtpeek +0x%05lX main %016llX ours %016llX%s", off + k, static_cast<unsigned long long>(qa),
                    static_cast<unsigned long long>(qb), qa != qb ? "  <-- differs" : "");
        }
        return true;
    }
    if (_stricmp(cmd, "peek") == 0) {
        // "peek rt+67B0+110 0x100": follow the same pointer path in BOTH views' settings objects and print the bytes
        // side by side as hex and floats (L3: the sun cascade objects hang off the settings object by pointer).
        uint32_t off[4] = {}; int depth = 0;
        char* e = const_cast<char*>(strchr(args, ' '));
        if (e) *e = 0;
        if (!parse_path(args, off, depth)) { BVR_LOG("peek: use `ds peek rt+67B0+110 0x100`"); return true; }
        const unsigned long len = e ? strtoul(e + 1, nullptr, 16) : 0x80;
        const uint8_t* base[2] = {rt_of(g_mainView.load()), rt_of(g_ownView.load())};
        const char* who[2] = {"main", "ours"};
        const uint8_t* at[2] = {nullptr, nullptr};
        for (int v = 0; v < 2; ++v) {
            const uint8_t* q = base[v];
            for (int d = 0; q && d < depth; ++d) {
                if (!readable_ptr(reinterpret_cast<uintptr_t>(q) + off[d], 8)) { q = nullptr; break; }
                if (d == depth - 1) { q += off[d]; break; }          // the last step is an offset, not a hop
                q = *reinterpret_cast<const uint8_t* const*>(q + off[d]);
                if (!readable_ptr(reinterpret_cast<uintptr_t>(q), 8)) { q = nullptr; break; }
            }
            at[v] = q;
            BVR_LOG("peek [%s] %s -> %p", who[v], args, q);
        }
        for (unsigned long k = 0; k < len && k < 0x400; k += 16) {
            char line[220]; int n = sprintf_s(line, "peek +0x%03lX", k);
            for (int v = 0; v < 2; ++v) {
                if (!at[v] || !readable_ptr(reinterpret_cast<uintptr_t>(at[v]) + k, 16)) { n += sprintf_s(line + n, sizeof(line) - n, " | %s -", who[v]); continue; }
                const float* f = reinterpret_cast<const float*>(at[v] + k);
                n += sprintf_s(line + n, sizeof(line) - n, " | %s %11.4g %11.4g %11.4g %11.4g", who[v], f[0], f[1], f[2], f[3]);
            }
            BVR_LOG("%s", line);
        }
        return true;
    }
    // L7: the engine's light-environment tables (G = [0x143F3B490], tables at G+0x11370 + i*8, records of 0x38 bytes whose +4
    // is the light-set slot written to cbInstanceData.envData) and a hardware watch on any absolute address.
    if (_stricmp(cmd, "envonce") == 0) { const int m = atoi(args); envtime::once(m); settings::set_i("env_once", m); return true; }
    if (_stricmp(cmd, "envshare") == 0) { const int m = atoi(args); envtime::share(m); settings::set_i("env_share", m); return true; }
    if (_stricmp(cmd, "envtime") == 0) { envtime::arm(atoi(args) > 0 ? atoi(args) : 2); return true; }
    if (_stricmp(cmd, "envdump") == 0) {
        const int n = atoi(args) > 0 ? atoi(args) : 12;
        // Sekiro's global; Dark Souls III's light-environment manager is still to be found (M4L), so this test
        // command only reports that, rather than dereferencing an address from another game.
        BVR_LOG("envdump: the light-environment manager has not been found for Dark Souls III yet (M4L)");
        return true;
        const uint8_t* G = nullptr;
        if (!G) { BVR_LOG("envdump: G not set"); return true; }
        const uint64_t* q = reinterpret_cast<const uint64_t*>(G + 0x11358);
        BVR_LOG("envdump: G %p | +11358 %llx +11360 %llx +11368 %llx +11370 %llx +11378 %llx +11380 %llx +11388 %llx +11390 %llx", G,
                q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        for (int t = 0; t < 4; ++t) {
            const uint8_t* tab = reinterpret_cast<const uint8_t*>(q[3 + t]);
            if (!tab || reinterpret_cast<uintptr_t>(tab) < 0x10000) continue;
            for (int i = 0; i < n; ++i) {
                const uint32_t* r = reinterpret_cast<const uint32_t*>(tab + i * 0x38);
                BVR_LOG("envdump   table %d record %2d @%p: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", t, i, r,
                        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13]);
            }
        }
        return true;
    }
    if (_stricmp(cmd, "hwabs") == 0) {   // "hwabs <slot 0-3> <hex address>" (0 = clear)
        int slot = 0; unsigned long long a = 0;
        sscanf_s(args, "%d %llx", &slot, &a);
        static char what[4][32];
        sprintf_s(what[slot & 3], "abs %llx", a);
        hwwatch::arm_slot(slot & 3, static_cast<uintptr_t>(a), what[slot & 3]);
        return true;
    }
    if (_stricmp(cmd, "cullhunt") == 0) {
        // V12z CULLHUNT (JJ: gravestones / rocks pop at the outer edges of BOTH eyes; cull_margin 2.0 and both hooked cull
        // builders change nothing): hardware READ watches on each view's camera link - fov (+0x50) and position x (+0x30, camdump: rows at +0, position at +0x30) -
        // list every game function that reads them (first 48 sites, with callers). `ds cullhunt off` ends it.
        if (!_strnicmp(args, "off", 3)) { hwwatch::off(); hwwatch::set_read(false); BVR_LOG("cullhunt: off"); return true; }
        void* m = g_mainView.load(); void* o = g_ownView.load();
        const uint8_t* lm = m ? static_cast<const uint8_t*>(field(m, 0x48)) : nullptr;
        const uint8_t* lo = o ? static_cast<const uint8_t*>(field(o, 0x48)) : nullptr;
        hwwatch::set_read(true);
        hwwatch::arm_slot(0, lm ? reinterpret_cast<uintptr_t>(lm + 0x50) : 0, "main link fov");
        hwwatch::arm_slot(1, lo ? reinterpret_cast<uintptr_t>(lo + 0x50) : 0, "our link fov");
        hwwatch::arm_slot(2, lm ? reinterpret_cast<uintptr_t>(lm + 0x30) : 0, "main link pos x");
        hwwatch::arm_slot(3, lo ? reinterpret_cast<uintptr_t>(lo + 0x30) : 0, "our link pos x");
        BVR_LOG("cullhunt: read watches on main link %p and our link %p (+0x50 fov, +0x30 pos x)", lm, lo);
        return true;
    }
    if (_stricmp(cmd, "hwwatch") == 0) {
        // "off" | up to four watches: "main+0x1FF0 ours+0x1FF0 main+0x2950" (offsets into each view's settings object)
        if (!_strnicmp(args, "off", 3)) { hwwatch::off(); return true; }
        const uint8_t* rtm = rt_of(g_mainView.load());
        const uint8_t* rto = rt_of(g_ownView.load());
        int slot = 0;
        const char* p = args;
        while (*p && slot < 4) {
            while (*p == ' ' || *p == ',') ++p;
            if (!*p) break;
            const bool ours = !_strnicmp(p, "ours", 4) || !_strnicmp(p, "own", 3) || !_strnicmp(p, "right", 5);
            const char* plus = strchr(p, '+');
            const unsigned long off = strtoul(plus ? plus + 1 : p, nullptr, 16);
            const uint8_t* base = ours ? rto : rtm;
            if (base) {
                char what[64]; sprintf_s(what, "%s rt +0x%lX", ours ? "ours" : "main", off);
                hwwatch::arm_slot(slot++, reinterpret_cast<uintptr_t>(base + off), what);
            } else {
                BVR_LOG("hwwatch: the %s view is not listed", ours ? "second" : "main");
            }
            while (*p && *p != ' ' && *p != ',') ++p;
        }
        for (int i = slot; i < 4; ++i) hwwatch::arm_slot(i, 0, "-");
        return true;
    }
    if (_stricmp(cmd, "splitdump") == 0) { picsplit::dump_arm(); BVR_LOG("splitdump: armed (next whole frame)"); return true; }
    if (_stricmp(cmd, "depthcheck") == 0) { g_depthCheck.store(atoi(args) == 1 ? 1 : 0); g_depthCheckReq.store(true); return true; }
    if (_stricmp(cmd, "hudprobe") == 0) { picsplit::hud_probe(atoi(args)); return true; }
    if (markers::handle_command(cmd, args)) return true;
    if (_stricmp(cmd, "vppick") == 0) { float t = 0; int on = 1; sscanf_s(args, "%d %f", &on, &t); picsplit::vp_pick(on, t); return true; }
    if (_stricmp(cmd, "vpcheck") == 0) { picsplit::vp_check(atoi(args) > 0 ? atoi(args) : 2); return true; }
    if (_stricmp(cmd, "dlssinfo") == 0) { BVR_LOG("%s", dlss_status()); BVR_LOG("%s", dlss::diag()); return true; }
    if (_stricmp(cmd, "picpath") == 0) { set_pic_path(args); return true; }
    if (_stricmp(cmd, "splitpath") == 0) {
        uint32_t off[4] = {}; int depth = 0;
        if (parse_path(args, off, depth)) {
            std::lock_guard<std::mutex> lk(g_picMutex);
            memcpy(g_splitOff, off, sizeof(off)); g_splitDepth = depth;
            BVR_LOG("picsplit: split target now %s", args);
        }
        return true;
    }
    if (_stricmp(cmd, "pichunt") == 0) { g_huntReq.store(true); return true; }
    if (_stricmp(cmd, "ownhunt") == 0) { g_ownHuntReq.store(true); return true; }
    if (_stricmp(cmd, "hdrswap") == 0) {
        const int m = atoi(args);
        g_hdrSwap.store(m);
        BVR_LOG("hdrswap %d: %s", m, m == 1 ? "the game view's HDR is copied into ours after the view loop"
                                    : m == 2 ? "our HDR is copied into the game view's after the view loop"
                                             : "off");
        return true;
    }
    if (_stricmp(cmd, "camprobe") == 0) { offaxis::cam_probe(); return true; }
    if (_stricmp(cmd, "projprobe") == 0) { offaxis::proj_probe(); return true; }
    if (_stricmp(cmd, "offaxis") == 0) { BVR_LOG("%s", offaxis::status()); return true; }
    if (_stricmp(cmd, "cullcamscan") == 0) { cullcam_scan(); return true; }
    if (_stricmp(cmd, "cullcamfix") == 0) {
        g_cullCamFix.store(atoi(args));
        BVR_LOG("cullcamfix %d (moved %u times, put back %u times so far)", g_cullCamFix.load(), g_cullCamFixed.load(), g_cullCamRestored.load());
        return true;
    }
    if (_stricmp(cmd, "offunion") == 0) { offaxis::union_cmd(args); return true; }
    if (_stricmp(cmd, "offcallers") == 0) { offaxis::callers_log(); return true; }
    if (_stricmp(cmd, "cullplanes") == 0) { offaxis::planes_cmd(args); return true; }
    if (_stricmp(cmd, "mirrorflip") == 0) { offaxis::set_mirror_flip(atoi(args)); BVR_LOG("%s", offaxis::status()); return true; }   // MIRRORFLIP (V6)
    if (_stricmp(cmd, "enginescene") == 0) {   // ENGINESCENE (V5): `ds enginescene 0|1` - the world view vetoes the loading-screen detector
        if (*args) { bvr::hud::set_engine_scene_veto(atoi(args) != 0); settings::set_i("enginescene", atoi(args) != 0 ? 1 : 0); }
        BVR_LOG("enginescene: veto %d, world pass %d, screen-only %d", bvr::hud::engine_scene_veto() ? 1 : 0,
                bvr::hud::world_pass() ? 1 : 0, bvr::hud::screen_only() ? 1 : 0);
        return true;
    }
    if (_stricmp(cmd, "focus") == 0) { BVR_LOG("%s", focus::status()); return true; }   // KEEPFOCUS (V9)
    if (_stricmp(cmd, "afw") == 0) return afw::command(args);   // AFW (V7): on|off|mode N|win N|dbg N|feather N|log N|status
    if (_stricmp(cmd, "tilefix") == 0) { if (*args) tilefix::set_mode(atoi(args)); BVR_LOG("%s", tilefix::status()); return true; }   // TILEFIX (V6)
    if (_stricmp(cmd, "lightgrid") == 0) {   // LIGHTGRID (V4): `ds lightgrid` = status, `ds lightgrid 0/1/2` = off / eye window / mirrored
        if (*args) lightgrid::set_mode(atoi(args));
        BVR_LOG("%s", lightgrid::status());
        return true;
    }
    if (_stricmp(cmd, "sunfocal") == 0) { sunrays::set_scale(static_cast<float>(atof(args))); return true; }   // SUNRAYS
    if (_stricmp(cmd, "bbfix") == 0) { picsplit::bbfix(*args ? atoi(args) : -1); return true; }   // BBFIX (V6)
    if (_stricmp(cmd, "rollscan") == 0) { picsplit::rollscan(atoi(args)); return true; }   // ROLLSCAN (V5)
    if (_stricmp(cmd, "occprobe") == 0) { picsplit::occprobe(atoi(args)); return true; }
    if (_stricmp(cmd, "screenband") == 0) { const float a = static_cast<float>(atof(args)); vr::set_screen_band(a); settings::set_f("screen_band", a); return true; }
    if (_stricmp(cmd, "menuview") == 0) { g_menuViewOk.store(atoi(args) ? 1 : 0); settings::set_i("menu_view", g_menuViewOk.load()); BVR_LOG("menuview %d: menu-preview views %s", g_menuViewOk.load(), g_menuViewOk.load() ? "do not take the second eye out" : "take the second eye out (old)"); return true; }
    if (_stricmp(cmd, "smokescale") == 0) { const float k = static_cast<float>(atof(args)); picsplit::smokescale(k); settings::set_f("smoke_scale", k); return true; }
    if (_stricmp(cmd, "smokeface") == 0) { picsplit::smokeface(atoi(args)); settings::set_i("smoke_face", atoi(args)); return true; }
    if (_stricmp(cmd, "smokelock") == 0) { picsplit::smokelock(atoi(args)); settings::set_i("smoke_lock", atoi(args)); return true; }
    if (_stricmp(cmd, "smokefill") == 0) { const float k = static_cast<float>(atof(args)); picsplit::smokefill(k); settings::set_f("smoke_fill", k); return true; }
    if (_stricmp(cmd, "hazefill") == 0) { const float f = static_cast<float>(atof(args)); picsplit::hazefill(f); settings::set_f("haze_fill", f); return true; }
    if (_stricmp(cmd, "fadesheet") == 0) { fs::g_on.store(atoi(args) ? 1 : 0); settings::set_i("fade_sheet", fs::g_on.load()); BVR_LOG("fadesheet %d", fs::g_on.load()); return true; }
    if (_stricmp(cmd, "hudhide") == 0) { picsplit::hudhide(args); return true; }
    if (_stricmp(cmd, "hudfade") == 0) { picsplit::hudfade(args); return true; }
    if (_stricmp(cmd, "texdump") == 0) { picsplit::texdump(args); return true; }
    if (_stricmp(cmd, "selfcheck") == 0) { g_selfCheck.store(atoi(args) ? 1 : 0); settings::set_i("self_check", g_selfCheck.load()); g_checkReq.store(true); BVR_LOG("selfcheck %d (every 10 s; one check now)", g_selfCheck.load()); return true; }
    if (_stricmp(cmd, "autorec") == 0) { g_autoRec.store(atoi(args) ? 1 : 0); settings::set_i("auto_rec", g_autoRec.load()); BVR_LOG("autorec %d (automatic headset recordings)", g_autoRec.load()); return true; }
    if (_stricmp(cmd, "loadfade2") == 0) { g_loadFade2.store(atoi(args) ? 1 : 0); settings::set_i("load_fade2", g_loadFade2.load()); BVR_LOG("loadfade2 %d", g_loadFade2.load()); return true; }
    if (_stricmp(cmd, "loadclean") == 0) { g_loadClean.store(atoi(args) ? 1 : 0); settings::set_i("load_clean", g_loadClean.load()); BVR_LOG("loadclean %d (loading screen signal %s)", g_loadClean.load(), loadscreen::ok() ? "hooked" : "NOT hooked"); return true; }
    if (_stricmp(cmd, "boxprobe") == 0) { picsplit::boxprobe(atoi(args)); return true; }
    if (_stricmp(cmd, "boxvis") == 0) { picsplit::boxvis(args && *args ? atoi(args) : -1); return true; }
    if (_stricmp(cmd, "occscale") == 0) { picsplit::occscale(static_cast<float>(atof(args))); return true; }
    if (_stricmp(cmd, "rayscale") == 0) { picsplit::ray_scale_set(static_cast<float>(atof(args))); return true; }
    if (_stricmp(cmd, "shaftprobe") == 0) { picsplit::shaftprobe(atoi(args)); return true; }
    if (_stricmp(cmd, "sunprobe") == 0) { picsplit::sunprobe(atoi(args)); return true; }
    if (_stricmp(cmd, "skipps") == 0) { picsplit::skip_ps(strtoull(args, nullptr, 16)); return true; }
    if (_stricmp(cmd, "postlog") == 0) { picsplit::postlog(atoi(args)); return true; }   // POSTLOG: the post chain
    if (_stricmp(cmd, "hudtex") == 0) { picsplit::hudtex(atoi(args)); return true; }   // HUDTEX: name the HUD's pieces
    if (_stricmp(cmd, "hudworld") == 0) { picsplit::hud_world_keep(atoi(args)); return true; }
    if (_stricmp(cmd, "hudsheet") == 0) {   // HUDSHEET: the HUD on the floating panel (1) or left in the picture (0)
        const int m = atoi(args);
        picsplit::hud_redirect_set(m);
        settings::set_i("hud_sheet", m);
        BVR_LOG("hudsheet %d: %s", m, m ? "the HUD is drawn into our sheet and shown on the floating panel"
                                        : "the HUD stays in the picture (it rides your head)");
        return true;
    }
    if (_stricmp(cmd, "drawtwice") == 0) {
        const int m = atoi(args);
        g_drawTwice.store(m);
        BVR_LOG("drawtwice %d: %s", m, m ? "the engine draws the whole frame twice a frame, once with each view last, "
                                           "so each eye gets a finished picture"
                                         : "off (one draw; only the view that renders last is finished)");
        return true;
    }
    if (_stricmp(cmd, "viewprio") == 0) {   // the playbook's eye-order discriminator (14 #two-pass-ownership)
        void* v = g_ownObj;
        if (!v) { BVR_LOG("viewprio: our view does not exist yet"); return true; }
        uint32_t* p = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(v) + addr::kViewPriority);
        const uint32_t was = *p, now = static_cast<uint32_t>(strtoul(args, nullptr, 0));
        *p = now;
        BVR_LOG("viewprio: our view's priority %u -> %u (render-all sorts by ~priority, so a HIGHER number renders FIRST)",
                was, now);
        return true;
    }
    if (_stricmp(cmd, "exposhare") == 0) {   // EXPOSHARE: one adapted brightness for both eyes
        const int m = atoi(args);
        g_expoShare.store(m);
        settings::set_i("exposure_share", m);
        BVR_LOG("exposhare %d: the second eye %s", m, m ? "tone-maps with the game view's adapted brightness"
                                                       : "adapts on its own (stock - its picture comes out black)");
        return true;
    }
    if (_stricmp(cmd, "rtdiff") == 0) { rtdiff(); return true; }
    if (_stricmp(cmd, "rtclass") == 0) {
        // "<frames>" measure (walk around while it runs) | "apply" (A+B) | "applya" (A only) | "off"
        if (!_strnicmp(args, "save", 4)) { rtclass_save(); return true; }
        if (!_strnicmp(args, "load", 4)) { rtclass_load(false); return true; }
        if (!_strnicmp(args, "applya", 6)) { rtclass_apply(true); return true; }
        if (!_strnicmp(args, "apply", 5)) { rtclass_apply(false); return true; }
        if (!_strnicmp(args, "off", 3)) { g_classState.store(0); g_copyN.store(0); BVR_LOG("rtclass: off, copy cleared"); return true; }
        const bool add = !_strnicmp(args, "add", 3);   // "add <frames>": keep what the loaded table already knows
        if (add) args += 3;
        g_classPrev = add ? g_classOf : std::vector<uint8_t>();
        const int frames = atoi(args) > 0 ? atoi(args) : 240;
        g_copyN.store(0);                         // measure the engine's own values, not our copy
        g_classWant.store(static_cast<uint32_t>(frames));
        g_classGot.store(0);
        g_classState.store(1);
        BVR_LOG("rtclass: measuring both views' settings for %d frames - MOVE THE CAMERA while it runs", frames);
        return true;
    }
    if (_stricmp(cmd, "rtprime") == 0) {   // copy the measured ranges for N frames, then let the engine maintain them
        const int n = atoi(args) > 0 ? atoi(args) : 30;
        if (!g_copyN.load()) rtclass_apply(false);   // build the ranges from the classification first
        g_copyAlways.store(false);
        g_primeFrames.store(n);
        BVR_LOG("rtprime: priming our view's settings for %d frames (%d ranges)", n, g_copyN.load());
        return true;
    }
    if (_stricmp(cmd, "shadowlend") == 0) { const int m = atoi(args); picsplit::shadow_lend(m); settings::set_i("shadow_share", m); return true; }
    // L7: the frame-to-frame buffers the two views share (TEST PLAN L7)
    if (_stricmp(cmd, "histspy") == 0) { picsplit::hist_spy(atoi(args) > 0 ? atoi(args) : 2); return true; }
    if (_stricmp(cmd, "histswap") == 0) { const int m = atoi(args); picsplit::hist_swap(m); settings::set_i("hist_swap", m); return true; }
    if (_stricmp(cmd, "vollend") == 0) { const int m = atoi(args); picsplit::vol_lend(m); settings::set_i("vol_lend", m); return true; }
    if (_stricmp(cmd, "eyestab") == 0) { picsplit::eye_stab(atoi(args) > 0 ? atoi(args) : 180); return true; }
    if (_stricmp(cmd, "histab") == 0) { picsplit::hist_ab(); return true; }
    if (_stricmp(cmd, "writers") == 0) {   // "writers <input 0-6> [view 0|1] [frames]"
        int in = 0, v = 0, f = 2; sscanf_s(args, "%d %d %d", &in, &v, &f); picsplit::writers(in, v, f); return true;
    }
    if (_stricmp(cmd, "lightsrc") == 0) { picsplit::light_src(atoi(args) > 0 ? atoi(args) : 30); return true; }
    if (_stricmp(cmd, "passprobe") == 0) {   // "passprobe <hash hex> [frames] [dump] [rt]"
        unsigned long long h = 0; int f = 120, d = 0, rt = 0; sscanf_s(args, "%llx %d %d %d", &h, &f, &d, &rt); picsplit::pass_probe(h, f, d != 0, rt); return true;
    }
    if (_stricmp(cmd, "instcap") == 0) {
        picsplit::inst_cap(nullptr);
        return true;

    }
    if (_stricmp(cmd, "immspy") == 0) { picsplit::imm_spy(atoi(args) > 0 ? atoi(args) : 2); return true; }
    if (_stricmp(cmd, "instspy") == 0) { picsplit::inst_spy(atoi(args) > 0 ? atoi(args) : 3); return true; }
    if (_stricmp(cmd, "bindspy") == 0) { unsigned long long h = 0; sscanf_s(args, "%llx", &h); picsplit::bind_spy(h); return true; }
    if (_stricmp(cmd, "inprobe") == 0) {   // "inprobe <hash hex> <slot> [frames]"
        unsigned long long h = 0; int sl = 0, f = 90; sscanf_s(args, "%llx %d %d", &h, &sl, &f); picsplit::in_probe(h, sl, f); return true;
    }
    if (_stricmp(cmd, "stageprobe") == 0) {   // "stageprobe <own texture> <k> [frames]"
        int o = 0, k = 1, f = 90; sscanf_s(args, "%d %d %d", &o, &k, &f); picsplit::stage_probe(o, k, f); return true;
    }
    if (_stricmp(cmd, "ownprobe") == 0) {   // "ownprobe <own texture 0-6> [frames] [dump]"
        int in = 0, f = 120, d = 0; sscanf_s(args, "%d %d %d", &in, &f, &d); picsplit::lit_probe(f, in, d != 0, true); return true;
    }
    if (_stricmp(cmd, "litprobe") == 0) {   // "litprobe <input 0-6> [frames]"
        int in = -1, f = 180, d = 0; sscanf_s(args, "%d %d %d", &in, &f, &d); picsplit::lit_probe(f, in, d != 0); return true;
    }
    if (_stricmp(cmd, "pairlearn") == 0) { g_learnReq = true; picsplit::dump_arm(); BVR_LOG("pairlearn: recording the next frame to find where each eye is finished"); return true; }
    if (_stricmp(cmd, "eyeburst") == 0) { bvr::vr::eye_burst::start(atoi(args) > 0 ? atoi(args) : 6); return true; }
    if (_stricmp(cmd, "flicker") == 0) { bvr::vr::black_watch::trace(atoi(args) > 0 ? atoi(args) : 200); return true; }
    if (_stricmp(cmd, "shadowres") == 0) { picsplit::shadowres_probe(atoi(args)); return true; }
    if (_stricmp(cmd, "contact") == 0) {   // `ds contact on [ray m] [strength]` - live
        int on = contact::on(); float len = contact::length(), st = contact::strength();
        sscanf_s(args, "%d %f %f", &on, &len, &st);
        contact::set(on, len, st);
        settings::set_i("contact_shadows", contact::on()); settings::set_f("contact_len", contact::length());
        settings::set_f("contact_strength", contact::strength());
        return true;
    }
    if (_stricmp(cmd, "shadowsharp") == 0) { picsplit::shadow_sharp(static_cast<float>(atof(args))); settings::set_f("shadow_sharp", picsplit::shadow_sharp_now()); return true; }
    if (_stricmp(cmd, "vpjitter") == 0) { float x = 0, y = 0; sscanf_s(args, "%f %f", &x, &y); picsplit::vp_jitter(x, y); return true; }
    if (_stricmp(cmd, "updwho") == 0) { picsplit::upd_who(atoi(args) > 0 ? atoi(args) : 4); return true; }
    if (_stricmp(cmd, "shadowxtest") == 0) { picsplit::shadowx_test(atoi(args)); return true; }
    if (_stricmp(cmd, "scenedump") == 0) { int n = 3; unsigned b = 0; sscanf_s(args, "%d %u", &n, &b); picsplit::scene_dump(n > 0 ? n : 3, b); return true; }
    if (_stricmp(cmd, "findf") == 0) {
        float a = 0, b = 0, c = 0;
        if (sscanf_s(args, "%f %f %f", &a, &b, &c) == 3) { std::thread(findf_thread, a, b, c).detach(); BVR_LOG("findf: searching %g %g %g", a, b, c); }
        return true;
    }
    if (_stricmp(cmd, "rtpoke") == 0) {   // "rtpoke <hex offset> <hex dword>": a test write into OUR view's settings
        char* e = nullptr;
        const unsigned long off = strtoul(args, &e, 16);
        const unsigned long val = e ? strtoul(e, nullptr, 16) : 0;
        void* o = g_ownObj;
        uint8_t* b = o ? static_cast<uint8_t*>(reinterpret_cast<VGet>(vt(o)[3])(o)) : nullptr;
        if (b && off + 4 <= addr::kRtSize) {
            uint32_t* q = reinterpret_cast<uint32_t*>(b + off);
            const uint32_t was = *q; *q = static_cast<uint32_t>(val);
            BVR_LOG("rtpoke: our view's settings +0x%lX  %08X -> %08X", off, was, static_cast<uint32_t>(val));
        } else BVR_LOG("rtpoke: our view has no settings object yet");
        return true;
    }
    if (_stricmp(cmd, "pokef") == 0) {   // "pokef <hex address> <float>"
        unsigned long long at = 0; float v = 0;
        if (sscanf_s(args, "%llx %f", &at, &v) == 2 && readable_ptr(static_cast<uintptr_t>(at), 4)) {
            float* q = reinterpret_cast<float*>(at);
            const float was = *q; *q = v;
            BVR_LOG("pokef: %p %g -> %g", q, was, v);
        }
        return true;
    }
    if (_stricmp(cmd, "shadowdiff") == 0) { picsplit::shadow_diff(atoi(args)); return true; }
    if (_stricmp(cmd, "suntrace") == 0) { picsplit::sun_trace(atoi(args) > 0 ? atoi(args) : 120); return true; }
    if (_stricmp(cmd, "cbdump") == 0) { picsplit::cb_dump(atoi(args) > 0 ? atoi(args) : 2); return true; }
    if (_stricmp(cmd, "cbspy") == 0) { const int n = atoi(args); picsplit::cb_spy(n != 0, n > 1 ? static_cast<uint32_t>(n) : 0); return true; }
    if (_stricmp(cmd, "twodraw") == 0) {
        const bool on = atoi(args) != 0;
        if (on) { g_wantView.store(false); picsplit::set_pair_mode(2); }   // our own second view must come out of the list
        g_twoDraw.store(on);
        BVR_LOG("twodraw: the game's OWN view is drawn %s", on ? "TWICE a frame, once per eye (second view off)" : "once (back to the second view)");
        return true;
    }
    if (_stricmp(cmd, "pairmode") == 0) {  // 0 = copy after each view's last TAGGED write, 1 = sticky (post chain included)
        const int m = atoi(args) ? 1 : 0;
        picsplit::set_pair_mode(m);
        return true;
    }
    if (_stricmp(cmd, "rtcopy") == 0) {
        // "off" | "auto" (every range of the last rtdiff) | "0xA-0xB,0xC-0xD" (byte offsets, end exclusive)
        int n = 0;
        if (!_strnicmp(args, "add", 3)) { // union: the current set plus every range of the last rtdiff
            n = g_copyN.load();
            for (int i = 0; i < g_diffN && n < 256; ++i) {
                bool have = false;
                for (int k = 0; k < n; ++k) if (g_copy[k].off == g_diff[i].off && g_copy[k].len >= g_diff[i].len) have = true;
                if (!have) g_copy[n++] = g_diff[i];
            }
        } else if (!_strnicmp(args, "auto", 4)) {
            for (int i = 0; i < g_diffN && n < 256; ++i) g_copy[n++] = g_diff[i];
        } else if (_strnicmp(args, "off", 3) != 0) {
            const char* p = args;
            while (*p && n < 256) {
                char* e = nullptr;
                const unsigned long a = strtoul(p, &e, 0);
                if (e == p || *e != '-') break;
                const unsigned long b = strtoul(e + 1, &e, 0);
                if (b > a && b <= addr::kRtSize) g_copy[n++] = {static_cast<uint32_t>(a), static_cast<uint32_t>(b - a)};
                p = e; while (*p == ',' || *p == ' ') ++p;
            }
        }
        g_copyN.store(n);
        BVR_LOG("rtcopy: %d ranges copied from the main view into ours every frame", n);
        return true;
    }
    return false;
}

} // namespace bvr::ds3::stereo

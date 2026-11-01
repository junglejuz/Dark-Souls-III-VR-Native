// DS3VR2 LIGHTGRID - see lightgrid.h.
#include "lightgrid.h"

#include "addresses.h"
#include "offaxis.h"
#include "settings.h"
#include "core/util/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
void* ds3_lightgrid_back = nullptr;   // read by the stub: where it jumps back to
void ds3_lightgrid_stub();
}

namespace bvr::ds3::lightgrid {
namespace {

constexpr uintptr_t kSeam = 0x14023514C;      // the join of the grid builder's two window paths (STATIC 2026-10-30)
constexpr uintptr_t kBack = 0x140235162;      // first instruction after the 22 replaced bytes
constexpr uint8_t kOrig[22] = {0x8B, 0x7C, 0x24, 0x28, 0xF3, 0x0F, 0x11, 0x54, 0x24, 0x48, 0xF3,
                               0x0F, 0x11, 0x4C, 0x24, 0x54, 0xF3, 0x0F, 0x11, 0x7C, 0x24, 0x24};
// the window's stack slots in the grid builder's frame
constexpr int kLeft = 0x24, kHeight = 0x48, kWidth = 0x54, kRight = 0x58, kTop = 0x70;

std::atomic<int> g_mode{1};
std::atomic<uint32_t> g_calls{0}, g_fixed[2] = {}, g_other{0}, g_tiled{0};
bool g_patched = false;
uint8_t* g_site = nullptr;
struct Win { float l, r, t, w, h; };
Win g_lastGame[2] = {}, g_lastOurs[2] = {};   // logging only (torn reads are harmless)
char g_status[400] = "lightgrid: not installed";

float& at(uint8_t* f, int off) { return *reinterpret_cast<float*>(f + off); }

void* alloc_near(uintptr_t anchor, size_t bytes) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t step = gran; step < 0x60000000ULL; step += gran)
        for (int dir = 0; dir < 2; ++dir) {
            const uintptr_t want = dir ? anchor + step : (anchor > step ? anchor - step : 0);
            if (!want) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(want & ~(gran - 1)), bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return p;
        }
    return nullptr;
}

} // namespace
} // namespace bvr::ds3::lightgrid

// Called by the stub with the camera and the grid builder's rsp; rewrites the window for an eye camera.
extern "C" void ds3_lightgrid_fix(const uint8_t* cam, uint8_t* f) {
    using namespace bvr::ds3;
    using namespace bvr::ds3::lightgrid;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const int m = g_mode.load(std::memory_order_relaxed);
    if (!m || !offaxis::on() || !cam) return;
    if (*reinterpret_cast<const int*>(cam + 0x70) > 1) { g_tiled.fetch_add(1, std::memory_order_relaxed); return; }   // tiled screenshot
    const int e = offaxis::eye_of_cam(cam);
    if (e < 0) { g_other.fetch_add(1, std::memory_order_relaxed); return; }
    float w[4];
    offaxis::window(e, w);
    float l = w[0], r = w[1];
    float b = w[2], t = w[3];
    if (offaxis::mirror_cam(cam, e)) { const float nb = -t; t = -b; b = nb; }   // MIRRORFLIP: the reflection pass's grid
    if (!(r > l) || !(t > b)) return;
    if (m == 2) { const float nl = -r; r = -l; l = nl; }
    g_lastGame[e] = {at(f, kLeft), at(f, kRight), at(f, kTop), at(f, kWidth), at(f, kHeight)};
    at(f, kLeft) = l; at(f, kRight) = r; at(f, kWidth) = r - l;
    at(f, kTop) = t; at(f, kHeight) = t - b;
    g_lastOurs[e] = {l, r, t, r - l, t - b};
    g_fixed[e].fetch_add(1, std::memory_order_relaxed);
}

namespace bvr::ds3::lightgrid {

void install() {
    g_mode.store(settings::i("lightgrid", 1));
    g_site = reinterpret_cast<uint8_t*>(addr::rebase(kSeam));
    if (memcmp(g_site, kOrig, sizeof(kOrig)) != 0) {
        snprintf(g_status, sizeof(g_status), "lightgrid: the game's bytes at the seam differ - NOT installed");
        BVR_LOG("%s", g_status);
        return;
    }
    ds3_lightgrid_back = reinterpret_cast<void*>(addr::rebase(kBack));
    uint8_t* tramp = static_cast<uint8_t*>(alloc_near(reinterpret_cast<uintptr_t>(g_site), 64));
    if (!tramp) { snprintf(g_status, sizeof(g_status), "lightgrid: no memory near the game - NOT installed"); BVR_LOG("%s", g_status); return; }
    tramp[0] = 0xFF; tramp[1] = 0x25; *reinterpret_cast<int32_t*>(tramp + 2) = 0;   // jmp qword ptr [rip+0] (no register used)
    *reinterpret_cast<void**>(tramp + 6) = reinterpret_cast<void*>(&ds3_lightgrid_stub);
    DWORD old = 0;
    VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old);
    const int64_t rel = static_cast<int64_t>(reinterpret_cast<uintptr_t>(tramp)) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(g_site) + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) { snprintf(g_status, sizeof(g_status), "lightgrid: trampoline out of reach - NOT installed"); BVR_LOG("%s", g_status); return; }
    uint8_t patch[sizeof(kOrig)];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(rel);
    if (!VirtualProtect(g_site, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) { BVR_LOG("lightgrid: VirtualProtect failed"); return; }
    memcpy(g_site, patch, sizeof(patch));
    VirtualProtect(g_site, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_site, sizeof(patch));
    g_patched = true;
    snprintf(g_status, sizeof(g_status), "lightgrid: seam %p patched (stub %p)", g_site, tramp);
    BVR_LOG("%s | mode %d", g_status, g_mode.load());
}

void set_mode(int m) {
    g_mode.store(m < 0 || m > 2 ? 1 : m);
    settings::set_i("lightgrid", g_mode.load());
    BVR_LOG("lightgrid: mode %d (%s)", g_mode.load(), g_mode.load() == 0 ? "the game's centred window" :
            g_mode.load() == 1 ? "each eye's own window" : "each eye's window mirrored left/right (test)");
}
int mode() { return g_mode.load(); }

const char* status() {
    static char s[900];
    const bool intact = g_patched && g_site && g_site[0] == 0xE9;   // Arxan puts guarded bytes back: read them
    snprintf(s, sizeof(s),
             "%s | mode %d | patch %s | builds %u: eye L %u R %u, other cameras %u, tiled %u | "
             "L game [%.3f..%.3f] top %.3f h %.3f -> ours [%.3f..%.3f] top %.3f h %.3f | "
             "R game [%.3f..%.3f] top %.3f h %.3f -> ours [%.3f..%.3f] top %.3f h %.3f",
             g_status, g_mode.load(), !g_patched ? "not applied" : intact ? "in place" : "GONE (the game restored its bytes)",
             g_calls.load(), g_fixed[0].load(), g_fixed[1].load(), g_other.load(), g_tiled.load(),
             g_lastGame[0].l, g_lastGame[0].r, g_lastGame[0].t, g_lastGame[0].h, g_lastOurs[0].l, g_lastOurs[0].r, g_lastOurs[0].t, g_lastOurs[0].h,
             g_lastGame[1].l, g_lastGame[1].r, g_lastGame[1].t, g_lastGame[1].h, g_lastOurs[1].l, g_lastOurs[1].r, g_lastOurs[1].t, g_lastOurs[1].h);
    return s;
}

} // namespace bvr::ds3::lightgrid

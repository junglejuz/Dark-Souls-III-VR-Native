// DS3VR2 ENVTIME - see envtime.h.
#include "envtime.h"

#include "core/util/log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace bvr::ds3::envtime {
namespace {

// These three are SEKIRO's addresses, kept as the shape of what to look for in Dark Souls III (M4L). Until they are
// found and proven here, `kAddressesAreForThisGame` keeps the whole module out of the game's code.
constexpr bool kAddressesAreForThisGame = false;
constexpr uintptr_t kRebuild = 0x1401F40B0;   // (mgr = G+0x11368): dedupe + sort the light environments, assign slots
constexpr uintptr_t kBuilder = 0x14019B820;   // fills one 544-byte instance block; envData = mgr->tables[h.low][h.high].slot

using RebuildFn = uint64_t(__fastcall*)(void*);
using BuilderFn = uint64_t(__fastcall*)(uint64_t, void*, void*, void*, void*, uint64_t, void*, void*, void*, uint64_t, uint64_t);
RebuildFn oRebuild = nullptr;
BuilderFn oBuilder = nullptr;

std::atomic<int> g_frames{0};
std::mutex g_mx;
LARGE_INTEGER g_t0{}, g_freq{};
double ms(const LARGE_INTEGER& t) { return 1000.0 * static_cast<double>(t.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart); }

struct Ev { double t; DWORD tid; char what[96]; };
Ev g_ev[64]; int g_evN = 0;
struct Ctx { void* ctx; DWORD tid; double first, last; uint32_t n; uint32_t low[9]; uint32_t env[6]; };
Ctx g_ctx[8]; int g_ctxN = 0;

void add_event(const char* what) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_evN < 64) { Ev& e = g_ev[g_evN++]; e.t = ms(t); e.tid = GetCurrentThreadId(); strncpy_s(e.what, what, _TRUNCATE); }
}

std::atomic<int> g_share{0};          // ENVSHARE: 1 = only the first rebuild of a render-all runs (both eyes share it)
std::atomic<int> g_callInRall{0};
std::atomic<uint32_t> g_skipped{0}, g_ran{0};

// ENVONCE (L7 2026-10-29): the engine's light-environment manager (G+0x11368) is ONE object for the whole renderer, but
// each view's scene prepare (0x14015BA10) resets its 8 per-worker tables (counts at mgr+0x48), its culling workers
// re-register every object's light environment (0x1401F3F30, which writes the object's handle = table | record << 32),
// and the render thread rebuilds the slots (0x1401F40B0). The game view's instance blocks are built by workers that are
// still running when OUR view does that (ENVTIME: rebuild #2 at 1.4 ms, the game view's blocks until 3.1 ms), so they
// read handles and slots our view is rewriting: envData (GBuffer3.a, the sun pass's light set) comes out scrambled and
// the left eye flickers between two lightings. Light environments are world-space - the same for both eyes - so the
// second view reuses the first view's: its reset is undone, its registrations return the first view's identical record
// (any table), and its rebuild runs only if it met an environment the first view did not have.
constexpr uintptr_t kRegister = 0x1401F3F30;  // (mgr, out handle, keys..., priority) - per-worker table, dedupe or append
using RegisterFn = uint64_t*(__fastcall*)(uint8_t*, uint64_t*, const uint32_t*, const uint32_t*, float, const uint32_t*, const uint32_t*,
                                          float, uint32_t);
RegisterFn oRegister = nullptr;
std::atomic<int> g_once{0};
std::atomic<bool> g_phaseB{false};      // the first view's rebuild is done: registrations now are the second view's
uint32_t g_countsA[8] = {};            // the first view's table counts, right after its rebuild
bool g_restored = false;
std::mutex g_regMx;
std::atomic<uint32_t> g_reused{0}, g_appended{0}, g_restores{0}, g_rebuildsSkipped{0};
uint32_t g_appendedThisFrame = 0;

uint64_t* __fastcall hkRegister(uint8_t* mgr, uint64_t* out, const uint32_t* k8, const uint32_t* k10, float f28, const uint32_t* k18,
                                const uint32_t* k20, float f2c, uint32_t v30) {
    if (!g_once.load(std::memory_order_relaxed) || !g_phaseB.load(std::memory_order_acquire) || !mgr || !out)
        return oRegister(mgr, out, k8, k10, f28, k18, k20, f2c, v30);
    std::lock_guard<std::mutex> lk(g_regMx);
    uint32_t* cnt = reinterpret_cast<uint32_t*>(mgr + 0x48);
    if (!g_restored) {                 // undo the second view's reset: the first view's records stay live
        for (int t = 0; t < 8; ++t) if (cnt[t] < g_countsA[t]) cnt[t] = g_countsA[t];
        g_restored = true;
        g_restores.fetch_add(1, std::memory_order_relaxed);
    }
    for (uint32_t t = 0; t < 8; ++t) {
        const uint8_t* tab = *reinterpret_cast<uint8_t* const*>(mgr + 8 + 8 * t);
        if (!tab) continue;
        for (uint32_t i = 0; i < cnt[t]; ++i) {
            const uint8_t* r = tab + i * 0x38;
            const uint32_t* u = reinterpret_cast<const uint32_t*>(r);
            if (u[3] == k8[1] && u[2] == k8[0] && u[5] == k10[1] && u[4] == k10[0] && u[7] == k18[1] && u[6] == k18[0] &&
                u[9] == k20[1] && u[8] == k20[0] && *reinterpret_cast<const float*>(r + 0x28) == f28 &&
                *reinterpret_cast<const float*>(r + 0x2C) == f2c) {
                *out = static_cast<uint64_t>(t) | (static_cast<uint64_t>(i) << 32);
                g_reused.fetch_add(1, std::memory_order_relaxed);
                return out;
            }
        }
    }
    ++g_appendedThisFrame;             // new to the second eye: the engine's own path, the slots get rebuilt for it
    g_appended.fetch_add(1, std::memory_order_relaxed);
    return oRegister(mgr, out, k8, k10, f28, k18, k20, f2c, v30);
}

uint64_t __fastcall hkRebuild(void* mgr) {
    const int k = g_callInRall.fetch_add(1, std::memory_order_relaxed);
    if (g_frames.load(std::memory_order_relaxed) > 0 && mgr) {   // the 8 tables' entry counts (mgr+0x48) and the list size (mgr+0x80?)
        const uint32_t* cnt = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mgr) + 0x48);
        char e[96]; sprintf_s(e, "rebuild #%d counts %u %u %u %u %u %u %u %u", k, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7]);
        add_event(e);
    }
    if (g_share.load(std::memory_order_relaxed) && k > 0) { g_skipped.fetch_add(1, std::memory_order_relaxed); return 0; }
    if (g_once.load(std::memory_order_relaxed) && k > 0) {
        std::lock_guard<std::mutex> lk(g_regMx);
        if (!g_appendedThisFrame) { g_rebuildsSkipped.fetch_add(1, std::memory_order_relaxed); return 0; }   // nothing new: keep the slots
    }
    g_ran.fetch_add(1, std::memory_order_relaxed);
    const uint64_t r = oRebuild(mgr);
    if (k == 0 && mgr) {               // the first view's environments are final: snapshot them for the second view
        std::lock_guard<std::mutex> lk(g_regMx);
        memcpy(g_countsA, static_cast<const uint8_t*>(mgr) + 0x48, sizeof(g_countsA));
        g_restored = false;
        g_appendedThisFrame = 0;
        g_phaseB.store(true, std::memory_order_release);
    }
    if (g_frames.load(std::memory_order_relaxed) > 0) add_event("slot rebuild end");
    return r;
}

uint64_t __fastcall hkBuilder(uint64_t a1, void* ctx, void* drawCtx, void* obj, void* a5, uint64_t a6, void* out, void* a8, void* a9,
                              uint64_t a10, uint64_t a11) {
    const uint64_t r = oBuilder(a1, ctx, drawCtx, obj, a5, a6, out, a8, a9, a10, a11);
    if (g_frames.load(std::memory_order_relaxed) <= 0 || !obj || !out) return r;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    const uint8_t* r14 = *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(obj) + 0x138);
    const uint64_t h = r14 ? *reinterpret_cast<const uint64_t*>(r14 + 0xE0) : ~0ull;
    const uint32_t kind = drawCtx ? *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(drawCtx) + 0x1C) : 0;
    const uint32_t env = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(out) + 0x50);
    std::lock_guard<std::mutex> lk(g_mx);
    Ctx* c = nullptr;
    for (int i = 0; i < g_ctxN; ++i) if (g_ctx[i].ctx == ctx) c = &g_ctx[i];
    if (!c && g_ctxN < 8) { c = &g_ctx[g_ctxN++]; *c = {}; c->ctx = ctx; c->tid = GetCurrentThreadId(); c->first = ms(t); }
    if (!c) return r;
    c->last = ms(t);
    ++c->n;
    if (kind == 8) ++c->low[static_cast<uint32_t>(h) < 8 ? static_cast<uint32_t>(h) : 8];
    ++c->env[env < 5 ? env : 5];
    return r;
}

} // namespace

bool install() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    // DS3 (2026-10-30): the three addresses above are SEKIRO's. Dark Souls III's light-environment manager has not
    // been reverse-engineered yet, so this module stays off until M4L finds and proves its own three functions.
    // Hooking Sekiro addresses in DarkSoulsIII.exe would patch arbitrary code.
    if (!kAddressesAreForThisGame) {
        BVR_LOG("envtime: off - the light-environment functions have not been found for Dark Souls III yet (M4L)");
        return false;
    }
    const MH_STATUS a = MH_CreateHook(reinterpret_cast<void*>(kRebuild), reinterpret_cast<void*>(&hkRebuild), reinterpret_cast<void**>(&oRebuild));
    const MH_STATUS b = MH_CreateHook(reinterpret_cast<void*>(kBuilder), reinterpret_cast<void*>(&hkBuilder), reinterpret_cast<void**>(&oBuilder));
    const MH_STATUS c = MH_CreateHook(reinterpret_cast<void*>(kRegister), reinterpret_cast<void*>(&hkRegister), reinterpret_cast<void**>(&oRegister));
    if (c == MH_OK) MH_QueueEnableHook(reinterpret_cast<void*>(kRegister));
    if (a == MH_OK) MH_QueueEnableHook(reinterpret_cast<void*>(kRebuild));
    if (b == MH_OK) MH_QueueEnableHook(reinterpret_cast<void*>(kBuilder));
    BVR_LOG("envtime: light-env slot rebuild %s, instance-block builder %s, registration %s", MH_StatusToString(a), MH_StatusToString(b),
            MH_StatusToString(c));
    return a == MH_OK && b == MH_OK && c == MH_OK;
}

void mark(const char* what) { if (g_frames.load(std::memory_order_relaxed) > 0) add_event(what); }
void render_all_begin() { g_callInRall.store(0, std::memory_order_relaxed); g_phaseB.store(false, std::memory_order_release); }
void once(int on) {
    g_once.store(on);
    BVR_LOG("envonce %d: %s", on, on ? "both eyes use ONE set of light environments per frame (the second view reuses the first view's records)"
                                    : "every view re-registers and rebuilds the light environments (stock)");
}
void share(int on) { g_share.store(on); BVR_LOG("envshare %d: %s", on, on ? "only the first light-environment rebuild of each frame runs; both eyes use it"
                                                                           : "the engine rebuilds the light environments for every view (stock)"); }
void log_counts() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 5000) return;
    last = now;
    BVR_LOG("envshare %d / envonce %d in 5 s: rebuilds run %u, skipped %u (share) %u (once, nothing new) | second-view registrations: reused %u, "
            "new %u, resets undone %u", g_share.load(), g_once.load(), g_ran.exchange(0), g_skipped.exchange(0), g_rebuildsSkipped.exchange(0),
            g_reused.exchange(0), g_appended.exchange(0), g_restores.exchange(0));
}

void on_present() {
    log_counts();
    if (g_frames.load() <= 0) return;
    add_event("Present");
    std::lock_guard<std::mutex> lk(g_mx);
    for (int i = 0; i < g_evN; ++i) BVR_LOG("envtime %8.3f ms  tid %5lu  %s", g_ev[i].t, g_ev[i].tid, g_ev[i].what);
    for (int i = 0; i < g_ctxN; ++i) {
        const Ctx& c = g_ctx[i];
        BVR_LOG("envtime   instance blocks of render ctx %p (tid %lu): %u blocks %8.3f .. %8.3f ms | handle table 0-7: %u %u %u %u %u %u %u %u | slot 0:%u 1:%u 2:%u 3:%u 4:%u >4:%u",
                c.ctx, c.tid, c.n, c.first, c.last, c.low[0], c.low[1], c.low[2], c.low[3], c.low[4], c.low[5], c.low[6], c.low[7],
                c.env[0], c.env[1], c.env[2], c.env[3], c.env[4], c.env[5]);
    }
    BVR_LOG("envtime: ---- frame ----");
    g_evN = 0; g_ctxN = 0;
    g_frames.fetch_sub(1);
}

void arm(int frames) {
    { std::lock_guard<std::mutex> lk(g_mx); g_evN = 0; g_ctxN = 0; QueryPerformanceCounter(&g_t0); }
    g_frames.store(frames);
    BVR_LOG("envtime: timing the light-env slot rebuild against each view's instance blocks for %d frames", frames);
}

} // namespace bvr::ds3::envtime

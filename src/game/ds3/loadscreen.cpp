// LOADSCREEN (ported from SKVR2 2026-11-01, JJ: "when first entering gameplay from the loading screen ... we see a window
// appear on the screen which should fade out before the actual game appears. We fixed this in Sekiro"). The mod switched to
// 3D as soon as the second view was listed, but the game draws its world behind its loading screen and keeps drawing the
// loading screen - fading it out - for a while after; the room screen in between shows that in a box. The engine itself
// says when its loading screen is on: the Scaleform movie is DRAWN. FromSoft draws each movie at one site; in Dark Souls III
// (STATIC, capstone, same shape as Sekiro's 0x1410F17BF): 0x140E530EF.. loads the renderer [[rsi+8]+0x980], fetches the
// movie root through 0x141BD6210(rsi+0x30) (return 0x140E53103) and calls Scaleform Renderer2D::Display 0x141BDA3A0
// (return 0x140E5310E). player = handle - 0x30; the movie and its name chain are Sekiro's (player+0x18,
// [[[[movie+0x50]+0x70]+0x18]+0x40] -> *(def+0x48) + 0xB) - unproven on DS3, every name is logged once to check it.
#include "game/ds3/loadscreen.h"

#include "core/util/log.h"

#include <windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace bvr::ds3::loadscreen {
namespace {

constexpr uintptr_t kGetRoot = 0x141BD6210;
constexpr uintptr_t kDisplay = 0x141BDA3A0;
constexpr uintptr_t kSite = 0x140E530EF;
constexpr uintptr_t kSiteRoot = 0x140E53103, kSiteDisp = 0x140E5310E;

using GetRootFn = void*(__fastcall*)(void*);
using DisplayFn = uintptr_t(__fastcall*)(void*, void*, void*, void*);
GetRootFn oGetRoot = nullptr;
DisplayFn oDisplay = nullptr;
thread_local void* t_player = nullptr;
bool g_ok = false;

std::atomic<ULONGLONG> g_loadMs{0};
std::atomic<uint32_t> g_loadDraws{0}, g_draws{0};

struct Seen { void* movie; void* def; uint8_t kind; };   // kind 0 other, 1 loading
Seen g_seen[64];
int g_seenN = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

void* movie_def(void* movie) {
    __try {
        const uint8_t* a = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(movie) + 0x50); if (!a) return nullptr;
        const uint8_t* b = *reinterpret_cast<uint8_t* const*>(a + 0x70); if (!b) return nullptr;
        const uint8_t* c = *reinterpret_cast<uint8_t* const*>(b + 0x18); if (!c) return nullptr;
        return *reinterpret_cast<void* const*>(c + 0x40);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool def_name(void* def, char* out, size_t n) {
    __try {
        const char* nm = *reinterpret_cast<const char* const*>(static_cast<uint8_t*>(def) + 0x48);
        if (!nm) return false;
        nm += 0xB;
        size_t i = 0;
        for (; i + 1 < n && nm[i]; ++i) out[i] = (nm[i] >= 32 && nm[i] < 127) ? nm[i] : '?';
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool has_ci(const char* s, const char* sub) {
    for (; *s; ++s) {
        size_t i = 0;
        while (sub[i] && s[i] && ((s[i] | 0x20) == (sub[i] | 0x20))) ++i;
        if (!sub[i]) return true;
    }
    return false;
}
uint8_t kind_of(void* movie) {
    void* def = movie_def(movie);
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_seenN; ++i)
        if (g_seen[i].movie == movie && g_seen[i].def == def) { const uint8_t k = g_seen[i].kind; ReleaseSRWLockExclusive(&g_lock); return k; }
    char nm[96] = "?";
    const bool named = def && def_name(def, nm, sizeof(nm));
    const uint8_t k = named && has_ci(nm, "loading") ? 1 : 0;
    int slot = -1;
    for (int i = 0; i < g_seenN; ++i) if (g_seen[i].movie == movie) { slot = i; break; }   // same address, new movie
    if (slot < 0) slot = g_seenN < 64 ? g_seenN++ : static_cast<int>(GetTickCount64() % 64);
    g_seen[slot] = {movie, def, k};
    ReleaseSRWLockExclusive(&g_lock);
    BVR_LOG("loadscreen: movie %p drawn = %s%s", movie, named ? nm : "(name not readable - offsets differ?)", k == 1 ? "  (the LOADING screen)" : "");
    return k;
}

void* __fastcall hkGetRoot(void* handle) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == kSiteRoot) t_player = static_cast<uint8_t*>(handle) - 0x30;
    return oGetRoot(handle);
}
uintptr_t __fastcall hkDisplay(void* r, void* root, void* a3, void* a4) {
    void* player = t_player; t_player = nullptr;
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == kSiteDisp && player) {
        void* movie = nullptr;
        __try { movie = *reinterpret_cast<void**>(static_cast<uint8_t*>(player) + 0x18); } __except (EXCEPTION_EXECUTE_HANDLER) { movie = nullptr; }
        if (movie) {
            g_draws.fetch_add(1, std::memory_order_relaxed);
            if (kind_of(movie) == 1) { g_loadMs.store(GetTickCount64(), std::memory_order_relaxed); g_loadDraws.fetch_add(1, std::memory_order_relaxed); }
        }
    }
    return oDisplay(r, root, a3, a4);
}

} // namespace

bool g_pending = false;   // V13z3: the code was still all zeros at install (not unpacked yet) - retry from the Present
bool check_and_hook(bool log, bool enableNow);
bool install() { return check_and_hook(true, false); }
void retry() {   // once a second from the Present until the code is there (gives up after 2 minutes)
    static int tries = 0;
    if (!g_pending || g_ok) return;
    static ULONGLONG last = 0;
    const ULONGLONG t = GetTickCount64();
    if (t - last < 1000) return;
    last = t;
    if (++tries > 120) { g_pending = false; BVR_LOG("loadscreen: the game's code never matched - signal off"); return; }
    check_and_hook(false, true);
}
bool check_and_hook(bool log, bool enableNow) {
    static const uint8_t gr[] = {0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x0B, 0x83, 0x78, 0x28, 0x01};
    static const uint8_t dp[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55};
    static const uint8_t site[] = {0x48, 0x8B, 0x46, 0x08, 0x48, 0x8B, 0x98, 0x80, 0x09, 0x00, 0x00, 0x48, 0x8D, 0x4E, 0x30, 0xE8};
    // V13z2 (first launch: "bytes differ"): name the check that failed and log the bytes found
    struct C { const char* what; uintptr_t at; const uint8_t* want; size_t n; } checks[] = {
        {"movie root fetch", kGetRoot, gr, sizeof(gr)}, {"Scaleform Display", kDisplay, dp, sizeof(dp)}, {"draw site", kSite, site, sizeof(site)}};
    bool okb = true, zero = true;
    for (const C& c : checks) {
        uint8_t got[16] = {};
        bool same = false;
        __try { memcpy(got, reinterpret_cast<void*>(c.at), c.n); same = memcmp(got, c.want, c.n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
        for (size_t i = 0; i < c.n; ++i) if (got[i]) zero = false;
        if (!same && log) {
            char hex[64] = {}; int k = 0;
            for (size_t i = 0; i < c.n && k < 60; ++i) k += sprintf_s(hex + k, sizeof(hex) - k, "%02X ", got[i]);
            BVR_LOG("loadscreen: %s at %llX differs - found %s", c.what, (unsigned long long)c.at, hex);
            okb = false;
        }
    }
    if (!okb) {
        if (zero) { if (log) BVR_LOG("loadscreen: the game's code is not unpacked yet - checked again once a second"); g_pending = true; }
        else if (log || g_pending) { BVR_LOG("loadscreen: bytes differ - loading-screen signal off"); g_pending = false; }
        return false;
    }
    const MH_STATUS s1 = MH_CreateHook(reinterpret_cast<void*>(kGetRoot), reinterpret_cast<void*>(&hkGetRoot), reinterpret_cast<void**>(&oGetRoot));
    const MH_STATUS s2 = MH_CreateHook(reinterpret_cast<void*>(kDisplay), reinterpret_cast<void*>(&hkDisplay), reinterpret_cast<void**>(&oDisplay));
    g_ok = (s1 == MH_OK || s1 == MH_ERROR_ALREADY_CREATED) && (s2 == MH_OK || s2 == MH_ERROR_ALREADY_CREATED);
    if (!g_ok && (s1 == MH_ERROR_NOT_EXECUTABLE || s2 == MH_ERROR_NOT_EXECUTABLE)) {
        // V13z5 (00:37:50): the code is unpacked but its pages are not executable yet - keep waiting (retry once a second)
        if (s1 == MH_OK) MH_RemoveHook(reinterpret_cast<void*>(kGetRoot));
        if (s2 == MH_OK) MH_RemoveHook(reinterpret_cast<void*>(kDisplay));
        static bool said = false;
        if (!said) { said = true; BVR_LOG("loadscreen: the game's code is not executable yet - checked again once a second"); }
        g_pending = true;
        return false;
    }
    if (!g_ok) BVR_LOG("loadscreen: hook status root %s, Display %s", MH_StatusToString(s1), MH_StatusToString(s2));
    if (g_ok && enableNow) { MH_QueueEnableHook(reinterpret_cast<void*>(kGetRoot)); MH_QueueEnableHook(reinterpret_cast<void*>(kDisplay)); MH_ApplyQueued(); }
    else if (g_ok) { MH_QueueEnableHook(reinterpret_cast<void*>(kGetRoot)); MH_QueueEnableHook(reinterpret_cast<void*>(kDisplay)); }
    g_pending = false;
    BVR_LOG("loadscreen: movie draw hooks %s", g_ok ? (enableNow ? "ON (the game's code is unpacked now)" : "queued") : "FAILED");
    return g_ok;
}

bool ok() { return g_ok; }
// TRAVELCLEAN2: was the loading screen drawn since the last call (once per Present)?
bool drawn_this_frame() { static uint32_t seen = 0; const uint32_t n = g_loadDraws.load(std::memory_order_relaxed); const bool d = n != seen; seen = n; return d; }
ULONGLONG ms_since_loading() { const ULONGLONG t = g_loadMs.load(std::memory_order_relaxed); return t ? GetTickCount64() - t : ~0ull; }

} // namespace bvr::ds3::loadscreen

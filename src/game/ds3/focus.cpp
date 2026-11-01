// DS3VR2 KEEPFOCUS - see focus.h.
#include "focus.h"

#include "settings.h"
#include "core/util/log.h"

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <intrin.h>

namespace bvr::ds3::focus {
namespace {

std::atomic<bool> g_on{false};
std::atomic<HWND> g_wnd{nullptr};
DWORD g_wndThread = 0;
WNDPROC g_prevProc = nullptr;
std::atomic<uint32_t> g_lies{0}, g_msgs{0};

using GetWndFn = HWND(WINAPI*)();
GetWndFn g_gfwOrig = nullptr, g_gawOrig = nullptr, g_gfOrig = nullptr;

HWND game() { return g_on.load(std::memory_order_relaxed) ? g_wnd.load(std::memory_order_relaxed) : nullptr; }
// our own module (the pace trace's fg column, KEEPFOCUS status) is told the truth. Its address range is taken once at
// install: asking Windows per call (GetModuleHandleEx) takes the loader lock, which a game thread must never wait on here.
uintptr_t g_selfLo = 0, g_selfHi = 0;
bool from_us(void* ret) { const uintptr_t a = reinterpret_cast<uintptr_t>(ret); return a >= g_selfLo && a < g_selfHi; }
HWND WINAPI gfw_detour() {
    if (HWND w = game()) if (!from_us(_ReturnAddress())) { g_lies.fetch_add(1, std::memory_order_relaxed); return w; }
    return g_gfwOrig();
}
// GetActiveWindow / GetFocus are per thread by nature: only the window's own thread is told it has them
HWND WINAPI gaw_detour() { HWND w = game(); return w && GetCurrentThreadId() == g_wndThread ? w : g_gawOrig(); }
HWND WINAPI gf_detour() { HWND w = game(); return w && GetCurrentThreadId() == g_wndThread ? w : g_gfOrig(); }

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_on.load(std::memory_order_relaxed)) {
        switch (msg) {
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE) { g_msgs.fetch_add(1, std::memory_order_relaxed); return 0; }
            break;
        case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) g_msgs.fetch_add(1, std::memory_order_relaxed); wp = MAKEWPARAM(WA_ACTIVE, 0); break;
        case WM_ACTIVATEAPP: if (!wp) g_msgs.fetch_add(1, std::memory_order_relaxed); wp = TRUE; break;
        case WM_NCACTIVATE: wp = TRUE; break;
        case WM_KILLFOCUS: g_msgs.fetch_add(1, std::memory_order_relaxed); return 0;
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
        }
    }
    return CallWindowProcW(g_prevProc, hwnd, msg, wp, lp);
}

char g_status[200] = "keepfocus: not installed";

} // namespace

void install() {
    const bool want = settings::i("keep_focus", 1) != 0;
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&install), &self) && self) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
            g_selfLo = reinterpret_cast<uintptr_t>(self);
            g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;
        }
    }
    void* t[3] = {};
    const MH_STATUS a = MH_CreateHookApiEx(L"user32", "GetForegroundWindow", reinterpret_cast<void*>(&gfw_detour), reinterpret_cast<void**>(&g_gfwOrig), &t[0]);
    const MH_STATUS b = MH_CreateHookApiEx(L"user32", "GetActiveWindow", reinterpret_cast<void*>(&gaw_detour), reinterpret_cast<void**>(&g_gawOrig), &t[1]);
    const MH_STATUS c = MH_CreateHookApiEx(L"user32", "GetFocus", reinterpret_cast<void*>(&gf_detour), reinterpret_cast<void**>(&g_gfOrig), &t[2]);
    for (void* p : t) if (p) MH_EnableHook(p);
    const bool ok = a == MH_OK && b == MH_OK && c == MH_OK;
    g_on.store(want && ok);
    snprintf(g_status, sizeof(g_status), "keepfocus: %s (hooks %s / %s / %s)", !want ? "off (keep_focus=0)" : ok ? "on" : "FAILED",
             MH_StatusToString(a), MH_StatusToString(b), MH_StatusToString(c));
    BVR_LOG("%s - the game keeps running at full speed when its window is not in front", g_status);
}

void attach(HWND w) {
    if (!w || g_wnd.load() || !IsWindow(w)) return;
    g_wndThread = GetWindowThreadProcessId(w, nullptr);
    g_prevProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&proc)));
    if (!g_prevProc) { BVR_LOG("keepfocus: the game window's messages could not be filtered (%lu)", GetLastError()); return; }
    g_wnd.store(w);
    BVR_LOG("keepfocus: game window %p (thread %lu) - activation losses are hidden from it", static_cast<void*>(w), g_wndThread);
}

HWND real_foreground() { return g_gfwOrig ? g_gfwOrig() : GetForegroundWindow(); }

const char* status() {
    static char s[300];
    const HWND fg = real_foreground();
    snprintf(s, sizeof(s), "%s | window %p, really in front %d | front-window answers bent %u, focus-loss messages hidden %u", g_status,
             static_cast<void*>(g_wnd.load()), fg && fg == g_wnd.load() ? 1 : 0, g_lies.load(), g_msgs.load());
    return s;
}

} // namespace bvr::ds3::focus

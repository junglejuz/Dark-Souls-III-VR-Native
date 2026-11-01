// DS3VR2 OFFLINE (JJ 2026-10-30: "would be good if the game just launched in offline mode by default and
// bypassed the logging into the game server").
//
// Two things are worth separating:
//
// 1. **Playing offline is a safety matter, not a convenience.** Dark Souls III penalises modified clients on its
//    servers, and this mod patches the game's code. Every test run here has been offline and the release must be
//    too. So VR launches refuse the game's outbound connections outright.
//
// 2. **The "failed to log in" box cannot be bypassed from here.** It is the game's own dialog and it appears even
//    with no network at all - we watched it do exactly that with the machine unable to reach the servers. What
//    this module removes is the WAIT in front of it: without it the game sits on "Logging in to the DARK SOULS III
//    game server..." for about ten seconds before failing. With it the attempt fails at once. Pressing the box's
//    button still belongs to the player (or to the mod, once it can SEE the box - that needs the Scaleform work in
//    M7; a blind key press on a timer is exactly what memory `no-blind-toggle-keys` says not to do).
//
// The hooks are armed at the first graphics frame, never at start-up: Steam initialises before that and the game
// will not start without it. Only connections made after that point are refused, which is the login and the
// matchmaking that follows it.
#include "offline.h"

#include "settings.h"

#include "core/util/log.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <MinHook.h>

#include <atomic>

namespace bvr::ds3::offline {
namespace {

std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_refused{0};
bool g_installed = false;

using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using WSAConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
ConnectFn oConnect = nullptr;
WSAConnectFn oWSAConnect = nullptr;

int WSAAPI hkConnect(SOCKET s, const sockaddr* name, int len) {
    if (g_on.load(std::memory_order_relaxed)) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return oConnect(s, name, len);
}

int WSAAPI hkWSAConnect(SOCKET s, const sockaddr* name, int len, LPWSABUF cd, LPWSABUF ad, LPQOS sq, LPQOS gq) {
    if (g_on.load(std::memory_order_relaxed)) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        WSASetLastError(WSAECONNREFUSED);
        return SOCKET_ERROR;
    }
    return oWSAConnect(s, name, len, cd, ad, sq, gq);
}

} // namespace

void install() {
    if (g_installed) return;
    g_installed = true;
    const bool want = settings::i("offline", 1) != 0;
    if (!want) { BVR_LOG("offline: off (offline = 0) - the game may log in to its servers; modded clients risk a ban"); return; }
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    if (!ws) ws = LoadLibraryW(L"ws2_32.dll");
    if (!ws) { BVR_LOG("offline: ws2_32 not loaded - connections NOT blocked"); return; }
    void* c = reinterpret_cast<void*>(GetProcAddress(ws, "connect"));
    void* w = reinterpret_cast<void*>(GetProcAddress(ws, "WSAConnect"));
    int ok = 0;
    if (c && MH_CreateHook(c, reinterpret_cast<void*>(&hkConnect), reinterpret_cast<void**>(&oConnect)) == MH_OK) {
        MH_QueueEnableHook(c); ++ok;
    }
    if (w && MH_CreateHook(w, reinterpret_cast<void*>(&hkWSAConnect), reinterpret_cast<void**>(&oWSAConnect)) == MH_OK) {
        MH_QueueEnableHook(w); ++ok;
    }
    MH_ApplyQueued();
    g_on.store(ok > 0);
    BVR_LOG("offline: %d of 2 connection calls hooked - the game's server login fails at once instead of waiting "
            "(its \"failed to log in\" box is the game's own and still needs one button)", ok);
}

bool on() { return g_on.load(); }
uint32_t refused() { return g_refused.load(); }

} // namespace bvr::ds3::offline

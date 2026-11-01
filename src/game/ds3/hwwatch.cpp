// DS3VR2 HWWATCH - which game code WRITES a given address (hardware write watch, 4 bytes, every thread).
// Port of swbf2vr/src/probe.cpp hwwatch (memory hw-read-watch-finds-readers: it found SWBF2's reader on the first run).
// Used to find what fills the main view's lighting / fog / shadow settings (L1 2026-10-29) so the same code can be run
// for the second view instead of copying bytes blindly.
//
// 2026-10-29: four slots (DR0-DR3) instead of one, the value AFTER the write is logged (the trap is after the store, so
// reading it is safe and does not re-trigger), and `rearm` puts the watch on threads the game started since arming.
#include "hwwatch.h"

#include "core/util/log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace bvr::ds3::hwwatch {
namespace {

std::mutex g_lock;
std::vector<uintptr_t> g_sites;
bool g_handler = false;
uintptr_t g_addr[4] = {};
char g_what[4][64] = {};
bool g_read = false;

thread_local bool t_inHandler = false;
LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    // V12z2 (crash 19:23 with READ watches): anything this handler does that touches a watched address fires the watch
    // again inside it - with the lock held = std::system_error. Re-entry just continues; the lock is only tried.
    if (t_inHandler) { ep->ContextRecord->Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    struct In { In() { t_inHandler = true; } ~In() { t_inHandler = false; } } in;
    const DWORD64 dr6 = ep->ContextRecord->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    int slot = 0;
    for (int i = 0; i < 4; ++i) if (dr6 & (1ull << i)) { slot = i; break; }
    ep->ContextRecord->Dr6 = 0;
    const CONTEXT& c = *ep->ContextRecord;
    const uintptr_t rip = c.Rip; // the instruction AFTER the write
    std::unique_lock<std::mutex> lk(g_lock, std::try_to_lock);
    if (!lk.owns_lock()) return EXCEPTION_CONTINUE_EXECUTION;
    const uintptr_t key = rip ^ (static_cast<uintptr_t>(slot) << 56);
    if (g_sites.size() < 48 && std::find(g_sites.begin(), g_sites.end(), key) == g_sites.end()) {
        g_sites.push_back(key);
        // [rsp] and a few stack slots: the callers (never read the watched address here - it would fire again)
        // The game's own module range (DS3 has ASLR, so this is read at run time).
        const uintptr_t kModLo = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t kModHi = kModLo + 0x5800000;
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c.Rsp);
        uintptr_t calls[6] = {};
        int nc = 0;
        for (int i = 0; i < 64 && nc < 6; ++i) {
            const uintptr_t v = sp[i];
            if (v > kModLo && v < kModHi) calls[nc++] = v - kModLo;
        }
        // the value only in write mode: in read mode reading it here would be one more read of the watched address
        uint32_t wv = 0;
        if (!g_read) wv = *reinterpret_cast<const uint32_t*>(g_addr[slot]);
        const uint32_t* w = &wv;
        float f = 0.0f; memcpy(&f, &wv, 4);
        BVR_LOG("hwwatch hit [%s]: reader/writer ends at exe+0x%llX (tid %lu), the value is now %08X (%.6g); stack exe+ %llX %llX %llX %llX %llX %llX",
                g_what[slot], static_cast<unsigned long long>(rip - kModLo), GetCurrentThreadId(), *w, f,
                calls[0], calls[1], calls[2], calls[3], calls[4], calls[5]);
        BVR_LOG("  rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX rbp %llX", c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp);
        BVR_LOG("  r8 %llX r9 %llX r12 %llX r13 %llX r14 %llX r15 %llX", c.R8, c.R9, c.R12, c.R13, c.R14, c.R15);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Write the four slots into one thread's debug registers.
void apply(CONTEXT& c) {
    DWORD64 dr7 = c.Dr7;
    for (int i = 0; i < 4; ++i) {
        const DWORD64 len = 3ull << (18 + 4 * i);   // 4 bytes
        const DWORD64 rw = (g_read ? 3ull : 1ull) << (16 + 4 * i);    // on write (or read + write: CULLHUNT)
        dr7 &= ~((0x3ull << (2 * i)) | (0xFull << (16 + 4 * i)));
        if (g_addr[i]) dr7 |= (1ull << (2 * i)) | rw | len;
    }
    c.Dr0 = g_addr[0]; c.Dr1 = g_addr[1]; c.Dr2 = g_addr[2]; c.Dr3 = g_addr[3];
    c.Dr7 = dr7;
}

int arm_all() {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                apply(c);
                if (SetThreadContext(t, &c)) ++n;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return n;
}

} // namespace

void arm_slot(int slot, uintptr_t addr, const char* what) {
    if (slot < 0 || slot > 3) return;
    if (!addr && !g_addr[slot]) return;                 // nothing to clear
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, handler) != nullptr;
    { std::lock_guard<std::mutex> lk(g_lock); g_sites.clear(); }
    g_addr[slot] = addr & ~3ull;
    strcpy_s(g_what[slot], what ? what : "?");
    BVR_LOG("hwwatch: slot %d watches writes to %p (%s) on %d threads", slot, reinterpret_cast<void*>(g_addr[slot]), g_what[slot], arm_all());
}

void arm(uintptr_t addr, const char* what) {
    for (int i = 1; i < 4; ++i) g_addr[i] = 0;
    arm_slot(0, addr, what);
}

void rearm() {
    bool any = false;
    for (uintptr_t a : g_addr) any = any || a != 0;
    if (any) arm_all();
}

void set_read(bool on) { g_read = on; }
void off() {
    for (uintptr_t& a : g_addr) a = 0;
    BVR_LOG("hwwatch: off (%d threads)", arm_all());
}

} // namespace bvr::ds3::hwwatch

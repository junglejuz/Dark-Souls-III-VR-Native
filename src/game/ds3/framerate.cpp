// DS3VR2 frame rate - see framerate.h.
//
// Dark Souls III has the cap built in as a DEBUG switch, so no code patch is needed (v1 DS3, STATIC + the same method
// as the public DS3DebugFPS): the frame-timer object ("SprjFlipper", a pointer in a global) gets its period written
// every frame by 0x140EED810, but if the byte at +0x358 is set the period becomes 1 / float[+0x354] instead. Writing
// those two fields is the whole cap. The global is found by a unique byte pattern, never by address (DS3 has ASLR).
//
// KNOWN GAME BUG, not ours: above roughly 80 fps the game's own running / rolling speed drifts (the physics step is
// tied to the frame rate). Sekiro's speed-table fix has no counterpart in DS3's readable code (checked 2026-10-30:
// the pattern has zero hits), so the panel warns instead of pretending it is fixed.
#include "framerate.h"

#include "addresses.h"
#include "settings.h"

#include "core/vr/openxr_runtime.h"

#include "core/util/log.h"

#include <windows.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::ds3::framerate {
namespace {

uint8_t** g_flipperSlot = nullptr;   // the global that holds the frame-timer object
// 0 = match the headset (JJ 2026-10-30: "match the headset's frame rate by default"). The runtime's own refresh
// interval is only known once a session is running, so `tick` picks it up on the first frame that reports one and
// re-applies it if the headset's rate changes (Virtual Desktop can switch it mid-session).
constexpr int kMatchHeadset = 0;
int      g_cap = kMatchHeadset;
int      g_headsetHz = 0;
int      g_applied = 0;
char     g_status[192] = "frame rate: game default (60)";

uint8_t* find_text(const uint8_t* pat, const char* mask, size_t n, int& hits) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* first = nullptr;
    hits = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        uint8_t* b = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
        for (size_t k = 0; k + n <= sec->Misc.VirtualSize; ++k) {
            size_t j = 0;
            for (; j < n; ++j) if (mask[j] == 'x' && b[k + j] != pat[j]) break;
            if (j == n) { if (!hits) first = b + k; ++hits; }
        }
    }
    return first;
}

uint8_t* flipper() {
    if (!g_flipperSlot) return nullptr;
    __try { return *g_flipperSlot; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Write the two debug-cap fields. Cheap enough to repeat every frame, which also survives the game resetting them.
bool push(int fps) {
    uint8_t* f = flipper();
    if (!f) return false;
    __try {
        if (fps <= 0 || fps == 60) {
            f[addr::kFlipperFpsEnable] = 0;                                  // back to the game's own period
        } else {
            *reinterpret_cast<float*>(f + addr::kFlipperFpsOverride) = static_cast<float>(fps);
            f[addr::kFlipperFpsEnable] = 1;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// What the cap should actually be right now: the headset's refresh when that is what the player asked for, and
// the game's own 60 until the runtime has told us (a cap of 60 is always safe).
int wanted_fps() {
    if (g_cap > 0) return g_cap;
    return g_headsetHz > 0 ? g_headsetHz : 60;
}

void apply() {
    const int fps = wanted_fps();
    if (!push(fps)) {
        snprintf(g_status, sizeof(g_status), "frame rate: the frame timer is not built yet (wanted %d fps)", fps);
        return;
    }
    if (fps == g_applied) return;
    g_applied = fps;
    snprintf(g_status, sizeof(g_status), "frame rate: cap %d fps%s%s", fps,
             g_cap == kMatchHeadset ? " (matching the headset)" : "",
             fps > 80 ? " - above 80 the game's own running speed drifts, a DS3 bug" : "");
    BVR_LOG("%s", g_status);
}

} // namespace

void install() {
    g_cap = settings::i("fpscap", kMatchHeadset);
    // mov byte [rax+0x80], 1 / mov rax, [rip+disp]   - the only place in .text, and that global is the frame timer
    // (confirmed independently from the period writer's caller at 0x140ED8EDD).
    static const uint8_t pat[] = {0xC6, 0x80, 0x80, 0x00, 0x00, 0x00, 0x01, 0x48, 0x8B, 0x05, 0, 0, 0, 0};
    int hits = 0;
    uint8_t* at = find_text(pat, "xxxxxxxxxx????", sizeof(pat), hits);
    if (hits != 1) {
        snprintf(g_status, sizeof(g_status), "frame rate: frame-timer pattern hits=%d (want 1) - cap stays 60", hits);
        BVR_LOG("%s", g_status);
        return;
    }
    g_flipperSlot = reinterpret_cast<uint8_t**>(at + 14 + *reinterpret_cast<const int32_t*>(at + 10));
    BVR_LOG("frame rate: frame-timer global at %p (rva 0x%llX)", g_flipperSlot,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_flipperSlot) -
                                            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    apply();
}

// Called once per Present: the cap is two data writes, so it is simply re-asserted. While the player has asked
// for "match the headset", the runtime's refresh interval is read here - it only exists once a session runs.
void tick() {
    if (!g_flipperSlot) return;
    if (g_cap == kMatchHeadset) {
        const int64_t ns = bvr::vr::display_period_ns();
        if (ns > 1000000) {                                    // a sane period (under 1000 fps)
            const int hz = static_cast<int>((1000000000.0 / static_cast<double>(ns)) + 0.5);
            if (hz >= 30 && hz <= 240 && hz != g_headsetHz) {
                g_headsetHz = hz;
                g_applied = 0;                                 // make apply() log the change
                BVR_LOG("frame rate: the headset runs at %d Hz - matching it", hz);
                apply();
            }
        }
    }
    const int fps = wanted_fps();
    if (fps != 60) push(fps);
}

void set_cap(int fps) {
    if (fps != kMatchHeadset && (fps < 30 || fps > 240)) return;
    g_cap = fps;
    settings::set_i("fpscap", fps);
    g_applied = 0;
    apply();
}
int cap() { return g_cap; }
const char* status() { return g_status; }

void draw_ui() {
    static const int kCaps[] = {kMatchHeadset, 45, 60, 72, 90, 120};
    static const char* kNames[] = {"match the headset", "45", "60 (the game's own)", "72", "90", "120"};
    int cur = 0;
    for (int i = 0; i < 6; ++i) if (kCaps[i] == g_cap) cur = i;
    if (ImGui::Combo("frame rate limit (fps)", &cur, kNames, 6)) set_cap(kCaps[cur]);
    if (g_cap == kMatchHeadset && g_headsetHz > 0) ImGui::TextDisabled("   your headset is running at %d Hz", g_headsetHz);
    ImGui::TextDisabled("   pick half your headset's rate instead if the game cannot keep up");
    ImGui::TextDisabled("   above 80 the game's own running and rolling speed drifts - that is a Dark Souls III bug");
}

} // namespace bvr::ds3::framerate

#pragma once
// BLACKWATCH (Origins 2026-10-28, JJ: "blinks an entirely black frame in VR, sometimes a single one, sometimes several within a
// second ... main menu, pause menu, and during the game"; JJ cannot report live from the headset). Always on and cheap: every
// present's game picture (before DLSS) and every finished headset eye image (after DLSS / held-left finish / bat paint) is
// sampled at a 3x3 grid of small boxes, read back a few frames later without waiting, and a frame much darker than that
// stream's recent frames is logged with its present number. Dark in the game picture too = the game / our render; dark only
// in the finished image = DLSS or our copy. Frames that reached the headset with no picture at all are logged by NOBLANK.
#include <d3d11.h>
#include <cstddef>
#include <cstdint>

namespace bvr::vr::black_watch {
void set_on(bool on);
bool on();
// sign: -1 left, +1 right, 0 untagged (not judged). present: the runtime's present counter.
void on_game(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, int sign, uint64_t present);
void on_final(ID3D11DeviceContext* ctx, ID3D11Texture2D* image, int eye, uint64_t present);
// SHEETWATCH (JJ 2026-10-28: black frames still on the map and pause screens - there the menu / map is on the floating
// sheet, not in the eye picture). Every sheet sent to the headset, at a 5x5 grid: a sheet that differs from the one before
// for 1-6 frames and then comes back is a BLINK, logged with what happened to that pass's sheet (facts = gfx_hud::pass_facts).
// missing: the frame went out without its sheet (why = the reason) - also a blink.
void on_sheet(ID3D11DeviceContext* ctx, ID3D11Texture2D* sheet, uint64_t present, uint32_t facts);
void on_sheet_missing(uint64_t present, uint32_t facts, const char* why, bool kept);
void trace(int samples);   // log each stream's per-frame brightness for N samples (flicker hunting)
void status_line(char* out, size_t n);
} // namespace bvr::vr::black_watch

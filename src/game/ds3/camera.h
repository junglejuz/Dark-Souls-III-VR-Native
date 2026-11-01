#pragma once
// DS3VR2 camera: the head pose in the game camera (M2), and per-eye cameras for the engine's views (M3).
//
// The seam (v1, HEADSET; skvr/DS3_PLAN.md section 3): ChrCam's director (0x140734790) ends every update by copying
// the active camera into its own +0x10..+0x5F (rotation rows +0x10/+0x20/+0x30, position +0x40, vertical FOV +0x50 in
// radians referenced to 16:9, aspect +0x54 = culling only, near +0x58, far +0x5C). The `call` after the copy
// (0x140735355) is where the head goes in; the game's own values are put back at the director's next entry, so its
// own logic never reads our output (playbook A2.3). The renderer's RendCam is fed from ChrCam (RE_NOTES section 3).
#include <cstdint>

namespace bvr::ds3::camera {

struct Cam {                 // the engine's camera block, +0x10..+0x4F (row-major rows + position, w ignored)
    float r[4][4];           // r[0] right, r[1] up, r[2] forward (sign: see fwd_sign), r[3] position
    float fov = 0.0f;        // vertical, radians, 16:9-referenced (the value written to +0x50)
    bool  valid = false;
};

bool install();              // pattern scan + hooks; needs decrypted code (adapter init, after the first gate call)
bool installed();
const char* status();

// Composed cameras of the most recent game update (game thread writes, anyone reads; seqlock inside).
// eye -1 = mid-eye head (what ChrCam got), 0 = left, 1 = right. The generation is the runtime's locate generation the
// poses came from (PERF-007 / pose-timing contract: tag the pair with it).
bool latest(int eye, Cam& out, uint32_t* generation = nullptr);

// The game's own (pre-head) camera of the most recent update - the head-free reference (MARKERBASE, culling).
bool game_camera(Cam& out);

// Generation of the camera update `back` updates ago (0 = newest). Used to tag the presented pair (pose_delay).
bool generation_back(int back, uint32_t& gen);

void recenter();
void set_head(bool on);
bool head_on();
uint64_t updates();          // director updates that reached the seam

// PAUSELOOK (v1 run 91) with TRAVELSAFE (v1 run 160): put the head into the paused game's last camera from the Present
// thread. Writes only into a camera the game handed over after `notBeforeMs` that still carries its class tag.
// 1 written; 0 no camera; -1 handed over before notBeforeMs; -2 class tag changed (freed / reused).
int repose(unsigned long long notBeforeMs);

void draw_ui();              // panel rows (VIEW drawer)
bool handle_command(const char* cmd, const char* args);

} // namespace bvr::ds3::camera

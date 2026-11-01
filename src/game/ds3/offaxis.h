#pragma once
#include <cstdint>
// DS3VR2 OFFAXIS (lens mode 1, 2026-10-30, JJ: "do the final fix for asymmetrical field of view"): each eye drawn with
// its TRUE off-centre window - the runtime's four tangents - instead of a centred lens turned towards it (mode 2).
//
// Dark Souls III builds every view projection from a camera object (fov +0x50, near +0x54, far +0x58, aspect +0x64,
// tile count +0x70) in two places: 0x14004B070 (the view's own setup; with tile count >= 1 it takes the engine's
// off-centre builder 0x14004BB40(out, left, right, bottom, top, near, far) - its tiled-screenshot path) and 0x14004B3B0
// (the same projection for ~50 other passes a frame, through the centred builder 0x14004BA40(out, fov, aspect, near,
// far), plus the TAA jitter multiply). Both are hooked to note the camera; while an EYE camera is being set up, the
// builders get that eye's window x near instead, so the projection and everything the engine derives from it (view-
// projection, the inverses lighting reads, light tiles, projection-to-shadow) agree.
//
// The eyes are told apart through the camera copies the engine makes of each view's link: the right eye's (our view's)
// link fov is raised by one float step - invisible - and the hooks match that exact value (set_tags, from stereo).
namespace bvr::ds3::offaxis {

void install();
void set_on(bool on);                                  // lens mode 1 active (camera.cpp)
bool on();
void set_window(int eye, float l, float r, float b, float t);   // tangents (left/down negative), camera.cpp
void set_tags(float leftFov, float rightFov);          // the two links' fov values this frame (stereo.cpp)
int eye_of_cam(const void* cam);                       // 0 / 1 for an eye camera copy, -1 otherwise (LIGHTGRID)
void window(int eye, float out[4]);                    // that eye's window: left, right, bottom, top tangents
void set_eye_link(int eye, const float rec[16]);       // MIRRORFLIP: each eye's link as written (rows + position), stereo.cpp
void set_mirror_flip(int on);                          // `ds mirrorflip 0|1`
bool mirror_cam(const void* cam, int eye);             // a reflection camera of that eye (flip on), LIGHTGRID
void proj_probe();                                     // `ds projprobe`: projection builder callers / inputs, 1 s
void cam_probe();                                      // `ds camprobe`: camera objects of the two setups, 1 s
const char* status();
void union_cmd(const char* args);   // V13e CULLUNION
void union_only(uintptr_t ret, float k);
void union_two(uintptr_t a, uintptr_t b, float k);   // two callers widened at once (0 = none)
int callers_list(uintptr_t* out, int max);   // POPSWEEP2: distinct view / pass setup callers seen with an eye camera   // POPSWEEP: union window for exactly this view-setup caller (0 = off)
void callers_log();
void planes_cmd(const char* args);   // V13f CULLPLANES

} // namespace bvr::ds3::offaxis

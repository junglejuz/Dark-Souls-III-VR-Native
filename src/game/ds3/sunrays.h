#pragma once
// DS3VR2 SUNRAYS (2026-10-30, JJ: "fix the volumetric light" - the sun beams of the flat game are missing in VR).
// The god-ray pass sizes its radial blur from one number the game computes, DarkSoulsIII.exe 0x141751CB0: the picture
// size in pixels over the field of view (a focal length). At the VR picture shape it comes out so that the game runs one
// blur round instead of two, skips the boost pass (PS 2bacb17b, x15-22) and blurs ~3x shorter: no visible beams.
// This hooks that function and scales it: setting sun_rays -1 = automatic (16:9 over the eye's shape, default), or a
// fixed multiplier (1 = the game's own value). `ds sunfocal K` (0 = automatic) changes it live.
namespace bvr::ds3::sunrays {

void install();
void set_scale(float k);
float scale();
void proj_probe();
void cam_probe();    // `ds camprobe`: the camera objects of the two per-view projection setups, 1 s   // `ds projprobe`: log the projection builders' callers and inputs for 1 s

} // namespace bvr::ds3::sunrays

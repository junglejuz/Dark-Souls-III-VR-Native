#pragma once
// DS3VR2 LIGHTGRID (V4, 2026-10-30, JJ HEADSET: "different lighting appears in different blocks ... turning off then on"
// since the off-centre lens). Dark Souls III lights Forward+: each pixel reads the light list of its screen tile
// (g_LightLookupBuffer t60 / g_LightIndices t59, FC_LightTile_Width cb8[56]). The lists are built on the CPU by
// GXScreenLightOrganizer's grid builder 0x140234E40 (one caller, 0x1401A2353), which takes the window the tiles cover
// from the camera's fov +0x50 and aspect +0x64 alone - a CENTRED window. With OFFAXIS the picture uses the eye's
// off-centre window, so each tile got the lights of another patch of the view. For eye cameras this replaces the
// grid's window with the eye's own (lightgrid_stub.asm explains the seam).
namespace bvr::ds3::lightgrid {

void install();
void set_mode(int m);          // 0 off (the game's centred window), 1 the eye's window (default), 2 the same mirrored left/right
int mode();
const char* status();          // `ds lightgrid`: calls, windows replaced per eye, the last windows, patch still in place

} // namespace bvr::ds3::lightgrid

#pragma once
// DS3VR2 AFW - alternate frame warping, a choice beside native stereo (JJ 2026-10-30: "add AFW as a rendering alternative
// to native stereo"; DS3VR2_PLAN.md V7). Each game frame draws ONE eye (left, right, left ...) through the game's own view
// only - our second view is taken out of the list - and the other eye is built from that same frame's picture and depth,
// so both eyes always show the same moment (not plain alternate-eye rendering). Port of Batman Arkham City's AFW
// (batman_city/src/game/arkhamcity/afw.cpp, itself AKN's - JJ: "it works really well"); idea from PureDark's UEVR AFW.
//   mode 1 this frame:     the drawn eye moved into the other eye's view through its depth; holes from the background
//   mode 2 previous frame: the other eye's own last real picture, brought to now through this frame's depth
//   mode 3 combined:       1, its holes (behind objects, the other eye's outer edge) filled from 2   (default)
// DS3 specifics: the depth is reverse Z in a padded R32G8X24 target (its top-left render-size part is used); the
// destination's depth terms are measured from its own matrix; the undrawn eye's camera is the drawn one's moved by the
// eye separation with its own lens window (both eyes look the same way in the true off-centre lens).
#include <cstdint>
#include <d3d11.h>

namespace bvr::ds3::afw {

void load_settings();                  // afw= / afw_mode= (adapter start)
bool on();
void set_on(bool v);
int mode();
// Render thread, at the drawn eye's present: build the other eye's headset image. vpJ = the drawn eye's world -> clip
// with this frame's jitter (what the depth was rendered with), vpU without, vpD = the other eye's; depth ndc of the other
// eye = za + zb / distance; depth = the drawn eye's depth (render size rw x rh in its top-left).
bool warp(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* drawn, ID3D11Texture2D* otherDst, ID3D11Resource* depth,
          const double vpJ[16], const double vpU[16], const double vpD[16], float za, float zb, unsigned rw, unsigned rh);
void note_drawn(int eye);
void reset_history();
bool command(const char* args);        // "ds afw on|off|mode N|win N|dbg N|feather N|log N|status"
void status_line(char* out, unsigned n);

} // namespace bvr::ds3::afw

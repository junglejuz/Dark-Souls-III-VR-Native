#pragma once
// DS3VR2 MARKERS (M8): the HUD pieces that belong to a thing in the world - enemy health bars, alert triangles, grapple
// points, lock-on, "Touch Remnant" prompts - drawn on that thing, at its depth, in each eye, instead of flat on the
// room-fixed HUD panel.
//
// Method: `VR_WORLD_MARKERS_GUIDE.md` §5b (per-vertex perspective card by a generated geometry shader, AKN MKGS, JJ
// "perfect") + §5c (the card faces the head-free game camera's POSITION). Nothing is cut out or re-laid-out: the
// marker keeps the game's own draw, shaders, textures and animation, and only its GEOMETRY is replaced by an upright
// card standing at the world point.
//
// What this game gives us (v1, HEADSET-proven: skvr/DS3_PLAN.md runs 128-141):
//   - every world marker goes through `toStage(out, viewPos, flag)` at 0x140C69B30: viewPos is the point in CAMERA
//     space (x right, y up, z = distance in front), out is its 1920x1080 stage position. Callers seen: +0xC6A8AD /
//     +0x8B492B enemy bars, +0x8B3E2D still world points, +0xE36A7E / +0xE36C59 alert triangles, grapple via
//     0x140E36DF0. v1 also proved the placement uses the camera the world is drawn with (0.16 ms after our write).
//   - the HUD is Scaleform, drawn into its own sheet (rt+40+40+1F8). Its vertex shaders are the classic GFx form
//     (geo11 fix dumps, memory `geo11-fix-dumps-name-the-constants`):
//         cbuffer Constants : b0 { float4 cxadd; float4 cxmul; float4 mvp[2]; float4 texgen[2]; }   // 96 bytes
//         o.pos.x = dot(v, mvp[0]); o.pos.y = dot(v, mvp[1]); o.pos.zw = float2(0, 1);
//     w is FORCED to 1, so moving a piece through cb0 can never taper - §5a's measured reason why an affine move still
//     swings with the head. Hence the geometry shader.
//
// Measured, not guessed (the guide's rule): the camera-space -> stage projection is CALIBRATED each frame by asking the
// game's own function about three known points, and the stage -> HUD-NDC map is FITTED from the pieces the game itself
// lays out at those stage points. Both are logged.
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11VertexShader;
struct ID3D11GeometryShader;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11Device;
struct IDXGISwapChain;

namespace bvr::ds3::markers {

bool install();                  // queues the projection hook (stereo::install, before MH_ApplyQueued)
bool installed();

// Per frame, on the present thread: age the marker points out, build each eye's card maps, clear the layers.
void on_present(ID3D11Device* dev, ID3D11DeviceContext* imm, unsigned eyeW, unsigned eyeH);

// A draw into the HUD sheet (from picsplit's Draw / DrawIndexed hook). `cb0` is the 96 bytes last written to the
// vertex shader's b0, or null. Returns true when the piece was drawn into the eye layers and the game's own draw
// should be SKIPPED (it must not also appear flat on the HUD panel - guide §3.4).
bool hud_draw(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, const float* cb0, unsigned cbRows, bool indexed, unsigned count,
              unsigned startIndex, int baseVertex, unsigned instances);
// BATCH (V11): after hud_draw returned false for a batched draw with marker pieces in it - the copy of b0 the game's flat
// draw must use instead (those pieces collapsed); null otherwise. The caller binds it, draws, and puts the game's back.
ID3D11Buffer* take_flat_override();
// MKREGION (V12g): this HUD draw's flat side - draw it with this GS (+ its constants at GS b13) instead of the game's draw
bool take_flat_gs(::ID3D11GeometryShader** gs, ::ID3D11Buffer** cb);

// AFW (V9): this frame's world -> clip of each eye when they are not both drawn (the undrawn eye's = the warp's camera);
// eye -1 clears (native: each eye's own view's matrix, as captured)
void set_eye_vp(int eye, const float vp[16]);
ID3D11ShaderResourceView* layer(int eye);   // that eye's marker layer (premultiplied), or null
ID3D11Texture2D* layer_tex(int eye);
void probe_eye_image(::ID3D11DeviceContext* ctx, ::ID3D11Texture2D* dst, int eye);   // V12v EYEPROBE (mkframes)        // ... as a texture, for the capture
bool any_this_frame();                      // a piece was placed this frame (skip the composite otherwise)

void draw_ui();                             // panel rows (HUD drawer)
bool handle_command(const char* cmd, const char* args);
const char* status();

} // namespace bvr::ds3::markers

#pragma once
// DS3VR2 DLSS (M6) - one NVIDIA DLSS Super Resolution feature per eye (own history), the same jitter sequence index for
// both eyes, our own camera motion vectors from depth (CAMMV), auto exposure, reversed-Z depth. Ported from AKN
// (akvr_native/src/dlss.cpp, JJ: background "so much better"), whose method follows Luma's Arkham Knight mod (Luma
// Framework, Filippo Tarpini / Garamond, custom MIT: reuse with credit). VR_ESSENTIALS section 9; playbook 19 #dlss-in-vr,
// FAIL-PERF-017/018.
//
// STATUS 2026-10-30: WIRED (M6). The game renders at the eye picture x the mode's scale (eyesize.cpp), the jitter goes
// in through the geometry viewports (picsplit VPJITTER, same offset for both eyes), each eye's depth is copied with its
// picture (picsplit), the view-projection comes from cbSceneParam VC_MatrixViewProj, and the evaluation runs per eye on
// the split picture just before it is submitted (stereo.cpp eye_capture / eye_synth).
#include <d3d11.h>

namespace bvr::ds3::dlss {

void  start(ID3D11Device* dev);                     // begin NGX's own start in the background (first Present)
int   mode();                                       // settings dlss: 0 off, 1 DLAA, 2 ultra quality, 3 quality, 4 balanced, 5 performance
float scale();                                      // render size / headset size per side
void  out_size(UINT& w, UINT& h);                   // the headset eye size DLSS writes
void  jitter(unsigned long long frame, float& jpx, float& jpy);   // this frame's sub-pixel offset (render pixels), same for both eyes
void  note_mv(int eye, ID3D11Resource* r);          // optional: the game's own motion vectors for this eye
void  note_depth(int eye, ID3D11Resource* r);       // this eye's depth (reversed Z)
// The eye's finished colour (render size) -> the full-size eye, or null (not ready / off). VP: this view's own
// view-projection WITHOUT the jitter (row-vector, 16 floats; DS3VR2 takes it from cbSceneParam VC_MatrixViewProj) - the
// camera motion vectors are built from it and this eye's depth.
ID3D11Texture2D* eval(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* src, float jpx, float jpy, const float* VP,
                      bool vpOk);
void  set_out_size(UINT w, UINT h);                 // headset eye size (runtime recommended x resolution slider)
void  command(const char* what, int v);             // preset / depthinv / mvmode
const char* diag();
void  reset();
// DLSSCHECK: bits 0/1 = the eye(s) that broke (or 3 when asked), bit 2 = a capture is wanted; cleared by the call
int   take_capture_request();
ID3D11Resource* tex(int eye, int kind);             // 0 colour in, 1 our motion vectors, 2 depth, 3 colour out
int   evals(int eye);                               // evaluations so far (DLSS has really run on this eye)

} // namespace bvr::ds3::dlss

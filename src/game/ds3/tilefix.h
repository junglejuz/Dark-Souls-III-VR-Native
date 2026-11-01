#pragma once
// DS3VR2 TILEFIX (V6, 2026-10-30; JJ HEADSET: one candle still blocky after LIGHTGRID). Dark Souls III also sorts lights
// into screen tiles ON THE GPU: compute shaders 6a54371b9235b4a4 (32x32 groups) and cbc6e4a6512db67c (16x16), cbuffer
// cbLightAccParam, write g_wLightIndices / g_lightLookup from the depth picture and the point / spot / line lights. Each
// tile's side planes are built from the projection's scale terms only (P00 cb0[13].x, P11 cb0[14].y): a CENTRED lens.
// With OFFAXIS each eye's projection also carries a shift (cb0[13].z, cb0[14].z), so the tiles were offset.
//
// Fix: when the game creates either shader, a second copy is built with two instructions inserted right after the tile
// bias (`mad r3.zw, cb0[7].xxxy, r2.xxxx, -r3.xxxy` = tileScale - groupID):
//     mad r3.z, r2.y, cb0[13].z, r3.z      (+ tileScale.x * x shift)
//     mad r3.w, r2.z, -cb0[14].z, r3.w     (- tileScale.y * y shift)
// and the DXBC checksum recomputed (validated offline on all 5047 dumped shaders; the patched blobs pass
// CreateComputeShader, a one-byte change does not - re/dxbc_patch.py). The game's binds are swapped to the copy while the
// switch is on. Mode 2 = both signs flipped (in case the derivation's convention is the other way).
#include <cstddef>
#include <cstdint>
#include <d3d11.h>

namespace bvr::ds3::tilefix {

using CreateCSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
void on_created(ID3D11Device* d, uint64_t hash, const void* code, size_t len, ID3D11ComputeShader* orig, CreateCSFn create);
ID3D11ComputeShader* swap(ID3D11ComputeShader* sh);   // the shader to bind instead (sh itself when off / not ours)
void set_mode(int m);                                  // 0 the game's shader, 1 corrected (default), 2 corrected, signs flipped
int mode();
const char* status();

} // namespace bvr::ds3::tilefix

#pragma once
// DS3VR2 CONTACT (2026-10-30, JJ: "look into ray traced shadows ... software and/or hardware raytracing are options"):
// software ray-traced CONTACT shadows. Right after the game's own screen-space sun-shadow pass (10ada77c, Shadow Quality
// MAX) has written its mask, the same full-screen draw runs once more with our pixel shader: from every pixel a short ray
// is marched towards the sun through that eye's depth buffer, and where something in the picture blocks it the mask's
// "lit" alpha is lowered (MIN blend, alpha only - the packed depth in RGB is untouched). It adds the small shadows a
// shadow map is too coarse for (where objects touch the ground, folds, grass, fingers), in each eye from its own depth.
//
// Everything the ray needs is already bound for the game's draw: the depth (t1, point sampler s7), cbPerScene (b0:
// g_InvProjMatrix and the four view -> shadow cascade matrices, so the sun direction in VIEW space is the null direction
// of cascade 0's xy rows - no convention guessed) and cbSceneParam (b8: rows 5-7 the view matrix, for "which way is up").
// Screen <-> clip mapping comes from the pixel's own derivatives of the game's two interpolants (uv and clip).
//
// Hardware ray tracing (DXR) was weighed and not built: DS3VR2_PLAN "SHADOWRES / CONTACT".

struct ID3D11DeviceContext;

namespace bvr::ds3::contact {

constexpr unsigned long long kMaskPs = 0x10ada77c2e4b7d47ull;   // the MAX screen-shadow pass (writes the sun mask)

using DrawFn = void(__stdcall*)(ID3D11DeviceContext*, unsigned, unsigned);
using DrawIdxFn = void(__stdcall*)(ID3D11DeviceContext*, unsigned, unsigned, int);
// Called right after the game's mask draw on its (deferred) context, with the same draw arguments and the real draw
// functions (not the hooks). Does nothing while switched off.
void after_mask(ID3D11DeviceContext* c, bool indexed, unsigned count, unsigned start, int base, DrawFn draw, DrawIdxFn drawIdx);

void load();                      // settings: contact_shadows (0/1), contact_len (m), contact_strength (0..1)
void set(int on, float len, float strength);
int on();
float length();
float strength();
const char* status();

} // namespace bvr::ds3::contact

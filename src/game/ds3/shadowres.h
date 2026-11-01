#pragma once
// DS3VR2 SHADOWRES (2026-10-30, JJ: "an option for higher resolution shadows ... the most efficient way that affects
// performance the least"): the game's sun shadow maps are CREATED bigger than the game asks for, and every viewport and
// scissor the game sets while one of them is the depth target is scaled by the same factor (picsplit's context hooks).
//
// Why this works (STATIC, 2026-10-30, all 7071 dumped shaders): at Shadow Quality MAX the sun shadows are 16-bit depth
// maps (R16_TYPELESS, 4096x4096 / 4096x2048, depth + shader resource). Every reader samples them by NORMALISED texture
// coordinates through the cascade matrices (g_ShadowTexture t32 in 2076 shaders, g_DepthShadowTexture t3, the copy pass's
// g_DepthTexture t1, the ESM downsample t0) - none loads by texel position, none asks for the size. So a bigger map needs
// no change on the reading side; only the DRAWING side (viewport, scissor) has to cover the bigger map. The screen-shadow
// pass 10ada77c reads all four cascades from ONE map (an atlas) with a 4-tap filter whose offsets are
// FC_ShadowMapParam.zw (cbSceneParam row 28) - scaled by 1 / factor so the filter stays one texel wide (crisper edges).
//
// Applied at the D3D11CreateDevice gate (a vtable patch like SHADERID): the factor is fixed for the launch.

struct ID3D11Device;
struct ID3D11DepthStencilView;
struct ID3D11Resource;

namespace bvr::ds3::shadowres {

// Default 2 (SR1, 2026-10-30, fake headset: no measurable frame-rate change, ~+0.7-1 GB video memory, each sun cascade
// 4096x4096 instead of 2048x2048).
// DS3: 1.0 (off) until the bigger maps have been MEASURED on this game. Sekiro's default was 2.0, but its
// cascade atlas at MAX is 8192; Dark Souls III's largest map was already 8192 in the first run, and doubling
// that reaches D3D11's 16384 limit for no proven gain. Raised only after a measured A/B (M4L).
constexpr float kDefaultFactor = 1.0f;          // shadow_res when settings.ini has none

// The game's D3D11CreateDevice gate (game thread, before the game creates any texture). `ini` = settings.ini.
// Reads [ds3vr2] shadow_res (1 = the game's size, 1.5, 2 = twice as many texels each way) and shadow_res_ours.
void install(ID3D11Device* dev, const wchar_t* ini);
float factor();                                  // this launch's factor (1 = off)
bool active();                                   // at least one shadow map was made bigger
// Our second view's render-target set is built while this is held: its (empty, never read - SHADOWX lends the game's)
// shadow maps keep the game's size unless shadow_res_ours = 1.
void own_view_build(bool begin);
float dsv_scale(ID3D11DepthStencilView* dsv);    // the factor if this depth view is on an enlarged map, else 1
bool is_big(ID3D11Resource* r);                  // one of the enlarged maps
// Self-checks. viewport_seen: every viewport the GAME sets on an enlarged map (right / bottom edge, its own numbers); one
// reaching past the size the game asked for means the engine sizes viewports from the real map itself -> scaling off.
void viewport_seen(float right, float bottom);
bool engine_follows();
// The factor for FC_ShadowMapParam.zw (the filter offset): 1/factor when it is one texel of the game's map, 1 when the
// engine already made it one texel of the bigger map.
float filter_scale(float zw);
const char* status();                            // plain line for the panel / log
void report(bool force = false);                 // log what was enlarged (and what was kept) - once unless forced

} // namespace bvr::ds3::shadowres

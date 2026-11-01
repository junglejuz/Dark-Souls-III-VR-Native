#pragma once
// DS3VR2 PICSPLIT - each eye's finished picture out of the ONE shared final target both engine views write.
//
// Evidence (2026-10-29): EYEMAP of both views in one run shows rt+40.. textures (the finished picture, depth) are the SAME
// objects in both render-target sets; the view pictures read at Present were identical to the pixel and equal to the
// window (= the view drawn last). The engine draws on deferred contexts only and replays ~85 command lists per frame on
// the immediate context (frame dump 01:18:32), so the replay order is the GPU order.
// Method: hook the deferred contexts' render-target binds / UAV binds / FinishCommandList to mark command lists that write
// the shared target T; on the immediate context, after a marked list is replayed, copy T into a slot. With two views the
// marked lists split in two equal halves (main first, ours second): left = the copy after the main view's last write,
// right = T after the second view's last write. Counts are logged every 5 s so the split is checked, not assumed.
#include <cstdint>

struct IDXGISwapChain;
struct ID3D11Texture2D;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Device;

struct ID3D11VertexShader;

namespace bvr::ds3::picsplit {

// MARKERS (M8): called for every draw into the HUD sheet, with that draw's vertex shader and the 96 bytes last written
// to its b0 (Scaleform's cxadd / cxmul / mvp[2] / texgen[2]). Returning true means the piece has been drawn on its
// object in both eyes and the game's own draw is skipped.
using HudDrawFn = bool (*)(::ID3D11DeviceContext*, ::ID3D11VertexShader*, const float* cb0, unsigned cbRows, bool indexed, unsigned count,
                           unsigned startIndex, int baseVertex, unsigned instances);
void set_hud_sheet(::ID3D11Resource* sheet);
// HUDSHEET (DS3): the game's HUD draws are re-bound, as they are recorded, into a sheet of ours that goes to the
// room-fixed HUD panel. Made / returned on the Present thread.
::ID3D11Texture2D* hud_sheet_ensure(::ID3D11Device* dev);
void hud_redirect_set(int on);
void hud_world_keep(int on);   // V12o: 1 = on-object HUD pieces stay flat in the picture (old)
int hud_redirect_on();
void hud_sheet_stats(uint32_t& draws, uint32_t& byHash, uint32_t& bySig, uint32_t& lists);
void hud_sheet_stats2(uint32_t& world, uint32_t& imm, uint32_t& inst);   // kept on their object / immediate / instanced
void hud_present(::ID3D11DeviceContext* imm);   // once a frame on the Present thread
void hudtex(int frames);                        // `ds hudtex N`: list the pieces drawn into the shared picture
void ray_scale_set(float k);                     // `ds rayscale K`: stretch the god-ray radial blur (test)
void bbfix(int on);                            // `ds bbfix 0|1` (-1 = status): sprites stay upright (V6)
void rollscan(int ms);                         // `ds rollscan [ms]`: where uploaded constants hold an eye's camera axes (V5)
void occprobe(int frames);                      // `ds occprobe N`: the occlusion results the game reads
void occscale(float k);                         // `ds occscale K`: multiply them (test)
void popname_record(int phase, int frames);     // POPNAME: record every mesh each view draws (phase 0 symmetric, 1 off-centre)
bool popname_busy();
void readback(int frames);                      // READBACK: CPU reads of GPU resources (Map READ), by size + caller
void popname_copies(uint32_t out[2][2]);
void popname_meshes(uint32_t out[2][2]);        // [phase][view] distinct meshes drawn        // [phase][view] copies of instanced meshes drawn (POPSWEEP)
void popname_report();                          // the meshes the right view lost with the off-centre lens + their callers
void hudfade(const char* args);                 // HUDFADE: `ds hudfade <hex>|clear` - the game's fade pieces off the panel
unsigned long long fade_ms_since();            // HUDFADE: ms since a fade piece was drawn (taken)
uint32_t fe_draws();                           // FESIGNAL: full-screen Scaleform draws so far (moves = drawn this frame)
unsigned long long fe_ms_since();               // FESIGNAL: ms since a full-screen Scaleform screen was drawn into the picture
void smokerec(int frames);                      // SMOKEREC: census of every 3D draw (shader, shader, texture) for N frames
void smokescale(float k);                       // SMOKESCALE: smoke card size with SMOKEFACE 2 (1 = the game's)
void smokeface(int m);                          // SMOKEFACE: 0 game, 1 our copy (self-check), 2 smoke cards face the game camera
void smokelock(int m);                          // SMOKELOCK: 1 the smoke stays where the game camera looks, 0 off, -1 mirrored (test)
void smokefill(float k);                        // SMOKEFILL: bonfire smoke billboard enlarged K x about each eye's centre (0 = off)
void hazefill(float f);                         // HAZEFILL: 0 off, 1 auto, > 1 scale - the bonfire smoke over the whole eye
void hudhide(const char* args);                // `ds hudhide WxH` / `off`: hide HUD pieces with that texture size (test)
void texdump(const char* args);                // `ds texdump <hex> ...`: save those HUD textures as pictures when next drawn
void boxvis(int on);                           // `ds boxvis 0|1` (-1 = status): scenery query zero answers -> visible (test)
void boxprobe(int frames);                    // `ds boxprobe N`: the scenery visibility boxes + their query reads (log only)
void shaftprobe(int frames);                    // `ds shaftprobe N`: the light-shaft effect draws
void sunprobe(int frames);                      // `ds sunprobe`: the god-ray passes' constants, both views
void skip_ps(uint64_t hash);                    // `ds skipps <hex>`: drop that pixel shader's immediate draws (0 = off)
void postlog(int frames);                       // `ds postlog N`: the post chain in GPU order
void set_hud_draw(HudDrawFn fn);
void hud_stats(uint32_t& seen, uint32_t& noPlacement, uint32_t& taken);
void hud_probe(int frames);         // HUDPROBE: which render target the HUD draws really land on
uint32_t hud_armed();               // draws offered to the marker code at all (0 = the callback never ran)

bool install(IDXGISwapChain* sc);              // first Present: deferred-context hooks + the replay tap
void set_target(ID3D11Texture2D* t);           // the shared final picture (per frame, from the main view's RT set)
// Each view's OWN textures (per-view EYEMAP paths): a list writing the target while reading them belongs to that view.
void set_own(int view, ID3D11Resource* const* res, int n);
void on_present();                             // frame boundary (stats, swap the finished pair)
// The last finished pair: left (after the first half of T's writes) and right (after the last write). Null until a frame
// with an even, non-zero number of writes has been seen.
ID3D11Texture2D* left();
ID3D11Texture2D* right();
const char* status();
// Which of the frame's target writes each eye's picture was copied after, and how many writes the frame had.
void pair_index(int& left, int& right, int& writes);
// SPLITDUMP: record ONE whole frame - a copy of the target after every list that wrote it (`ds splitdump`).
void dump_arm();
int dump_ready();                 // number of copies once the frame is complete, else 0
ID3D11Texture2D* dump_tex(int i);
void dump_done();
void set_enabled(bool on);  // count the target's writers (3D on)
void set_hud_on(bool on);   // the HUD goes to its sheet (stereo gameplay: native pair or AFW)
void set_pair(bool on);     // both views listed: copy the left / right pictures
void cb_spy(bool on, uint32_t bytes = 0);
void shadowx_test(int m);
void upd_who(int n);
void vp_jitter(float x, float y);   // VPJITTER test: geometry-pass viewports shifted by x, y px
void jitter_set(float x, float y);  // the same shift, per frame and silent (DLSS)
void jitter_picture_size(unsigned w, unsigned h);   // only viewports of this size are jittered (0 = the old >= 1000 rule)
void jitter_size_from_target();     // the same, taken from the split target itself (what the eyes are copied from)
void jitter_stats(uint32_t& shifted, uint32_t& kept);
// ---- DLSS (M6): the inputs each eye's upscaler needs ----
ID3D11Texture2D* afw_grab(ID3D11DeviceContext* ctx, ID3D11Resource** depth);   // AFW: this frame's picture + a copy of its depth
void dlss_want(bool depth);                      // capture the per-eye depth (DLSS)
void want_view_proj(bool on);                    // capture each view's own view-projection (DLSS and the markers)
void set_depth(ID3D11Resource* shared);          // the depth target both views share (per frame, from the RT set)
ID3D11Texture2D* eye_depth(int eye);             // that eye's copy of the depth, taken with its picture
bool view_proj(int view, float out[16], float screen[4]);   // that view's VC_MatrixViewProj (row-vector) + FC_ScreenSize
void vp_pick(int on, float tol);                 // VPPICK: keep only matrices with the eye's own lens
void vp_check(int n);                            // log the next N captured matrices against a known point
const char* dlss_status();
void shadow_sharp(float f);   // SHADOWSHARP: shadow filter offset x f (1 = one texel of the map, smaller = crisper)
void shadowres_probe(int frames);   // SHADOWRES: log what was enlarged, the filter constants, N frames of shadow viewports
float shadow_sharp_now();
void scene_dump(int writes, unsigned bytes = 0);   // SHADOWX: log the next N writes of cbSceneParam (1136 B) whole
void shadow_diff(int frames); // SHADOWDIFF: sun-map binds, every shadow-constant write and the views carrying them, for N frames
void sun_trace(int frames);  // per frame: the sun direction in every write of the scene buffer
void cb_dump(int writes);   // log the next N whole writes of that buffer, row by row
void set_shadow(int view, ID3D11Resource* const* res, int n);   // that view's own sun shadow maps (cascades)
void set_eye_pos(int view, const float rec[16]);   // SHADOWX: each eye camera's link record (rows + position) this frame (0 game, 1 ours)
void shadow_lend(int mode);  // 0 off, 1 the sun map (t32), 2 also the scattering volume (t6) and light shadows (t7)  // L5: our view's lighting draws sample the MAIN view's sun shadow map   // log the next N whole writes of that buffer, row by row       // L5: log the shadow rows of the per-view constant buffer (Dark Souls III names from the geo11 fix)
void set_pair_indices(int left, int right, int writes);
// HDRSWAP (M3b probe): copy `src` over `dst` after each list that belongs to a view, i.e. just before the frame's
// post chain. nullptr turns it off. Tells whether DS3's single post run follows the HDR contents it reads.
void set_hdr_swap(::ID3D11Resource* src, ::ID3D11Resource* dst);
uint32_t hdr_swap_copies();
// ONEWAY: how often the frame's write structure changed under the eyes' capture points. A flickering eye shows up
// here as a count that keeps climbing; a steady scene should leave it still.
uint32_t pair_unstable();  // PAIRLEARN: copy each eye at these exact write numbers
// L7 (2026-10-29): the frame-to-frame buffers the two views share (passes named by the geo11 fix's shader hashes).
void hist_spy(int frames);       // log every watched pass in GPU order: view, history and output textures
void hist_swap(int mask);        // HISTSWAP: per-view history; bit 0 sun-shadow volume, 1 fog volume, 2 AO, 3 TAA
void vol_lend(int on);           // VOLLEND: our volumetric light pass reads the main view's sun map (compute t3)
void set_hist_own(int view, ID3D11Resource* const* res, int n);   // that view's own textures (identity of a pass's view)
void eye_stab(int frames);       // EYESTAB: per-frame picture change of each eye, logged when done
void hist_ab();                  // HISTAB: control / vollend / histswap steps with an EYESTAB each, judged from the log
void measure(ID3D11DeviceContext* imm);
void writers(int input, int view, int frames);   // WRITERS: shaders writing LITPROBE input #input of a view
void lit_probe(int frames, int only = -1, bool dump = false, bool own = false);
void stage_probe(int own, int k, int frames);
void inst_cap(::ID3D11Device* dev);   // INSTCAP: one frame of G-buffer draws: envData per draw and view, issuing engine function
void inst_spy(int frames);
void imm_spy(int frames);    // IMMSPY: instance-block writes on the immediate context (Map / UpdateSubresource)   // INSTSPY: envData (light-set index) of every instance block per view, and who writes it
void bind_spy(uint64_t hash);                       // BINDSPY: every texture a shader reads, per view
void in_probe(uint64_t hash, int slot, int frames);  // INPROBE: one input of a shader, per view, frame to frame   // STAGEPROBE: own texture after each view's k-th list writing it
void pass_probe(uint64_t hash, int frames, bool dump, int rt = 0);   // PASSPROBE: one shader's output per view, frame to frame
void light_src(int frames);   // LIGHTSRC: per frame and view, the light source of each ambient region pass + the eye brightness   // LITPROBE: per-frame change of each input of the lighting pass, game view vs ours   // after on_present each frame (EYESTAB, HISTAB)
void set_pair_mode(int m);  // 0 = copy after each view's last TAGGED write, 1 = sticky: after the last write of its run

} // namespace bvr::ds3::picsplit

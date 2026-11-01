// DS3VR2 PICSPLIT - see picsplit.h.
#include "picsplit.h"
#include "shaderid.h"
#include "shadowres.h"
#include "contact.h"
#include "camera.h"
#include "offaxis.h"
#include "tilefix.h"
#include "markers.h"
#include "settings.h"

#include "core/gfx/frame_inspector.h"
#include "core/gfx/blit.h"
#include "core/gfx/gfx_hud.h"
#include "core/util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <MinHook.h>

#include <atomic>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bvr::ds3::picsplit {
bool shadow_scene_fix(float* d);   // SHADOWX (defined after the file-local helpers)
bool shadow_head_fix(float* d);
namespace {

std::atomic<bool> g_on{false};
std::atomic<bool> g_pair{false};   // both views listed: copy the pair (off = only count, e.g. with the main view alone)
std::atomic<ID3D11Texture2D*> g_T{nullptr};

// ---- which render-target views point at T (lock-free cache: RTV/UAV pointer -> 1 yes / 2 no) ----
constexpr int kViewCache = 4096;
std::atomic<void*> g_viewKey[kViewCache];
std::atomic<uint8_t> g_viewVal[kViewCache];
std::atomic<ID3D11Texture2D*> g_cacheFor{nullptr};
bool view_is_T(ID3D11View* v) {
    if (!v) return false;
    ID3D11Texture2D* T = g_T.load(std::memory_order_relaxed);
    if (!T) return false;
    if (g_cacheFor.load(std::memory_order_relaxed) != T) { // T changed (resize / travel): forget the cache
        for (auto& k : g_viewKey) k.store(nullptr, std::memory_order_relaxed);
        g_cacheFor.store(T);
    }
    const uintptr_t h = (reinterpret_cast<uintptr_t>(v) >> 4) * 2654435761u;
    for (int probe = 0; probe < 8; ++probe) {
        const int i = static_cast<int>((h + probe) & (kViewCache - 1));
        void* k = g_viewKey[i].load(std::memory_order_acquire);
        if (k == v) return g_viewVal[i].load(std::memory_order_relaxed) == 1;
        if (!k) {
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            const bool yes = r == T;
            if (r) r->Release();
            void* expected = nullptr;
            if (g_viewKey[i].compare_exchange_strong(expected, v)) g_viewVal[i].store(yes ? 1 : 2, std::memory_order_release);
            return yes;
        }
    }
    ID3D11Resource* r = nullptr; // table full here: answer without caching
    v->GetResource(&r);
    const bool yes = r == T;
    if (r) r->Release();
    return yes;
}

// ---- per deferred context: "wrote T since the last FinishCommandList" ----
constexpr int kCtx = 128;
std::atomic<void*> g_ctxKey[kCtx];
std::atomic<uint8_t> g_ctxWrote[kCtx];
int ctx_slot(void* c) {
    const uintptr_t h = (reinterpret_cast<uintptr_t>(c) >> 4) * 2654435761u;
    for (int probe = 0; probe < kCtx; ++probe) {
        const int i = static_cast<int>((h + probe) & (kCtx - 1));
        void* k = g_ctxKey[i].load(std::memory_order_acquire);
        if (k == c) return i;
        if (!k) { void* e = nullptr; if (g_ctxKey[i].compare_exchange_strong(e, c) || e == c) return i; }
    }
    return -1;
}

// ---- marked command lists (ring) with their view tag ----
constexpr int kLists = 512;
std::atomic<void*> g_marked[kLists];
std::atomic<uint8_t> g_markTag[kLists];
std::atomic<uint32_t> g_markHead{0};
int is_marked(void* l) { // -1 not marked, else the tag (0 none, 1 main, 2 ours, 3 both)
    for (int i = 0; i < kLists; ++i)
        if (g_marked[i].load(std::memory_order_relaxed) == l) {
            g_marked[i].store(nullptr, std::memory_order_relaxed);
            return g_markTag[i].load(std::memory_order_relaxed);
        }
    return -1;
}

// ---- hooks on the deferred implementation ----
using OMSetRT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRTUAV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                            UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using CSSetUAV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using CopyRes = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopySub = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using Finish = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
OMSetRT oSetRT = nullptr; OMSetRTUAV oSetRTUAV = nullptr; CSSetUAV oCSUAV = nullptr; CopyRes oCopyRes = nullptr;
CopySub oCopySub = nullptr; Finish oFinish = nullptr;
std::atomic<uint32_t> g_rtCalls{0}, g_tBinds{0}, g_finishes{0}, g_marks{0};

// VIEWTAG (splitdump 01:31: writes 1-7 keep last frame's picture, write 8 puts in the main view's, write 14 ours; the
// rest change nothing): a list that writes T while reading one view's OWN textures (per-view EYEMAP set) belongs to that
// view. Tags: bit 0 = main, bit 1 = ours.
constexpr int kOwn = 8;
std::atomic<ID3D11Resource*> g_own[2][kOwn];
std::atomic<uint8_t> g_ctxTag[kCtx];
int owner_of(ID3D11Resource* r) {
    for (int v = 0; v < 2; ++v) for (int i = 0; i < kOwn; ++i) if (g_own[v][i].load(std::memory_order_relaxed) == r) return v;
    return -1;
}
// SRV -> owner bits cache (0x80 = known, low bits = owner bits); cleared when the per-view sets change.
constexpr int kSrvCache = 8192;
std::atomic<void*> g_srvKey[kSrvCache];
std::atomic<uint8_t> g_srvVal[kSrvCache];
std::atomic<uint32_t> g_ownGen{0};
uint32_t g_srvCacheGen = 0;
uint8_t srv_owner_bits(ID3D11ShaderResourceView* v) {
    if (g_srvCacheGen != g_ownGen.load(std::memory_order_relaxed)) {
        for (auto& k : g_srvKey) k.store(nullptr, std::memory_order_relaxed);
        g_srvCacheGen = g_ownGen.load(std::memory_order_relaxed);
    }
    const uintptr_t h = (reinterpret_cast<uintptr_t>(v) >> 4) * 2654435761u;
    for (int probe = 0; probe < 8; ++probe) {
        const int i = static_cast<int>((h + probe) & (kSrvCache - 1));
        void* k = g_srvKey[i].load(std::memory_order_acquire);
        if (k == v) return g_srvVal[i].load(std::memory_order_relaxed) & 3;
        if (!k) {
            ID3D11Resource* r = nullptr; v->GetResource(&r);
            const int o = r ? owner_of(r) : -1;
            if (r) r->Release();
            const uint8_t bits = o >= 0 ? static_cast<uint8_t>(1u << o) : 0;
            void* e = nullptr;
            if (g_srvKey[i].compare_exchange_strong(e, v)) g_srvVal[i].store(0x80 | bits, std::memory_order_release);
            return bits;
        }
    }
    return 0;
}
// SHADOWLEND (L5 2026-10-29): the sun cascade matrices live in ONE buffer written per PASS, not per view (3 writes a
// frame with one view and with two), so both eyes already share them - what differs is the shadow TEXTURE each view
// binds, and our view's is empty (L4b). So give our view's lighting draws the main view's shadow map, which is what
// geo11 does implicitly (shadows rendered once in light space, both eyes sample them). The lighting pass samples
// g_ShadowTexture at t32 (from the geo11 fix's shader dumps).
// SHADOWLEND, second attempt. The first keyed on "which view is this context recording?" (the accumulated owner bits
// of the textures it had read). Those bits are only cleared when the context stops writing the final target, so a
// lighting pass could carry a stale tag and the MAIN view's draw got our empty map: JJ saw the LEFT eye flicker
// between two lightings (2026-10-29 12:13). The draw is the wrong thing to ask about. Ask the TEXTURE instead: each
// view's own shadow maps are resolved from its own render-target set, so a bind of OUR map can be swapped for the
// main view's same-slot map and a bind of the main's can never be touched.
constexpr UINT kShadowSlot = 32;         // g_ShadowTexture (the geo11 fix's shader dumps)
constexpr int kShadowTex = 8;            // the cascade maps we resolve per view
std::atomic<int> g_shadowLend{0};        // 0 off, 1 = lend the sun maps
std::atomic<ID3D11Resource*> g_shadowRes[2][kShadowTex] = {};        // [0] main's, [1] ours
std::atomic<ID3D11ShaderResourceView*> g_mainShadowSrv[kShadowTex] = {}; // the game's own SRV for main's map i
std::atomic<uint32_t> g_shadowGen{0};
std::atomic<uint32_t> g_lendTook{0}, g_lendGave{0}, g_lendSkipped{0};

// SRV -> which shadow map it is: 0 = not one, 1..8 = main's map i-1, 9..16 = ours' map i-9.
constexpr int kShadowCache = 1024;
std::atomic<void*> g_shKey[kShadowCache];
std::atomic<uint8_t> g_shVal[kShadowCache];
uint32_t g_shCacheGen = 0xFFFFFFFFu;
std::atomic<uint32_t> g_shFull{0};   // SHADOWDIFF: lookups that found the table full (answered directly, uncached)
uint8_t shadow_code_direct(ID3D11ShaderResourceView* v) {
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    uint8_t code = 0;
    for (int w = 0; w < 2 && r && !code; ++w)
        for (int q = 0; q < kShadowTex; ++q)
            if (g_shadowRes[w][q].load(std::memory_order_relaxed) == r) { code = static_cast<uint8_t>(w * kShadowTex + q + 1); break; }
    if (r) r->Release();
    return code;
}
uint8_t shadow_code(ID3D11ShaderResourceView* v) {
    const uint32_t gen = g_shadowGen.load(std::memory_order_relaxed);
    if (g_shCacheGen != gen) { for (auto& k : g_shKey) k.store(nullptr, std::memory_order_relaxed); g_shCacheGen = gen; }
    const uintptr_t h = (reinterpret_cast<uintptr_t>(v) >> 4) * 2654435761u;
    for (int probe = 0; probe < 8; ++probe) {
        const int i = static_cast<int>((h + probe) & (kShadowCache - 1));
        void* k = g_shKey[i].load(std::memory_order_acquire);
        if (k == v) return g_shVal[i].load(std::memory_order_relaxed);
        if (!k) {
            ID3D11Resource* r = nullptr; v->GetResource(&r);
            uint8_t code = 0;
            for (int w = 0; w < 2 && r && !code; ++w)
                for (int q = 0; q < kShadowTex; ++q)
                    if (g_shadowRes[w][q].load(std::memory_order_relaxed) == r) { code = static_cast<uint8_t>(w * kShadowTex + q + 1); break; }
            if (r) r->Release();
            void* e = nullptr;
            if (g_shKey[i].compare_exchange_strong(e, v)) g_shVal[i].store(code, std::memory_order_release);
            return code;
        }
    }
    // SHADOWDIFF (2026-10-29): this used to return 0 = "not a shadow map" once the 8 probed entries were taken - a full
    // table silently switched SHADOWLEND off (the M4 runs logged 0 binds). Answer directly instead, and count it.
    g_shFull.fetch_add(1, std::memory_order_relaxed);
    return shadow_code_direct(v);
}

// ---- SHADOWDIFF (JJ 2026-10-29 after M4: "the shadows are now different in each eye ... a result of adjusting the
// cameras for the asymmetrical field of view"; "use a probe and analyze the difference between the shadow directions").
// For `ds shadowdiff N` frames: (A) where every sun map is bound - stage, slot, whose map - and by which view's list;
// (B) every write of the shadow constants: FC_ShadowDir and the four FC_ProjSpaceToShadowMatrix, which map the RENDERING
// view's projection space into the shadow map, plus the view whose command list carried the write; (C) which views'
// lists bind that buffer. If one write serves both eyes, the right eye looks its shadows up from the left eye's angle -
// harmless for two straight eyes 6 cm apart, wrong for turned eyes 14 deg apart (playbook 19 #shared-shadow-profile:
// the shared producer must cover both views; matrix equality alone is not enough for canted views).
std::atomic<int> g_sdFrames{0};
std::atomic<uint32_t> g_sdBind[2][64][2 * kShadowTex + 1];   // [PS, CS][slot][code]
std::atomic<uint32_t> g_sdBindView[2][2 * kShadowTex + 1][4];  // [PS, CS][code][list view bits] - filled at Finish
std::atomic<uint32_t> g_sdUntracked{0};
std::atomic<uint32_t> g_sdCtxWrite[kCtx];        // per context: the shadow-constant writes recorded in this list (+1 = index)
std::atomic<uint8_t> g_sdCtxBits[kCtx];   // per context: owner bits of every texture its list read (views 1 main, 2 ours)
std::atomic<uint32_t> g_sdCtxCode[kCtx];  // per context: bit (stage * 16 + code - 1) for each sun map bound
std::atomic<uint32_t> g_sdCtxCb[kCtx];    // per context: binds of the shadow-constant buffer
std::atomic<uint32_t> g_sdCbByView[4];
int g_sdWrite = 0;                          // shadow-constant writes this frame
std::atomic<void*> g_sdCbBuf[4];            // the buffer objects the shadow constants were written into
std::atomic<uint32_t> g_sdImm[2 * kShadowTex + 1];   // sun maps bound on the IMMEDIATE context (other code than the deferred one)
std::atomic<uint32_t> g_sdImmCalls{0};
void sd_srv(int s, int stage, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    if (s < 0) { g_sdUntracked.fetch_add(1, std::memory_order_relaxed); return; }
    for (UINT i = 0; i < n; ++i) {
        if (!v[i]) continue;
        g_sdCtxBits[s].fetch_or(srv_owner_bits(v[i]), std::memory_order_relaxed);
        const uint8_t code = shadow_code_direct(v[i]);
        if (!code) continue;
        if (start + i < 64) g_sdBind[stage][start + i][code].fetch_add(1, std::memory_order_relaxed);
        g_sdCtxCode[s].fetch_or(1u << (stage * 16 + code - 1), std::memory_order_relaxed);
    }
}
// ---- L7 (2026-10-29): the frame-to-frame buffers the two views share ----
// JJ: the left eye is right, but "flickering in and out with an incorrect image, which looks like a different lighting
// source". The geo11 fix's dumps name every pass that reads a previous frame (table below). If the two views share those
// textures, each view's temporal work builds on the OTHER eye's last frame. Worst suspect: the volumetric light's
// sun-shadow volume, built by a compute pass from the sun map at COMPUTE t3 - SHADOWLEND only swaps the pixel slot t32,
// so ours is built from our empty map, and the game's view can blend it in through the shared history.
// Per command list we track what is bound (shaders by their geo11 hash, a few SRV / UAV / RTV slots). These are the
// bindings THIS draw reads - reset at every FinishCommandList, never accumulated across lists (L5a's lesson).
constexpr int kTrackSrv = 32, kTrackUav = 4;
constexpr int16_t kSrcUav = 100, kSrcRtv0 = 200;   // a source: 0-15 an SRV slot, 100+u a UAV, 200 render target 0
struct HistPass { uint64_t hash; const char* name; bool cs; int bit; int nHist; int8_t hist[3]; int16_t src[3]; bool bySun; };
// hist = the slots holding the previous frame; src = where this frame's version is after the pass (next frame's history).
// bit = HISTSWAP bit (-1 = spy only). bySun = the pass reads that view's own sun map, so the map tells the view.
const HistPass kPasses[] = {
    {0xc5e347bab1de5afbull, "sun-shadow volume", true, 0, 1, {7}, {kSrcUav + 0}, true},           // g_PrevDirLightShadowTex t7 -> u0
    {0xeff1b4f2d6f72973ull, "fog volume", true, 1, 1, {1}, {kSrcUav + 0}, false},                  // g_PrevVolumteTexture t1 -> u0
    {0x0f22056782a29001ull, "AO temporal", false, 2, 1, {3}, {kSrcRtv0}, false},                   // g_PrevAOTex t3 -> RT0
    {0x57ba11be28134946ull, "AO temporal 2", false, 2, 1, {3}, {kSrcRtv0}, false},
    {0xb29e37013df1ed73ull, "TAA", false, 3, 3, {1, 4, 6}, {kSrcRtv0, 3, 5}, false},               // PrevFrameTex, g_PrevMoveTex, g_PrevStencilTex
    {0x0db837620f2133c1ull, "reflections", false, -1, 1, {11}, {-1}, false},                     // g_PrevFrameTexture t11
    {0x24be6e842c4467d8ull, "vol accumulate", true, -1, 1, {6}, {-1}, false},                    // reads the sun-shadow volume at t6
    {0x94371e143481d51aull, "lighting", false, -1, 0, {}, {}, true},                             // the sun lighting (t32)
    {0x38557003cd595b71ull, "ambient region", false, -1, 0, {}, {}, false},                      // irradiance volume + IBL per region
    {0xa1be3a6dd0edb88eull, "ambient region 2", false, -1, 0, {}, {}, false},
    {0xed06754102032497ull, "sun light", false, -1, 0, {}, {}, true},                            // cbLight b5: the light set (L7)
};
constexpr int kPassN = sizeof(kPasses) / sizeof(kPasses[0]);
// MAXSUN (2026-10-29): at Shadow Quality MAX the sun pass is shader 10ada77c (reads the per-view 4096x4096 shadow copy at
// t32) - counted as the "sun light" pass so the L7 probes (litprobe, lightsrc, eyestab) cover it.
constexpr uint64_t kSunPassMax = 0x10ada77c2e4b7d47ull;
int pass_of(uint64_t h) {
    if (h == kSunPassMax) h = 0xed06754102032497ull;
    if (h) for (int i = 0; i < kPassN; ++i) if (kPasses[i].hash == h) return i;
    return -1;
}

struct Bound {
    int8_t psPass = -1, csPass = -1;                  // the bound pixel / compute shader, as an index into kPasses
    ID3D11ShaderResourceView* psSrv[kTrackSrv] = {};
    ID3D11ShaderResourceView* csSrv[kTrackSrv] = {};
    ID3D11UnorderedAccessView* csUav[kTrackUav] = {};
    ID3D11RenderTargetView* rtv0 = nullptr;
    uint8_t psSun = 0, csSun = 0;                     // whose sun map is bound at PS t32 / CS t3 (1 main, 2 ours), before any lend
    ID3D11ShaderResourceView* psSrv32 = nullptr;      // PS t32 as bound by the game
    uint64_t psHash = 0, csHash = 0;                  // every bound shader's geo11 hash (WRITERS)
    ID3D11Buffer* psCb[16] = {};                      // pixel-stage constant buffers (LIGHTSRC)
    UINT psCbFirst[16] = {};                          // ... and the first constant of the bound window (PSSetConstantBuffers1)
    ID3D11RenderTargetView* rtvs[4] = {};
    ID3D11DepthStencilView* dsv = nullptr;            // SHADOWPASS: depth-only passes (the shadow-map copy at MAX)
    const void* ps = nullptr;                         // SHAFTPROBE: the bound pixel shader object
    ID3D11VertexShader* vs = nullptr;                 // MARKERS: the HUD piece's own vertex shader ...
    ID3D11Buffer* vsCb0 = nullptr;                    // ... the constant buffer it reads at b0 ...
    UINT vsCb0First = 0;                              // ... and where this piece's window starts in it (16-byte units)
};
Bound g_bound[kCtx];

// SHADOWPASS (2026-10-29, part of `ds shadowdiff`): at Shadow Quality MAX the sun maps are sampled at PS t1 by a pass of
// their own (a screen-space shadow mask?), not by the lighting at t32. Log, per frame: each draw that samples a sun map
// (shader, whose maps, the texture it writes), each draw that reads one of those written textures (shader, slot), and
// when those command lists are replayed - so it shows whether the two views share the written texture and which view's
// lighting reads which result, in GPU order.
template <class V> ID3D11Resource* res_of(V* v);
// LENDX (2026-10-29, MAX shadows): pass 30e33769 copies each view's sun map 0 into a 4096x4096 depth texture of its own
// ("X"); the MAX sun pass 10ada77c reads X at PS t32 and the volumetric passes read it at CS t0 / t3 - our view's X is
// built from its EMPTY maps, and one volumetric pass (24be6e84) writes a texture SHARED by both views. Each view's X is
// learned here (the depth target of that copy pass, by whose map it samples) and logged; the fix lends the path maps at
// the copy pass's INPUT (LENDANY), so our X is filled from the game view's maps and every reader stays consistent.
constexpr uint64_t kShadowCopyPs = 0x30e33769729f2fcfull;
std::atomic<uint32_t> g_frameNoFwd{0};
std::atomic<uint32_t> g_xSeen{0x80000000u};   // frame the MAX copy pass was last seen
// shadowlend 3 (SHADOWX) acts only while the game draws the MAX shadow path; otherwise it behaves like 1 (the t32 lend).
bool lend3() {
    return g_shadowLend.load(std::memory_order_relaxed) == 3 &&
           g_frameNoFwd.load(std::memory_order_relaxed) - g_xSeen.load(std::memory_order_relaxed) < 120;
}
std::atomic<ID3D11Resource*> g_xRes[2] = {};
std::atomic<ID3D11Resource*> g_xTex[2][4] = {};   // every X each view fills (the copy pass fills two a view)
// SHADOWX (shadowlend 3, 2026-10-30): the two views' X differ only by where each view centres its cascade boxes (world ->
// shadow linear parts equal to 1e-8, translations 30-47 texels apart, cascade_compare.py). Our view reads the GAME
// view's X (its static AND moving casters, light space = the same for any viewer) and our cascade matrices are the
// game's world -> shadow re-based into our view space: M' = M_game * V_game * V_ours^-1 (rows 18-33; V = rows 5-7).
std::atomic<ID3D11Resource*> g_xByMap[2][kShadowTex] = {};          // [view][source map] -> X
std::atomic<ID3D11ShaderResourceView*> g_xSrv[2][kShadowTex] = {};  // [PS, CS][source map] the game's own SRV of its X
std::atomic<uint32_t> g_xGave{0}, g_xMiss{0}, g_matFixed{0}, g_matStale{0}, g_matGame{0}, g_matUnsure{0};
int x_slot(ID3D11Resource* r, int& view) {   // which view's X and from which map, or -1
    if (r) for (int v = 0; v < 2; ++v) for (int k = 0; k < kShadowTex; ++k)
        if (g_xByMap[v][k].load(std::memory_order_relaxed) == r) { view = v; return k; }
    return -1;
}
std::mutex g_eyeMutex;
float g_eyePos[2][3] = {};
float g_eyeRows[2][3][3] = {};   // each eye camera's axes (link rows +0x00/+0x10/+0x20)
bool g_eyePosOk = false;
// Which eye a scene-buffer write belongs to, from its view matrix V (rows 5-7) and camera position: by position when the
// eyes stand apart, else by orientation (V x the eye's axes is a signed axis permutation for the right eye only).
// -1 = not an eye camera / cannot tell.
int eye_of(const double V[3][4], const double pos[3]) {
    float e[2][3], R[2][3][3];
    { std::lock_guard<std::mutex> lk(g_eyeMutex); if (!g_eyePosOk) return -1; memcpy(e, g_eyePos, sizeof(e)); memcpy(R, g_eyeRows, sizeof(R)); }
    double dd[2];
    for (int v = 0; v < 2; ++v) { dd[v] = 0; for (int c = 0; c < 3; ++c) dd[v] += (pos[c] - e[v][c]) * (pos[c] - e[v][c]); dd[v] = sqrt(dd[v]); }
    if ((dd[0] < dd[1] ? dd[0] : dd[1]) > 0.05) return -1;
    if (fabs(dd[0] - dd[1]) >= 0.01) return dd[0] < dd[1] ? 0 : 1;
    double score[2];
    for (int v = 0; v < 2; ++v) {
        score[v] = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const double m = fabs(V[i][0] * R[v][j][0] + V[i][1] * R[v][j][1] + V[i][2] * R[v][j][2]);
                score[v] += fabs(m - (m > 0.5 ? 1.0 : 0.0));
            }
    }
    if (fabs(score[0] - score[1]) < 0.02) return -1;   // eyes together AND parallel: equal anyway
    return score[0] < score[1] ? 0 : 1;
}
std::mutex g_matMutex;
// Re-base our view's four cascade matrices (3x4 view -> shadow, rows casRow..casRow+15, every 4th row 0 0 0 1) on the
// game view's: M' = (M_game * V_game) * V_ours^-1, V = rows 5-7 (world -> view). buf 0 = the 1216-byte volumetric buffer
// (rows 18-33), 1 = cbSceneParam (1136 B, rows 38-53, the MAX screen-shadow pass). Only writes whose camera stands at
// an eye (within 5 cm) count - cbSceneParam is also written for the shadow cameras. Returns true if `d` was changed.
double g_sGame[2][4][3][4] = {};
float g_wsGame[3][4] = {};
float g_oursOrig[12][4] = {}, g_oursNew[12][4] = {};   // our cbSceneParam cascades 1-3 (rows 42-53) before / after re-basing
bool g_oursOk = false;
// SHADOWSUB (2026-10-30): our view's cascade matrices live in several constant buffers of different layouts (cbSceneParam,
// the screen-shadow pass's 528-byte b0, the shared 256-byte b1/b2 rewritten per cascade, the volumetric 1216-byte buffer).
// Substitute by VALUE: any 3x4 block in any constant-buffer update equal to one of OUR matrices (4 world -> shadow, 4
// view -> shadow) becomes the game view's equivalent. The game's own writes never match ours.
struct SubPair { float from[12]; float to[12]; };
SubPair g_sub[8]; int g_subN = 0; uint32_t g_subFrame = 0;
std::mutex g_subMutex;
std::atomic<uint32_t> g_subHits[4] = {}, g_subScans{0};   // hits by buffer class: 0 cbSceneParam, 1 528 B, 2 256 B, 3 other
int sub_class(UINT bytes) { return bytes == 1136 ? 0 : bytes == 528 ? 1 : bytes == 256 ? 2 : 3; }
bool sub_scan(float* d, UINT bytes) {
    std::lock_guard<std::mutex> lk(g_subMutex);
    if (!g_subN) return false;
    g_subScans.fetch_add(1, std::memory_order_relaxed);
    bool hit = false;
    const int rows = static_cast<int>(bytes / 16);
    for (int r = 0; r + 3 <= rows; ++r) {
        const float* b = d + r * 4;
        if (b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f) continue;
        for (int k = 0; k < g_subN; ++k) {
            const float* f = g_sub[k].from;
            bool eq = true;
            for (int i = 0; i < 12 && eq; ++i) eq = fabsf(b[i] - f[i]) <= 1e-6f * (1.0f + fabsf(f[i]));
            if (!eq) continue;
            memcpy(d + r * 4, g_sub[k].to, sizeof(g_sub[k].to));
            hit = true;
            g_subHits[sub_class(bytes)].fetch_add(1, std::memory_order_relaxed);
            r += 2;
            break;
        }
    }
    return hit;
}
std::atomic<int> g_sxTest{0};   // `ds shadowxtest`: 1 = zero our cbSceneParam shadow rows, 2 = zero our volumetric cascades (does it reach?)   // cbSceneParam rows 31-33 VC_ShadowMapMatrix: cascade 0 WORLD -> shadow (view-independent: copied as is)
uint32_t g_sGameFrame[2] = {};
bool g_sGameOk[2] = {};
std::atomic<uint32_t> g_rbGame[2], g_rbFixed[2], g_rbStale[2], g_rbUnsure[2];
bool rebase_cascades(float* d, int casRow, int buf) {
    const float* sun = d + (buf == 0 ? 15 : 30) * 4;   // the sun row: 1216 B row 15, cbSceneParam row 30 (world)
    if (sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2] < 0.25f) return false;
    double V[3][4];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) V[r][c] = d[(5 + r) * 4 + c];
    double pos[3];
    for (int c = 0; c < 3; ++c) pos[c] = -(V[0][c] * V[0][3] + V[1][c] * V[1][3] + V[2][c] * V[2][3]);
    const int view = eye_of(V, pos);
    if (view < 0) { g_rbUnsure[buf].fetch_add(1, std::memory_order_relaxed); return false; }
    const uint32_t frame = g_frameNoFwd.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_matMutex);
    if (view == 0) {
        for (int k = 0; k < 4; ++k)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) {
                    const float* M = d + (casRow + 4 * k + r) * 4;
                    double acc = c == 3 ? M[3] : 0.0;
                    for (int i = 0; i < 3; ++i) acc += M[i] * V[i][c];
                    g_sGame[buf][k][r][c] = acc;
                }
        if (buf == 1) memcpy(g_wsGame, d + 31 * 4, sizeof(g_wsGame));
        g_sGameFrame[buf] = frame; g_sGameOk[buf] = true;
        g_rbGame[buf].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_sGameOk[buf] || frame - g_sGameFrame[buf] > 1) { g_rbStale[buf].fetch_add(1, std::memory_order_relaxed); return false; }
    const int skip = g_sxTest.load(std::memory_order_relaxed);
    if (buf == 0 && (skip & 4)) return false;
    double Vi[3][4];
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) Vi[r][c] = V[c][r]; Vi[r][3] = pos[r]; }
    if (buf == 1) memcpy(g_oursOrig, d + (casRow + 4) * 4, sizeof(g_oursOrig));
    SubPair pairs[8];
    if (buf == 1)
        for (int k = 0; k < 4; ++k) {
            const float* M = d + (casRow + 4 * k) * 4;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {   // S_o = M_o * V_o and S_g
                    double acc = c == 3 ? M[r * 4 + 3] : 0.0;
                    for (int i = 0; i < 3; ++i) acc += M[r * 4 + i] * V[i][c];
                    pairs[k].from[r * 4 + c] = static_cast<float>(acc);
                    pairs[k].to[r * 4 + c] = static_cast<float>(g_sGame[1][k][r][c]);
                    pairs[4 + k].from[r * 4 + c] = M[r * 4 + c];
                }
            }
        }
    float keep[16][4];
    if (buf == 1 && (skip & 8)) memcpy(keep, d + casRow * 4, sizeof(keep));
    for (int k = 0; k < 4; ++k)
        for (int r = 0; r < 3; ++r) {
            float* out = d + (casRow + 4 * k + r) * 4;
            for (int c = 0; c < 4; ++c) {
                double acc = c == 3 ? g_sGame[buf][k][r][3] : 0.0;
                for (int i = 0; i < 3; ++i) acc += g_sGame[buf][k][r][i] * Vi[i][c];
                out[c] = static_cast<float>(acc);
            }
        }
    if (buf == 1) {
        for (int k = 0; k < 4; ++k) memcpy(pairs[4 + k].to, d + (casRow + 4 * k) * 4, sizeof(pairs[4 + k].to));
        { std::lock_guard<std::mutex> sl(g_subMutex); memcpy(g_sub, pairs, sizeof(pairs)); g_subN = 8; g_subFrame = frame; }
        memcpy(g_oursNew, d + (casRow + 4) * 4, sizeof(g_oursNew)); g_oursOk = true;
        if (skip & 8) memcpy(d + casRow * 4, keep, sizeof(keep));
        if (!(skip & 32)) memcpy(d + 31 * 4, g_wsGame, sizeof(g_wsGame));
    }
    if ((skip & 3) == buf + 1) {
        for (int r = 0; r < 16; ++r) for (int c = 0; c < 4; ++c) d[(casRow + r) * 4 + c] = 0.0f;
        if (buf == 1) for (int r = 31; r < 34; ++r) for (int c = 0; c < 4; ++c) d[r * 4 + c] = 0.0f;
    }
    if (frame != g_sGameFrame[buf]) g_rbStale[buf].fetch_add(1, std::memory_order_relaxed);
    g_rbFixed[buf].fetch_add(1, std::memory_order_relaxed);
    return true;
}
// ---- DLSS (M6, 2026-10-30): each view's own view-projection and its own copy of the shared depth ----
// cbSceneParam rows 24-27 = VC_MatrixViewProj, packed the same way as FC_MatrixView (rows 5-7, proven by
// rebase_cascades): clip_i = sum_c M[i][c] * w_c. CAMMV wants the row-vector form, so it is transposed here. Only
// writes whose camera stands at an eye count (eye_of) - the buffer is written for the shadow cameras too.
std::atomic<bool> g_vpWant{false};
struct ViewVP { float m[16]; float screen[4]; uint32_t frame; bool ok; };
ViewVP g_viewVP[2] = {};
std::mutex g_vpMutex;
std::atomic<uint32_t> g_vpSeen[2] = {}, g_vpUnsure{0};
std::atomic<int> g_vpCheck{0};   // `ds vpcheck N`: prove the next N captured matrices against a known point
std::atomic<uint32_t> g_vpRejOrtho{0}, g_vpRejSing{0};
// VPPICK (ported from SKVR2 JITTER1, 2026-11-01 overnight): keep an eye's matrix only if its clip x / y rows have the
// length of that EYE's own lens (runtime window, offaxis::window) - "last write at the eye's position" let other cameras
// written there (the game's own centred camera, cull / union lenses, reflections) through, and the kept matrix flipped.
// The left eye is the game's own view, the one with extra cameras; DLSS reads this matrix for its motion vectors.
// `ds vppick 0|1 [tolerance]` (settings vp_pick). Counts and the lens spread kept per eye: `ds dlssinfo`.
std::atomic<int> g_vpPick{-1};
std::atomic<float> g_vpTol{0.02f};
std::atomic<uint32_t> g_vpRejLens[2] = {};
void scene_capture_vp(const float* d) {
    if (!g_vpWant.load(std::memory_order_relaxed)) return;
    double V[3][4];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) V[r][c] = d[(5 + r) * 4 + c];
    double pos[3];
    for (int c = 0; c < 3; ++c) pos[c] = -(V[0][c] * V[0][3] + V[1][c] * V[1][3] + V[2][c] * V[2][3]);
    const int view = eye_of(V, pos);
    if (view < 0) { g_vpUnsure.fetch_add(1, std::memory_order_relaxed); return; }
    // VC_MatrixViewProj (rows 24-27) is CAMERA-RELATIVE: measured on an eye write 2026-10-30 it is exactly
    // P * R_view (its 4th column is P's, not a translation), because the vertex shaders are fed world - camera.
    // The full world -> clip matrix therefore needs the camera subtraction folded in:
    //   clip_i = sum_c<3 M[i][c] * (w - cam)_c + M[i][3]   ->  Mfull[i][3] = M[i][3] - sum_c M[i][c] * cam[c].
    // Proof the fold is right: Mfull[3][3] came out as 191.49, the very number FC_MatrixView row 7 carries.
    // Sekiro folded in SC_RenderCameraPos (row 61); DS3's cbSceneParam has no such row - it stops at 960 bytes -
    // so the camera is SC_CameraPos (row 3). `ds vpcheck` proves the fold by pushing a known point through.
    const float* camRow = d + 3 * 4;
    double Mfull[4][4];
    for (int i = 0; i < 4; ++i) {
        double t = d[(24 + i) * 4 + 3];
        for (int c = 0; c < 3; ++c) { Mfull[i][c] = d[(24 + i) * 4 + c]; t -= Mfull[i][c] * camRow[c]; }
        Mfull[i][3] = t;
    }
    // Usable? Row 3 of a PERSPECTIVE matrix is the camera's forward direction (clip.w = distance along it), so its
    // length is about 1; an orthographic camera writes 0 0 0 1 there. And the matrix must actually invert - the
    // reprojection starts by unprojecting this eye's pixels.
    const double fl = sqrt(Mfull[3][0] * Mfull[3][0] + Mfull[3][1] * Mfull[3][1] + Mfull[3][2] * Mfull[3][2]);
    if (fl < 0.9 || fl > 1.1) { g_vpRejOrtho.fetch_add(1, std::memory_order_relaxed); return; }
    {   // determinant by the same elimination the inverse uses
        double a[4][4];
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) a[i][j] = Mfull[i][j];
        double det = 1;
        for (int c = 0; c < 4 && det != 0; ++c) {
            int pv = c;
            for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[pv][c])) pv = r;
            if (fabs(a[pv][c]) < 1e-20) { det = 0; break; }
            if (pv != c) { for (int j = 0; j < 4; ++j) { const double t = a[c][j]; a[c][j] = a[pv][j]; a[pv][j] = t; } det = -det; }
            det *= a[c][c];
            for (int r = c + 1; r < 4; ++r) { const double f = a[r][c] / a[c][c]; for (int j = c; j < 4; ++j) a[r][j] -= f * a[c][j]; }
        }
        if (det == 0) { g_vpRejSing.fetch_add(1, std::memory_order_relaxed); return; }
    }
    if (g_vpPick.load(std::memory_order_relaxed) < 0) g_vpPick.store(settings::i("vp_pick", 1));
    if (g_vpPick.load(std::memory_order_relaxed) && offaxis::on()) {   // VPPICK
        float w[4] = {};
        offaxis::window(view, w);
        if (w[1] - w[0] > 0.05f && w[3] - w[2] > 0.05f) {
            const double p00 = 2.0 / (w[1] - w[0]), p02 = (w[1] + w[0]) / (w[1] - w[0]);
            const double p11 = 2.0 / (w[3] - w[2]), p12 = (w[3] + w[2]) / (w[3] - w[2]);
            const double wantX = sqrt(p00 * p00 + p02 * p02), wantY = sqrt(p11 * p11 + p12 * p12);
            const double lx = sqrt(Mfull[0][0] * Mfull[0][0] + Mfull[0][1] * Mfull[0][1] + Mfull[0][2] * Mfull[0][2]);
            const double ly = sqrt(Mfull[1][0] * Mfull[1][0] + Mfull[1][1] * Mfull[1][1] + Mfull[1][2] * Mfull[1][2]);
            const double tol = g_vpTol.load(std::memory_order_relaxed);
            if (fabs(lx - wantX) > tol * wantX || fabs(ly - wantY) > tol * wantY) {
                if (g_vpRejLens[view].fetch_add(1, std::memory_order_relaxed) < 3)
                    BVR_LOG("vppick: eye %d - a matrix at the eye with lens x %.4f y %.4f left out (its own lens: %.4f / %.4f)",
                            view, lx, ly, wantX, wantY);
                return;
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_vpMutex);
    // transposed into the row-vector form our reprojection maths uses (clip_j = sum_i w_i * N[i][j])
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) g_viewVP[view].m[j * 4 + i] = static_cast<float>(Mfull[i][j]);
    for (int c = 0; c < 4; ++c) g_viewVP[view].screen[c] = d[1 * 4 + c];
    if (g_vpCheck.fetch_sub(1, std::memory_order_relaxed) > 0) {
        // Self-check, not a guess: a point 10 units straight ahead must land in the middle of the picture at
        // depth = near / 10 (reversed Z). fwd = FC_MatrixView row 2's world direction.
        const double fwd[3] = {V[2][0], V[2][1], V[2][2]};
        double w4[4] = {pos[0] + 10 * fwd[0], pos[1] + 10 * fwd[1], pos[2] + 10 * fwd[2], 1.0};
        double clip[4] = {};
        for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) clip[j] += w4[i] * g_viewVP[view].m[i * 4 + j];
        BVR_LOG("dlss vpcheck view %d: a point 10 units ahead lands at x %.4f y %.4f depth %.5f (w %.3f) - right is "
                "0.000 0.000 %.5f", view, clip[3] != 0 ? clip[0] / clip[3] : 0.0, clip[3] != 0 ? clip[1] / clip[3] : 0.0,
                clip[3] != 0 ? clip[2] / clip[3] : 0.0, clip[3], 0.0800006 / 10.0);
    }
    g_viewVP[view].frame = g_frameNoFwd.load(std::memory_order_relaxed);
    g_viewVP[view].ok = true;
    g_vpSeen[view].fetch_add(1, std::memory_order_relaxed);
}
// The depth target is SHARED by the two views (EYEMAP), so each eye's copy is taken at the same moment as its picture
// (after that view's last write of the final target, before the next view's geometry runs).
ID3D11Resource* g_depthSrc = nullptr;
ID3D11Texture2D* g_depthCopy[2] = {};
std::atomic<uint32_t> g_depthCopies[2] = {};
std::atomic<bool> g_depthWant{false};
DXGI_FORMAT depth_typeless(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        default: return f;
    }
}
bool ensure_depth(ID3D11DeviceContext* ctx, int i) {
    if (!g_depthSrc) return false;
    ID3D11Texture2D* src = nullptr;
    if (FAILED(g_depthSrc->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src))) || !src) return false;
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d); src->Release();
    const DXGI_FORMAT want = depth_typeless(d.Format);
    if (g_depthCopy[i]) {
        D3D11_TEXTURE2D_DESC c{}; g_depthCopy[i]->GetDesc(&c);
        if (c.Width == d.Width && c.Height == d.Height && c.Format == want) return true;
        g_depthCopy[i]->Release(); g_depthCopy[i] = nullptr;
    }
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return false;
    d.Format = want; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0; d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_depthCopy[i]));
    if (!ok) BVR_LOG("dlss: depth copy %ux%u format %d FAILED", d.Width, d.Height, (int)want);
    dev->Release();
    return ok;
}
void copy_eye_depth(ID3D11DeviceContext* ctx, int i) {
    if (!g_depthWant.load(std::memory_order_relaxed) || !g_depthSrc) return;
    if (ensure_depth(ctx, i)) { ctx->CopyResource(g_depthCopy[i], g_depthSrc); g_depthCopies[i].fetch_add(1, std::memory_order_relaxed); }
}

void shadow_mat_fix(float* d) { rebase_cascades(d, 18, 0); }

// a bind of OUR X at any slot becomes the game's X from the same source map; the game's own binds teach its SRVs
bool x_swap(int stage, UINT n, ID3D11ShaderResourceView* const* v, ID3D11ShaderResourceView** copy) {
    bool swapped = false;
    for (UINT i = 0; i < n; ++i) {
        copy[i] = v[i];
        if (!v[i]) continue;
        int view = -1;
        const int k = x_slot(res_of(v[i]), view);
        if (k < 0) continue;
        if (view == 0) { g_xSrv[stage][k].store(v[i], std::memory_order_relaxed); continue; }
        ID3D11ShaderResourceView* m = g_xSrv[stage][k].load(std::memory_order_relaxed);
        if (!m) m = g_xSrv[stage ^ 1][k].load(std::memory_order_relaxed);
        if (m && m != v[i]) { copy[i] = m; swapped = true; g_xGave.fetch_add(1, std::memory_order_relaxed); }
        else g_xMiss.fetch_add(1, std::memory_order_relaxed);
    }
    return swapped;
}
int x_owner(ID3D11Resource* r) {
    if (r) for (int v = 0; v < 2; ++v) for (auto& t : g_xTex[v]) if (t.load(std::memory_order_relaxed) == r) return v;
    return -1;
}
std::atomic<uint32_t> g_xLearned{0};
void learn_x(int s) {
    const Bound& b = g_bound[s];
    if (b.psHash != kShadowCopyPs || !b.dsv || !b.psSrv[1]) return;
    const uint8_t code = shadow_code_direct(b.psSrv[1]);
    if (!code) return;
    const int v = code <= kShadowTex ? 0 : 1;
    g_xSeen.store(g_frameNoFwd.load(std::memory_order_relaxed), std::memory_order_relaxed);
    ID3D11Resource* x = res_of(b.dsv);
    if (x && x_owner(x) < 0)
        for (auto& t : g_xTex[v]) { ID3D11Resource* e = nullptr; if (t.compare_exchange_strong(e, x)) break; }
    if (x) g_xByMap[v][(code - 1) % kShadowTex].store(x, std::memory_order_relaxed);
    if (x && g_xRes[v].exchange(x) != x) {
        g_xLearned.fetch_add(1);
        BVR_LOG("lendx: %s view's MAX shadow texture is %p (from map %d)", v ? "OUR" : "the GAME", x, (code - 1) % kShadowTex);
    }
}
struct SdMask { ID3D11Resource* rt; uint8_t code; uint64_t hash; int level; };   // level 0 = filled from a sun map, 1 = from a level-0 texture
SdMask g_sdMasks[16]; int g_sdMaskN = 0;
struct SdKey { int s; uint64_t hash; int slot; ID3D11Resource* r; };
SdKey g_sdKeys[256]; int g_sdKeyN = 0;
std::mutex g_sdMutex;
std::atomic<uint8_t> g_sdCtxEv[kCtx];
struct SdList { std::atomic<void*> list; int s; uint8_t bits; };
SdList g_sdLists[64]; std::atomic<uint32_t> g_sdListHead{0};
template <class V> ID3D11Resource* res_of(V* v);
bool sd_first(int s, uint64_t hash, int slot, ID3D11Resource* r) {   // once per frame per (context, shader, slot, texture)
    for (int i = 0; i < g_sdKeyN; ++i)
        if (g_sdKeys[i].s == s && g_sdKeys[i].hash == hash && g_sdKeys[i].slot == slot && g_sdKeys[i].r == r) return false;
    if (g_sdKeyN < 256) g_sdKeys[g_sdKeyN++] = {s, hash, slot, r};
    return g_sdKeyN < 256;
}
const char* sd_shape(ID3D11Resource* r, char* buf, size_t n) {
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
        sprintf_s(buf, n, "%ux%u fmt %d", d.Width, d.Height, static_cast<int>(d.Format));
    } else sprintf_s(buf, n, "-");
    return buf;
}
void sd_draw(int s, bool cs) {
    const Bound& b = g_bound[s];
    std::lock_guard<std::mutex> lk(g_sdMutex);
    const bool xReader = !cs && (b.psHash == 0x10ada77c2e4b7d47ull || b.psHash == 0x15ec1c8cd4c1ea00ull ||
                                 (b.psHash >> 32) == 0x15ec1c8cull || (b.psHash >> 32) == 0x2cdd5a19ull || (b.psHash >> 32) == 0x2fed0576ull || b.psHash == 0x926a00b2a26ef35aull);
    if (xReader && sd_first(s, b.psHash, 400, nullptr)) {   // every input of the MAX passes that read X
        char line[1400]; int k = sprintf_s(line, "shadowpass ctx %d: %016llx inputs:", s, static_cast<unsigned long long>(b.psHash));
        for (int q = 0; q < 16 && k < 600; ++q) {
            if (!b.psCb[q]) continue;
            D3D11_BUFFER_DESC bd{}; b.psCb[q]->GetDesc(&bd);
            k += sprintf_s(line + k, sizeof(line) - k, " b%d=%p(%uB u%d first %u)", q, b.psCb[q], bd.ByteWidth, (int)bd.Usage, b.psCbFirst[q]);
        }
        for (int i = 0; i < 33 && k < 1360; ++i) {
            ID3D11Resource* r = res_of(i < 32 ? b.psSrv[i] : b.psSrv32);
            if (!r) continue;
            char sh[48];
            k += sprintf_s(line + k, sizeof(line) - k, " t%d=%p(%s)", i, r, sd_shape(r, sh, sizeof(sh)));
        }
        BVR_LOG("%s", line);
    }
    ID3D11Resource* rt = cs ? res_of(b.csUav[0]) : res_of(b.rtv0);
    if (!rt && !cs) rt = res_of(b.dsv);
    ID3D11ShaderResourceView* const* srv = cs ? b.csSrv : b.psSrv;
    const uint64_t hash = cs ? b.csHash : b.psHash;
    char sh[48], sh2[48];
    for (int i = 0; i < 16; ++i) {
        if (!srv[i]) continue;
        const uint8_t code = shadow_code_direct(srv[i]);
        if (!code) continue;
        bool known = false;
        for (int m = 0; m < g_sdMaskN; ++m) if (g_sdMasks[m].rt == rt && g_sdMasks[m].code == code) known = true;
        if (!known && rt && g_sdMaskN < 16) g_sdMasks[g_sdMaskN++] = {rt, code, hash, 0};
        g_sdCtxEv[s].store(1, std::memory_order_relaxed);
        if (sd_first(s, hash, i, rt))
            BVR_LOG("shadowpass ctx %d: %s shader %016llx samples %s map %d at t%d -> writes %p (%s)%s", s, cs ? "CS" : "PS",
                    static_cast<unsigned long long>(hash), code <= kShadowTex ? "the GAME view's" : "OUR view's",
                    (code - 1) % kShadowTex, i, rt, sd_shape(rt, sh, sizeof(sh)), !cs && !b.rtv0 && b.dsv ? " = its DEPTH target" : "");
        break;
    }
    for (int i = 0; i < 33; ++i) {
        ID3D11Resource* r = res_of(i < 32 ? srv[i] : (cs ? nullptr : b.psSrv32));
        if (!r) continue;
        for (int m = 0; m < g_sdMaskN; ++m) {
            if (g_sdMasks[m].rt != r || r == rt) continue;
            g_sdCtxEv[s].store(1, std::memory_order_relaxed);
            if (g_sdMasks[m].level == 0 && rt) {   // follow one step further: who reads what this pass writes
                bool have = false;
                for (int k = 0; k < g_sdMaskN; ++k) if (g_sdMasks[k].rt == rt) have = true;
                if (!have && g_sdMaskN < 16) g_sdMasks[g_sdMaskN++] = {rt, g_sdMasks[m].code, hash, 1};
            }
            if (!cs && g_sdMasks[m].level == 0 && sd_first(s, hash, 300, nullptr)) {   // the constant buffers of a draw reading X
                char cbs[400]; int k = 0;
                for (int q = 0; q < 16 && k < 360; ++q) {
                    if (!b.psCb[q]) continue;
                    D3D11_BUFFER_DESC bd{}; b.psCb[q]->GetDesc(&bd);
                    k += sprintf_s(cbs + k, sizeof(cbs) - k, " b%d=%p %uB u%d c%X first %u;", q, b.psCb[q], bd.ByteWidth, (int)bd.Usage,
                                   bd.CPUAccessFlags, b.psCbFirst[q]);
                }
                BVR_LOG("shadowpass ctx %d: PS shader %016llx constant buffers:%s", s, static_cast<unsigned long long>(hash), k ? cbs : " none");
            }
            if (sd_first(s, hash, 100 + i, r))
                BVR_LOG("shadowpass ctx %d: %s shader %016llx READS %p (level %d, from %s maps by %016llx) at t%d -> writes %p (%s)", s,
                        cs ? "CS" : "PS", static_cast<unsigned long long>(hash), r, g_sdMasks[m].level, g_sdMasks[m].code <= kShadowTex ? "the GAME view's" : "OUR view's",
                        static_cast<unsigned long long>(g_sdMasks[m].hash), i, rt, sd_shape(rt, sh2, sizeof(sh2)));
            break;
        }
    }
}

std::atomic<int> g_histSpy{0};         // frames still to log
std::atomic<int> g_histSwap{0};        // HISTSWAP bits
std::atomic<int> g_volLend{0};         // VOLLEND
std::atomic<uint32_t> g_frameNo{0};
std::atomic<uint32_t> g_hsRestore{0}, g_hsSave{0}, g_hsFirst{0}, g_hsTwice{0}, g_hsUnknown{0}, g_hsShape{0}, g_volGave{0};
std::atomic<uint32_t> g_passBound[kPassN], g_passRan[kPassN];
std::atomic<ID3D11ShaderResourceView*> g_mainShadowSrvCS[kShadowTex] = {};   // the game's compute-stage SRV for main's map i

// That view's own textures (resolved per view; a path whose texture both views share is left out).
constexpr int kHistOwn = 12;
std::atomic<ID3D11Resource*> g_histOwn[2][kHistOwn];
int hist_owner(ID3D11Resource* r) {
    if (r) for (int v = 0; v < 2; ++v) for (int i = 0; i < kHistOwn; ++i) if (g_histOwn[v][i].load(std::memory_order_relaxed) == r) return v;
    return -1;
}
template <class V> ID3D11Resource* res_of(V* v) { ID3D11Resource* r = nullptr; if (v) { v->GetResource(&r); if (r) r->Release(); } return r; }

// HISTSPY events: recorded per context while the list is built, then printed in REPLAY order (= GPU order).
struct HistEv { int8_t pass, view, via; ID3D11Resource* hist[3]; ID3D11Resource* src[3]; };
constexpr int kEvPerList = 24;
struct CtxEv { int n; HistEv ev[kEvPerList]; };
CtxEv g_ctxEv[kCtx];
struct ListEv { std::atomic<void*> list; int n; HistEv ev[kEvPerList]; };
constexpr int kListEv = 128;
ListEv g_listEv[kListEv];
std::atomic<uint32_t> g_listEvHead{0};
int g_execN = 0;                       // lists replayed this frame (immediate context)
int g_execN_now() { return g_execN; }

// The view a watched pass belongs to: first by that view's OWN textures among what the pass binds, then - for passes that
// read the sun map - by whose sun map it reads. -1 = cannot tell; HISTSWAP then leaves the pass alone.
// via: 1 own texture, 2 sun map, 3 both views' own textures bound (contradiction), 0 nothing to go by.
int view_of(const Bound& b, const HistPass& p, int8_t& via) {
    unsigned bits = 0;
    auto add = [&](ID3D11Resource* r) { const int o = hist_owner(r); if (o >= 0) bits |= 1u << o; };
    if (p.cs) { for (auto* v : b.csSrv) add(res_of(v)); for (auto* u : b.csUav) add(res_of(u)); }
    else { for (auto* v : b.psSrv) add(res_of(v)); add(res_of(b.rtv0)); }
    if (bits == 1 || bits == 2) { via = 1; return static_cast<int>(bits) - 1; }
    const uint8_t sun = p.cs ? b.csSun : b.psSun;
    if (p.bySun && sun) { via = 2; return sun - 1; }
    // MAXSUN: at Shadow Quality MAX the volume passes read the view's own shadow copy X (at t3), not a sun map
    unsigned xb = 0;
    if (p.cs) { for (auto* v : b.csSrv) { const int o = x_owner(res_of(v)); if (o >= 0) xb |= 1u << o; } }
    else { for (auto* v : b.psSrv) { const int o = x_owner(res_of(v)); if (o >= 0) xb |= 1u << o; } }
    if (xb == 1 || xb == 2) { via = 4; return static_cast<int>(xb) - 1; }
    via = bits == 3 ? 3 : 0;
    return -1;
}

ID3D11Resource* slot_res(const Bound& b, const HistPass& p, int16_t code) {
    if (code < 0) return nullptr;
    if (code == kSrcRtv0) return res_of(b.rtv0);
    if (code >= kSrcUav) return code - kSrcUav < kTrackUav ? res_of(b.csUav[code - kSrcUav]) : nullptr;
    return code < kTrackSrv ? res_of(p.cs ? b.csSrv[code] : b.psSrv[code]) : nullptr;
}

struct Shape { int dim = 0; UINT w = 0, h = 0, d = 0, mips = 0, fmt = 0; };
Shape shape_of(ID3D11Resource* r) {
    Shape s;
    if (!r) return s;
    D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
            D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
            s = {2, d.Width, d.Height, d.ArraySize, d.MipLevels, static_cast<UINT>(d.Format)};
        }
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        ID3D11Texture3D* t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture3D), reinterpret_cast<void**>(&t))) && t) {
            D3D11_TEXTURE3D_DESC d{}; t->GetDesc(&d); t->Release();
            s = {3, d.Width, d.Height, d.Depth, d.MipLevels, static_cast<UINT>(d.Format)};
        }
    }
    return s;
}
bool same(const Shape& a, const Shape& b) {
    return a.dim && a.dim == b.dim && a.w == b.w && a.h == b.h && a.d == b.d && a.mips == b.mips && a.fmt == b.fmt;
}
// A plain GPU copy target of the same shape (no binds: it is only ever copied to and from).
ID3D11Resource* create_like(ID3D11DeviceContext* c, ID3D11Resource* r) {
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11Resource* out = nullptr;
    D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
            D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
            d.BindFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT; d.MiscFlags = 0;
            ID3D11Texture2D* n = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &n))) out = n;
        }
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        ID3D11Texture3D* t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture3D), reinterpret_cast<void**>(&t))) && t) {
            D3D11_TEXTURE3D_DESC d{}; t->GetDesc(&d); t->Release();
            d.BindFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT; d.MiscFlags = 0;
            ID3D11Texture3D* n = nullptr;
            if (SUCCEEDED(dev->CreateTexture3D(&d, nullptr, &n))) out = n;
        }
    }
    dev->Release();
    return out;
}

// HISTSWAP: each view's own copy of a pass's history. The copies are recorded INTO the game's command list at the pass,
// so on the GPU they run exactly there: before it, the view's saved history goes into the history texture; after it,
// the pass's output (next frame's history) is saved for that view. Same for ping-pong and copy-type histories.
struct Save { ID3D11Resource* tex = nullptr; uint32_t frame = 0; bool valid = false; };
Save g_save[2][kPassN][3];

// LITPROBE (L7): the game's eye flickers only while our view exists, and none of our own switches changes that, so
// something our view leaves behind is read by the game's view. Copy every input of the final lighting pass, per view, at
// the pass itself (inside the game's list = the exact GPU moment); measure() then compares each input frame to frame.
// The input that changes for the game's view but not for ours is where the flicker enters.
constexpr int kProbeIn = 7;
constexpr UINT kProbeSlot[kProbeIn] = {0, 1, 3, 5, 6, 7, 32};
const char* const kProbeName[kProbeIn] = {"GBuffer0 (t0)", "depth (t1)", "GBuffer1 (t3)", "GBuffer3 (t5)",
                                          "irradiance (t6)", "specular acc (t7)", "shadow mask (t32)"};
std::atomic<int> g_probeWant{0};
std::atomic<int> g_probeOnly{-1};
std::atomic<bool> g_probeOwn{false};
std::atomic<uint64_t> g_ppHash{0};    // PASSPROBE: the shader whose output is measured
std::atomic<uint32_t> g_ppUnknown{0};
std::atomic<int> g_ppRt{0};             // PASSPROBE: which render target / UAV of the pass
std::atomic<int> g_stageK{0};
std::atomic<uint64_t> g_inHash{0};     // INPROBE / BINDSPY: the shader whose inputs are examined
std::atomic<int> g_inSlot{-1};         // INPROBE: that input slot is copied per view before the shader's first draw
std::atomic<int> g_bindSpy{0};         // BINDSPY: frames left to log the shader's bindings         // STAGEPROBE: snapshot after each view's K-th list writing the watched texture  // OWNPROBE: measure the views' own textures as they stand at Present instead   // one input at a time (-1 all): a full readback of seven big textures a frame starves the GPU
ID3D11Resource* g_probe[2][kProbeIn] = {};
ID3D11Resource* g_probeSrc[2][kProbeIn] = {};   // which texture it was (shared between the views or not)
uint32_t g_probeFrame[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
const int kLightingPass = pass_of(0x94371e143481d51aull);

void probe_inputs(ID3D11DeviceContext* c, const Bound& b, int view) {
    const uint32_t frame = g_frameNo.load(std::memory_order_relaxed);
    if (view < 0 || g_probeFrame[view] == frame) return;   // the first lighting draw of that view this frame
    g_probeFrame[view] = frame;
    for (int k = 0; k < kProbeIn; ++k) {
        if (g_probeOnly.load(std::memory_order_relaxed) >= 0 && k != g_probeOnly.load(std::memory_order_relaxed)) continue;
        ID3D11Resource* r = kProbeSlot[k] == 32 ? res_of(b.psSrv32) : res_of(b.psSrv[kProbeSlot[k]]);
        g_probeSrc[view][k] = r;
        if (!r) continue;
        ID3D11Resource*& t = g_probe[view][k];
        if (!t || !same(shape_of(t), shape_of(r))) { if (t) t->Release(); t = create_like(c, r); }
        if (t) oCopyRes(c, t, r);
    }
}

// LIGHTSRC (L7, JJ: "measure the good frames and the bad frames from the left eye, in terms of where the lighting is
// coming from"). The game view's gathered light is built by the ambient passes, once per light-probe REGION the view
// overlaps: each binds that region's irradiance volume (t27-30) and reflection cube (t25) and reads CB_ENVMAP_REGION (b3,
// the region's box, volume mapping and strength) and cbSceneParam (b8, camera, pre-exposure). Per frame and view we copy
// both constant buffers at the draw itself (inside the game's list: exactly what the draw reads) and log them next to
// the left eye's brightness of the same frame, so good and bad frames can be compared region by region.
constexpr int kLsFrames = 4, kLsPerFrame = 16;
struct LsRec { int8_t pass, view; ID3D11Resource* ibl; ID3D11Resource* vol; ID3D11Buffer* cb[2]; };
struct LsFrame { uint32_t frame = 0xFFFFFFFFu; int n = 0; LsRec r[kLsPerFrame]; };
LsFrame g_ls[kLsFrames];
ID3D11Buffer* g_lsStage[kLsFrames][kLsPerFrame][2] = {};
std::mutex g_lsMutex;
std::atomic<int> g_lsWant{0};
const int kAmbient1 = pass_of(0x38557003cd595b71ull), kAmbient2 = pass_of(0xa1be3a6dd0edb88eull);
const int kSunPass = pass_of(0xed06754102032497ull);

ID3D11Buffer* staging_like(ID3D11DeviceContext* c, ID3D11Buffer* src, ID3D11Buffer*& slot) {
    D3D11_BUFFER_DESC d{}; src->GetDesc(&d);
    if (slot) { D3D11_BUFFER_DESC e{}; slot->GetDesc(&e); if (e.ByteWidth == d.ByteWidth) return slot; slot->Release(); slot = nullptr; }
    D3D11_BUFFER_DESC sd{}; sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (dev) { dev->CreateBuffer(&sd, nullptr, &slot); dev->Release(); }
    return slot;
}

void lightsrc_record(ID3D11DeviceContext* c, const Bound& b, int pi, int view) {
    const uint32_t frame = g_frameNo.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_lsMutex);
    LsFrame& f = g_ls[frame % kLsFrames];
    if (f.frame != frame) { f.frame = frame; f.n = 0; }
    if (f.n >= kLsPerFrame) return;
    const int i = f.n++;
    LsRec& r = f.r[i];
    r.pass = static_cast<int8_t>(pi); r.view = static_cast<int8_t>(view);
    r.ibl = res_of(b.psSrv[25]); r.vol = res_of(b.psSrv[27]);
    const int slots[2] = {pi == kLightingPass ? 0 : pi == kSunPass ? 5 : 3, pi == kLightingPass ? -1 : 8};   // lighting: b0; sun: b5 cbLight
    for (int k = 0; k < 2; ++k) {
        r.cb[k] = nullptr;
        ID3D11Buffer* src = slots[k] >= 0 ? b.psCb[slots[k]] : nullptr;
        if (!src) continue;
        ID3D11Buffer* st = staging_like(c, src, g_lsStage[frame % kLsFrames][i][k]);
        if (st) { oCopyRes(c, st, src); r.cb[k] = st; }
    }
}

// BINDSPY / INPROBE: what a given shader reads, per view (first draw of the frame)
void inputs_before(ID3D11DeviceContext* c, const Bound& b, bool cs) {
    unsigned bits = 0;
    auto add = [&](ID3D11Resource* r) { const int o = hist_owner(r); if (o >= 0) bits |= 1u << o; };
    if (cs) { for (auto* v : b.csSrv) add(res_of(v)); for (auto* u : b.csUav) add(res_of(u)); }
    else { for (auto* v : b.psSrv) add(res_of(v)); for (auto* r : b.rtvs) add(res_of(r)); }
    const int view = bits == 1 ? 0 : bits == 2 ? 1 : -1;
    static uint32_t spied[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    const uint32_t frame = g_frameNo.load(std::memory_order_relaxed);
    if (g_bindSpy.load(std::memory_order_relaxed) > 0 && view >= 0 && spied[view] != frame) {
        spied[view] = frame;
        for (int k = 0; k < kTrackSrv; ++k) {
            ID3D11Resource* r = res_of(cs ? b.csSrv[k] : b.psSrv[k]);
            if (!r) continue;
            const Shape sh = shape_of(r);
            const int o = hist_owner(r);
            BVR_LOG("bindspy %016llx %s t%-2d %p %ux%ux%u fmt %u%s", static_cast<unsigned long long>(g_inHash.load()), view ? "OURS" : "GAME", k, r,
                    sh.w, sh.h, sh.d, sh.fmt, o < 0 ? "" : o == view ? " (this view's own)" : " (the OTHER view's own!)");
        }
        if (!cs) if (ID3D11Resource* r = res_of(b.psSrv32)) {
            const Shape sh = shape_of(r); const int o = hist_owner(r);
            BVR_LOG("bindspy %016llx %s t32 %p %ux%ux%u fmt %u%s", static_cast<unsigned long long>(g_inHash.load()), view ? "OURS" : "GAME", r, sh.w, sh.h,
                    sh.d, sh.fmt, o < 0 ? "" : o == view ? " (this view's own)" : " (the OTHER view's own!)");
        }
        for (int k = 0; k < 4; ++k) if (ID3D11Resource* r = res_of(cs ? static_cast<ID3D11View*>(b.csUav[k]) : static_cast<ID3D11View*>(b.rtvs[k])))
            BVR_LOG("bindspy %016llx %s %s%d %p", static_cast<unsigned long long>(g_inHash.load()), view ? "OURS" : "GAME", cs ? "u" : "rt", k, r);
    }
    const int slot = g_inSlot.load(std::memory_order_relaxed);
    if (slot >= 0 && (slot < kTrackSrv || (slot == 32 && !cs)) && view >= 0 && g_probeWant.load(std::memory_order_relaxed) > 0 && g_probeFrame[view] != frame) {
        ID3D11Resource* r = slot == 32 ? res_of(b.psSrv32) : res_of(cs ? b.csSrv[slot] : b.psSrv[slot]);
        if (!r) return;
        g_probeFrame[view] = frame;
        g_probeSrc[view][0] = r;
        ID3D11Resource*& t = g_probe[view][0];
        if (!t || !same(shape_of(t), shape_of(r))) { if (t) t->Release(); t = create_like(c, r); }
        if (t) oCopyRes(c, t, r);
    }
}

void pass_probe_after(ID3D11DeviceContext* c, const Bound& b, bool cs) {
    if (g_probeWant.load(std::memory_order_relaxed) <= 0) return;
    unsigned bits = 0;
    auto add = [&](ID3D11Resource* r) { const int o = hist_owner(r); if (o >= 0) bits |= 1u << o; };
    if (cs) { for (auto* v : b.csSrv) add(res_of(v)); for (auto* u : b.csUav) add(res_of(u)); }
    else { for (auto* v : b.psSrv) add(res_of(v)); for (auto* r : b.rtvs) add(res_of(r)); }
    if (bits != 1 && bits != 2) { g_ppUnknown.fetch_add(1, std::memory_order_relaxed); return; }
    const int view = static_cast<int>(bits) - 1;
    const uint32_t frame = g_frameNo.load(std::memory_order_relaxed);
    if (g_probeFrame[view] == frame) return;
    const int rt = g_ppRt.load(std::memory_order_relaxed) & 3;
    ID3D11Resource* r = cs ? res_of(b.csUav[rt]) : res_of(b.rtvs[rt]);
    if (!r) return;
    g_probeFrame[view] = frame;
    g_probeSrc[view][0] = r;
    ID3D11Resource*& t = g_probe[view][0];
    if (!t || !same(shape_of(t), shape_of(r))) { if (t) t->Release(); t = create_like(c, r); }
    if (t) oCopyRes(c, t, r);
}

void pass_before(ID3D11DeviceContext* c, int s, int pi, int8_t& view) {
    const HistPass& p = kPasses[pi];
    const Bound& b = g_bound[s];
    if (g_inHash.load(std::memory_order_relaxed) == p.hash) inputs_before(c, b, p.cs);
    g_passRan[pi].fetch_add(1, std::memory_order_relaxed);
    int8_t via = 0;
    view = static_cast<int8_t>(view_of(b, p, via));
    const uint32_t frame = g_frameNo.load(std::memory_order_relaxed);
    if (p.bit >= 0 && (g_histSwap.load(std::memory_order_relaxed) >> p.bit & 1)) {
        if (view < 0) g_hsUnknown.fetch_add(1, std::memory_order_relaxed);
        else for (int k = 0; k < p.nHist; ++k) {
            ID3D11Resource* H = slot_res(b, p, p.hist[k]);
            Save& sv = g_save[view][pi][k];
            if (!H) continue;
            if (!sv.valid) { g_hsFirst.fetch_add(1, std::memory_order_relaxed); continue; }
            if (sv.frame == frame) { g_hsTwice.fetch_add(1, std::memory_order_relaxed); continue; }  // a second run this frame
            if (!same(shape_of(H), shape_of(sv.tex))) { g_hsShape.fetch_add(1, std::memory_order_relaxed); continue; }
            oCopyRes(c, H, sv.tex);
            g_hsRestore.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (pi == kLightingPass && g_probeWant.load(std::memory_order_relaxed) > 0 && !g_ppHash.load(std::memory_order_relaxed) &&
        !g_stageK.load(std::memory_order_relaxed) && g_inSlot.load(std::memory_order_relaxed) < 0) probe_inputs(c, b, view);
    if ((pi == kAmbient1 || pi == kAmbient2 || pi == kLightingPass || pi == kSunPass) && view >= 0 && g_lsWant.load(std::memory_order_relaxed) > 0)
        lightsrc_record(c, b, pi, view);
    if (g_histSpy.load(std::memory_order_relaxed) > 0 && g_ctxEv[s].n < kEvPerList) {
        HistEv& e = g_ctxEv[s].ev[g_ctxEv[s].n++];
        e.pass = static_cast<int8_t>(pi); e.view = view; e.via = via;
        for (int k = 0; k < 3; ++k) {
            e.hist[k] = k < p.nHist ? slot_res(b, p, p.hist[k]) : nullptr;
            e.src[k] = k < p.nHist ? slot_res(b, p, p.src[k]) : nullptr;
        }
    }
}

void pass_after(ID3D11DeviceContext* c, int s, int pi, int8_t view) {
    const HistPass& p = kPasses[pi];
    if (g_ppHash.load(std::memory_order_relaxed) == p.hash) pass_probe_after(c, g_bound[s], p.cs);
    if (view < 0 || p.bit < 0 || !(g_histSwap.load(std::memory_order_relaxed) >> p.bit & 1)) return;
    const Bound& b = g_bound[s];
    for (int k = 0; k < p.nHist; ++k) {
        ID3D11Resource* S = slot_res(b, p, p.src[k]);
        if (!S) continue;
        Save& sv = g_save[view][pi][k];
        if (!sv.tex || !same(shape_of(S), shape_of(sv.tex))) {
            if (sv.tex) sv.tex->Release();
            sv.valid = false;
            sv.tex = create_like(c, S);
            if (!sv.tex) { g_hsShape.fetch_add(1, std::memory_order_relaxed); continue; }
        }
        oCopyRes(c, sv.tex, S);
        sv.valid = true;
        sv.frame = g_frameNo.load(std::memory_order_relaxed);
        g_hsSave.fetch_add(1, std::memory_order_relaxed);
    }
}

void spy_replay(void* list, int ex) {
    for (auto& l : g_listEv) {
        if (l.list.load(std::memory_order_acquire) != list) continue;
        static const char* kView[3] = {"?", "MAIN", "OURS"};
        static const char* kVia[4] = {"nothing to tell", "own texture", "sun map", "BOTH views' textures"};
        for (int i = 0; i < l.n; ++i) {
            const HistEv& e = l.ev[i];
            const HistPass& p = kPasses[e.pass];
            const Shape h = shape_of(e.hist[0]);
            BVR_LOG("histspy   replay #%d: %-17s view %s (%s) | history %p %p %p (%ux%ux%u f%u) | this frame %p %p %p",
                    ex, p.name, kView[e.view + 1], kVia[e.via & 3], e.hist[0], e.hist[1], e.hist[2], h.w, h.h, h.d, h.fmt,
                    e.src[0], e.src[1], e.src[2]);
        }
        l.list.store(nullptr, std::memory_order_release);
        return;
    }
}

std::atomic<uint8_t> g_ctxSrvBits[128]; // owner bits of the textures bound since the context's last render-target change
// INPUTS: the pixel-shader textures bound on a context while it draws into T (what the final pass reads).
std::atomic<uint8_t> g_ctxInT[kCtx];
std::mutex g_inMutex;
// `tag` = which view's own textures this context had read when it bound this one (1 main, 2 ours, 3 both): it tells
// us whether the two views share a texture (one entry, tag 3) or each has its own (two entries) - EXPSHARE 2026-10-29.
// N2 (2026-11-01, JJ's crash at 11:29:53 on travel): `r` is only an IDENTITY - never call it later. The 10-s report used to
// QueryInterface it after a travel had freed the texture (DEP fault in d3d11, picsplit.cpp on_present). Its size and
// format are taken while it is bound and alive.
struct In { ID3D11Resource* r; UINT slot; uint32_t hits; uint8_t tag; UINT w, h, fmt; };
In g_inputs[24]; int g_inputN = 0;
using PSSetSRV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
PSSetSRV oPSSRV = nullptr;
void STDMETHODCALLTYPE hkPSSRV(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    const int s = g_on.load(std::memory_order_relaxed) ? ctx_slot(c) : -1;
    if (g_sdFrames.load(std::memory_order_relaxed) > 0 && v && n <= 128) sd_srv(s, 0, start, n, v);
    if (s >= 0 && v) {   // L7: what this list's draws read (before any lend changes it)
        Bound& b = g_bound[s];
        for (UINT i = 0; i < n; ++i) {
            const UINT slot = start + i;
            if (slot < kTrackSrv) b.psSrv[slot] = v[i];
            if (slot == kShadowSlot) {
                const uint8_t code = v[i] ? shadow_code(v[i]) : 0;
                b.psSun = code == 0 ? 0 : code <= kShadowTex ? 1 : 2;
                b.psSrv32 = v[i];
            }
        }
    }
    if (s >= 0 && v) {
        uint8_t bits = 0;
        for (UINT i = 0; i < n && i < 16; ++i) if (v[i]) bits |= srv_owner_bits(v[i]);
        if (bits) {
            g_ctxSrvBits[s].fetch_or(bits, std::memory_order_relaxed);
            if (g_ctxInT[s].load(std::memory_order_relaxed)) g_ctxTag[s].fetch_or(bits, std::memory_order_relaxed);
        }
        // SHADOWLEND: a bind of OUR sun shadow map becomes the main view's same map (texture identity, never the draw).
        // LENDANY (2026-10-29, MAX shadows): at any pixel slot - at MAX the maps are read at t1 by the copy pass 30e33769
        // that fills each view's own 4096x4096 shadow texture (the MAX sun pass reads that at t32), never at t32.
        // Mode 1 = t32 only (the L5 behaviour, default); mode 2 = every slot (MAX test: made BOTH eyes sunless, M4b).
        if (lend3() && n <= 32) {   // SHADOWX
            ID3D11ShaderResourceView* copy[32];
            if (x_swap(0, n, v, copy)) { oPSSRV(c, start, n, copy); return; }
        } else if (const int lend = g_shadowLend.load(std::memory_order_relaxed) == 3 ? 1 : g_shadowLend.load(std::memory_order_relaxed);
                   lend && n <= 32) {
            ID3D11ShaderResourceView* copy[32];
            bool swapped = false;
            for (UINT i = 0; i < n; ++i) {
                copy[i] = v[i];
                if (!v[i] || (lend == 1 && start + i != kShadowSlot)) continue;
                const uint8_t code = shadow_code(v[i]);
                if (code >= 1 && code <= kShadowTex) {                    // the main view's map: remember the game's SRV
                    g_mainShadowSrv[code - 1].store(v[i], std::memory_order_relaxed);
                    g_lendTook.fetch_add(1, std::memory_order_relaxed);
                } else if (code > kShadowTex) {                           // our map: hand over the main view's
                    ID3D11ShaderResourceView* m = g_mainShadowSrv[code - kShadowTex - 1].load(std::memory_order_relaxed);
                    if (m && m != v[i]) { copy[i] = m; swapped = true; g_lendGave.fetch_add(1, std::memory_order_relaxed); }
                    else g_lendSkipped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (swapped) { oPSSRV(c, start, n, copy); return; }
        }
    }
    if (s >= 0 && g_ctxInT[s].load(std::memory_order_relaxed) && v) {
        std::lock_guard<std::mutex> lk(g_inMutex);
        for (UINT i = 0; i < n && i < 8; ++i) {
            if (!v[i]) continue;
            ID3D11Resource* r = nullptr; v[i]->GetResource(&r);
            if (!r) continue;
            const uint8_t tag = g_ctxSrvBits[s].load(std::memory_order_relaxed) & 3;
            bool found = false;
            for (int k = 0; k < g_inputN; ++k)
                if (g_inputs[k].r == r && g_inputs[k].slot == start + i && g_inputs[k].tag == tag) { ++g_inputs[k].hits; found = true; }
            if (!found && g_inputN < 24) {
                D3D11_TEXTURE2D_DESC d{}; D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
                g_inputs[g_inputN++] = {r, start + i, 1, tag, d.Width, d.Height, static_cast<UINT>(d.Format)};
            }
            r->Release();
        }
    }
    oPSSRV(c, start, n, v);
}

// WRITERS (L7): every shader that writes one texture during a frame (render target 0-3 or compute UAV 0-3).
std::atomic<ID3D11Resource*> g_writeWatch{nullptr};
std::atomic<ID3D11Resource*> g_writeWatchB{nullptr};   // STAGEPROBE: the same texture of our view
std::atomic<bool> g_writeQuiet{false};                 // no per-frame writer log (STAGEPROBE)
bool g_stageLog = false;                               // log the snapshot points once
std::atomic<int> g_writeFrames{0};
std::mutex g_writeMutex;
struct Writer { uint64_t hash; bool cs; uint32_t n; };
Writer g_writers[32]; int g_writerN = 0;
// ... and in GPU order: lists that wrote it carry the count and first shader to the replay
std::atomic<uint32_t> g_ctxWHits[kCtx]; std::atomic<uint64_t> g_ctxWHash[kCtx]; std::atomic<uint8_t> g_ctxWView[kCtx];
struct ListW { std::atomic<void*> list; uint32_t hits; uint64_t hash; uint8_t views; };
ListW g_listW[128]; std::atomic<uint32_t> g_listWHead{0};
void check_writer(const Bound& b, bool cs) {
    ID3D11Resource* w = g_writeWatch.load(std::memory_order_relaxed);
    ID3D11Resource* w2 = g_writeWatchB.load(std::memory_order_relaxed);
    if (!w) return;
    uint8_t hit = 0;
    auto test = [&](ID3D11Resource* r) { if (!r) return; if (r == w) hit |= 1; if (w2 && r == w2) hit |= 2; };
    if (cs) { for (auto* u : b.csUav) if (u) test(res_of(u)); }
    else { for (auto* r : b.rtvs) if (r) test(res_of(r)); }
    if (!hit) return;
    if (const int s = static_cast<int>(&b - g_bound); s >= 0 && s < kCtx) g_ctxWView[s].fetch_or(hit, std::memory_order_relaxed);
    const uint64_t h = cs ? b.csHash : b.psHash;
    if (const int s = static_cast<int>(&b - g_bound); s >= 0 && s < kCtx) {
        if (g_ctxWHits[s].fetch_add(1, std::memory_order_relaxed) == 0) g_ctxWHash[s].store(h, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(g_writeMutex);
    for (int i = 0; i < g_writerN; ++i) if (g_writers[i].hash == h && g_writers[i].cs == cs) { ++g_writers[i].n; return; }
    if (g_writerN < 32) g_writers[g_writerN++] = {h, cs, 1};
}

// PASSPROBE (L7): the output of one shader (by geo11 hash), per view, copied right after its first draw of the frame;
// measured frame to frame by the LITPROBE machinery (slot 0). The view is told by its own textures among the bindings.
void pass_probe_after(ID3D11DeviceContext* c, const Bound& b, bool cs);

// ---- L7 hooks: shaders, compute textures, draws ----
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using CSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIdxFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
PSSetShaderFn oPSShader = nullptr; CSSetShaderFn oCSShader = nullptr; PSSetSRV oCSSRV = nullptr;
DrawFn oDraw = nullptr; DrawIdxFn oDrawIdx = nullptr; DispatchFn oDispatch = nullptr;

using PSSetCBFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
PSSetCBFn oPSCB = nullptr;
void STDMETHODCALLTYPE hkPSCB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* cb) {
    if (g_sdFrames.load(std::memory_order_relaxed) > 0 && cb)
        if (const int s = ctx_slot(c); s >= 0)
            for (UINT i = 0; i < n; ++i)
                for (auto& b : g_sdCbBuf) if (cb[i] && b.load(std::memory_order_relaxed) == cb[i]) { g_sdCtxCb[s].fetch_add(1, std::memory_order_relaxed); break; }
    if (g_on.load(std::memory_order_relaxed) && cb)
        if (const int s = ctx_slot(c); s >= 0)
            for (UINT i = 0; i < n; ++i) if (start + i < 16) { g_bound[s].psCb[start + i] = cb[i]; g_bound[s].psCbFirst[start + i] = 0; }
    oPSCB(c, start, n, cb);
}
using PSSetCB1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
PSSetCB1Fn oPSCB1 = nullptr;
void STDMETHODCALLTYPE hkPSCB1(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* cb, const UINT* first, const UINT* num) {
    if (g_sdFrames.load(std::memory_order_relaxed) > 0 && cb)
        if (const int s = ctx_slot(c); s >= 0)
            for (UINT i = 0; i < n; ++i)
                for (auto& b : g_sdCbBuf) if (cb[i] && b.load(std::memory_order_relaxed) == cb[i]) { g_sdCtxCb[s].fetch_add(1, std::memory_order_relaxed); break; }
    if (g_on.load(std::memory_order_relaxed) && cb)
        if (const int s = ctx_slot(c); s >= 0)
            for (UINT i = 0; i < n; ++i) if (start + i < 16) { g_bound[s].psCb[start + i] = cb[i]; g_bound[s].psCbFirst[start + i] = first ? first[i] : 0; }
    oPSCB1(c, start, n, cb, first, num);
}

// INSTCAP (L7): for ONE frame, every G-buffer draw's instance block (b4 window, 544 bytes) is copied in order into a capture
// buffer on the GPU, with the draw's view, shader and the engine function that issued it.
constexpr int kInstCap = 2048;
constexpr UINT kInstSize = 544;
ID3D11Buffer* g_instBuf = nullptr;
struct InstRec { int8_t view; uint64_t hash; void* ret; UINT first; UINT width; ID3D11Buffer* buf; };
InstRec g_instRec[kInstCap];
std::atomic<int> g_instN{0};
std::atomic<uint32_t> g_instFrame{0xFFFFFFFFu};
std::atomic<int> g_instState{0};       // 0 idle, 1 armed (capturing frame g_instFrame), 2 to read back
void inst_capture(ID3D11DeviceContext* c, const Bound& b, void* ret) {
    if (!b.rtvs[3] || !b.psCb[4]) return;
    if (!g_instBuf) {
        static std::mutex m; std::lock_guard<std::mutex> lk(m);
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        if (dev && !g_instBuf) { D3D11_BUFFER_DESC d{}; d.ByteWidth = kInstCap * kInstSize; d.Usage = D3D11_USAGE_DEFAULT; dev->CreateBuffer(&d, nullptr, &g_instBuf); }
        if (dev) dev->Release();
        if (!g_instBuf) return;
    }
    const int i = g_instN.fetch_add(1, std::memory_order_relaxed);
    if (i >= kInstCap) return;
    D3D11_BUFFER_DESC d{}; b.psCb[4]->GetDesc(&d);
    const UINT off = b.psCbFirst[4] * 16;
    InstRec& r = g_instRec[i];
    unsigned bits = 0;
    for (auto* rt : b.rtvs) if (rt) { const int o = hist_owner(res_of(rt)); if (o >= 0) bits |= 1u << o; }
    r.view = static_cast<int8_t>(bits == 1 ? 0 : bits == 2 ? 1 : -1);
    r.hash = b.psHash; r.ret = ret; r.first = b.psCbFirst[4]; r.width = d.ByteWidth; r.buf = b.psCb[4];
    if (off + kInstSize > d.ByteWidth) { r.view = -2; return; }
    D3D11_BOX box{off, 0, 0, off + kInstSize, 1, 1};
    oCopySub(c, g_instBuf, 0, static_cast<UINT>(i) * kInstSize, 0, 0, b.psCb[4], 0, &box);
}

void STDMETHODCALLTYPE hkPSShader(ID3D11DeviceContext* c, ID3D11PixelShader* sh, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_on.load(std::memory_order_relaxed)) {
        const int s = ctx_slot(c);
        if (s >= 0) {
            g_bound[s].psHash = bvr::ds3::shaderid::hash_of(sh);
            g_bound[s].ps = sh;
            const int pi = pass_of(g_bound[s].psHash);
            g_bound[s].psPass = static_cast<int8_t>(pi);
            if (pi >= 0) g_passBound[pi].fetch_add(1, std::memory_order_relaxed);
        }
    }
    oPSShader(c, sh, ci, n);
}
void STDMETHODCALLTYPE hkCSShader(ID3D11DeviceContext* c, ID3D11ComputeShader* sh, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_on.load(std::memory_order_relaxed)) {
        const int s = ctx_slot(c);
        if (s >= 0) {
            g_bound[s].csHash = bvr::ds3::shaderid::hash_of(sh);
            const int pi = pass_of(g_bound[s].csHash);
            g_bound[s].csPass = static_cast<int8_t>(pi);
            if (pi >= 0) g_passBound[pi].fetch_add(1, std::memory_order_relaxed);
        }
    }
    oCSShader(c, bvr::ds3::tilefix::swap(sh), ci, n);   // TILEFIX: the light-tile culling, corrected for the eye's lens
}
CSSetShaderFn oCSShaderI = nullptr;
void STDMETHODCALLTYPE hkCSShaderI(ID3D11DeviceContext* c, ID3D11ComputeShader* sh, ID3D11ClassInstance* const* ci, UINT n) {
    oCSShaderI(c, bvr::ds3::tilefix::swap(sh), ci, n);   // TILEFIX on the immediate context
}
// VOLLEND: SHADOWLEND for the compute stage. The volumetric light pass reads the sun map at CS t3
// (g_DepthShadowTexture); a bind of OUR map there gets the main view's same map (texture identity, never the draw).
constexpr UINT kVolSunSlot = 3;
void STDMETHODCALLTYPE hkCSSRV(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    const int s = g_on.load(std::memory_order_relaxed) ? ctx_slot(c) : -1;
    if (g_sdFrames.load(std::memory_order_relaxed) > 0 && v && n <= 128) sd_srv(s, 1, start, n, v);
    if (s >= 0 && v && n <= 32) {
        Bound& b = g_bound[s];
        for (UINT i = 0; i < n; ++i) if (start + i < kTrackSrv) b.csSrv[start + i] = v[i];
        // VOLLEND / LENDX: every compute slot (MAX reads X at t0 and t3)
        ID3D11ShaderResourceView* copy[32];
        if (lend3()) {   // SHADOWX
            if (x_swap(1, n, v, copy)) { oCSSRV(c, start, n, copy); return; }   // the record keeps OUR X (tells the view)
        } else {
        bool swapped = false;
        for (UINT i = 0; i < n; ++i) {
            copy[i] = v[i];
            const uint8_t code = v[i] ? shadow_code(v[i]) : 0;
            if (start + i == kVolSunSlot) b.csSun = code == 0 ? 0 : code <= kShadowTex ? 1 : 2;
            if (code >= 1 && code <= kShadowTex) g_mainShadowSrvCS[code - 1].store(v[i], std::memory_order_relaxed);
            else if (code > kShadowTex && g_volLend.load(std::memory_order_relaxed)) {
                const int q = code - kShadowTex - 1;
                ID3D11ShaderResourceView* m = g_mainShadowSrvCS[q].load(std::memory_order_relaxed);
                if (!m) m = g_mainShadowSrv[q].load(std::memory_order_relaxed);
                if (m && m != v[i]) {
                    copy[i] = m;
                    if (start + i < kTrackSrv) b.csSrv[start + i] = m;
                    swapped = true;
                    g_volGave.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (swapped) { oCSSRV(c, start, n, copy); return; }
        }
    }
    oCSSRV(c, start, n, v);
}
UINT cb_bytes(ID3D11Resource* r);   // defined further down with the constant-buffer hooks

// ---- MARKERS (M8): HUD draws ---------------------------------------------------------------------------------
// Scaleform draws the whole HUD into its own sheet, so a draw whose render target IS that sheet is a HUD piece. Its
// placement lives in the 96-byte Constants buffer (cxadd, cxmul, mvp[2], texgen[2] - the geo11 fix's own declaration
// of Dark Souls III's UI shaders), which the game rewrites per draw; the last write before the draw in RECORDING order is the
// one that draw uses, so a small shadow of those writes is enough.
std::atomic<ID3D11Resource*> g_hudSheet{nullptr};
bool view_is_hud(ID3D11View* v) {
    ID3D11Resource* sheet = g_hudSheet.load(std::memory_order_relaxed);
    if (!v || !sheet) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return false;
    const bool hit = r == sheet;
    r->Release();
    return hit;
}
constexpr UINT kGfxMaxBytes = 16384;   // the HUD's own b0 measured 3840; a few sizes exist, so keep headroom
constexpr int kGfxShadow = 8;
struct GfxCb { std::atomic<ID3D11Resource*> buf{nullptr}; UINT bytes = 0; uint8_t data[kGfxMaxBytes]; };
GfxCb g_gfxCb[kGfxShadow];
std::atomic<uint32_t> g_gfxHead{0};
std::mutex g_gfxMutex;
void gfx_note(ID3D11Resource* buf, const void* data, UINT bytes) {
    if (!buf || !data || !bytes || bytes > kGfxMaxBytes) return;
    std::lock_guard<std::mutex> lk(g_gfxMutex);
    for (auto& e : g_gfxCb)
        if (e.buf.load(std::memory_order_relaxed) == buf) { memcpy(e.data, data, bytes); e.bytes = bytes; return; }
    GfxCb& e = g_gfxCb[g_gfxHead.fetch_add(1, std::memory_order_relaxed) % kGfxShadow];
    memcpy(e.data, data, bytes);
    e.bytes = bytes;
    e.buf.store(buf, std::memory_order_relaxed);
}
// The six float4s this piece reads: the window the draw was given (16-byte units) inside the shadowed buffer.
// BATCH (V11): how many float4 rows of the shadow follow a window start (a batched HUD draw indexes up to 144 of them)
unsigned gfx_rows(ID3D11Resource* buf, UINT firstConstant) {
    if (!buf) return 0;
    std::lock_guard<std::mutex> lk(g_gfxMutex);
    for (auto& e : g_gfxCb)
        if (e.buf.load(std::memory_order_relaxed) == buf) return e.bytes / 16u > firstConstant ? e.bytes / 16u - firstConstant : 0u;
    return 0;
}
const float* gfx_of(ID3D11Resource* buf, UINT firstConstant) {
    if (!buf) return nullptr;
    std::lock_guard<std::mutex> lk(g_gfxMutex);
    for (auto& e : g_gfxCb)
        if (e.buf.load(std::memory_order_relaxed) == buf) {
            const UINT off = firstConstant * 16u;
            if (off + 96u > e.bytes) return nullptr;
            return reinterpret_cast<const float*>(e.data + off);
        }
    return nullptr;
}
// Our own Map record for those buffers: the engine's shared one (g_ctxMap) only keeps constant buffers of 512 bytes
// and more, and its size is packed into a byte (3840 overflowed it).
struct GfxMap { ID3D11Resource* r = nullptr; void* data = nullptr; UINT bytes = 0; };
GfxMap g_gfxMap[kCtx];
void gfx_map(ID3D11DeviceContext* c, ID3D11Resource* r, void* data) {
    const UINT bytes = cb_bytes(r);
    if (!bytes || bytes > kGfxMaxBytes) return;
    if (const int s = ctx_slot(c); s >= 0) { g_gfxMap[s].r = r; g_gfxMap[s].data = data; g_gfxMap[s].bytes = bytes; }
}
void gfx_unmap(ID3D11DeviceContext* c, ID3D11Resource* r) {
    if (const int s = ctx_slot(c); s >= 0 && g_gfxMap[s].r == r && g_gfxMap[s].data) {
        gfx_note(r, g_gfxMap[s].data, g_gfxMap[s].bytes);
        g_gfxMap[s].r = nullptr; g_gfxMap[s].data = nullptr;
    }
}
HudDrawFn g_hudDraw = nullptr;
std::atomic<uint32_t> g_hudDraws{0}, g_hudNoCb{0}, g_hudTaken{0}, g_hudArmed{0};
// HUDPROBE (`ds hudprobe N`): for N frames, every render target that draws land on, with its size, format and how many
// draws went to it, plus how many of those had a 96-byte vertex constant buffer. The HUD's own sheet is then obvious
// (hundreds of small draws, most with a Scaleform placement) and so is whether it is the texture we believe it is.
struct RtTally { std::atomic<ID3D11Resource*> res{nullptr}; uint32_t draws, gfx; UINT w, h; int fmt; UINT cb0; };
constexpr int kRtTally = 192;
RtTally g_rtTally[kRtTally];
std::atomic<int> g_hudProbe{0};
std::mutex g_tallyMutex;
void tally_draw(ID3D11RenderTargetView* v, bool hasGfxCb, UINT cb0Bytes) {
    if (!v) return;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return;
    r->Release();   // the game owns it; we only use the pointer as an identity
    std::lock_guard<std::mutex> lk(g_tallyMutex);
    for (auto& t : g_rtTally) {
        if (t.res.load(std::memory_order_relaxed) == r) { ++t.draws; if (hasGfxCb) ++t.gfx; if (cb0Bytes) t.cb0 = cb0Bytes; return; }
        if (!t.res.load(std::memory_order_relaxed)) {
            D3D11_RESOURCE_DIMENSION d{};
            r->GetType(&d);
            if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td{};
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
                t.w = td.Width; t.h = td.Height; t.fmt = static_cast<int>(td.Format);
            }
            t.draws = 1; t.gfx = hasGfxCb ? 1u : 0u; t.cb0 = cb0Bytes;
            t.res.store(r, std::memory_order_relaxed);
            return;
        }
    }
}
void tally_report() {
    std::lock_guard<std::mutex> lk(g_tallyMutex);
    ID3D11Resource* sheet = g_hudSheet.load(std::memory_order_relaxed);
    BVR_LOG("hudprobe: render targets drawn to (the HUD sheet we believe in is %p):", (void*)sheet);
    for (auto& t : g_rtTally) {
        ID3D11Resource* r = t.res.load(std::memory_order_relaxed);
        if (!r) break;
        BVR_LOG("hudprobe   %p %ux%u format %d - %u draws, vertex b0 is %u bytes (%u draws had a shadowed 96-byte "
                "one)%s", (void*)r, t.w, t.h, t.fmt, t.draws, t.cb0, t.gfx, r == sheet ? "   <= the sheet" : "");
        t.res.store(nullptr, std::memory_order_relaxed);
    }
}
// Returns true when the marker code drew this piece into the eyes, so the game's own draw must be skipped.
bool hud_piece(ID3D11DeviceContext* c, int s, bool indexed, UINT count, UINT start, INT base, UINT inst) {
    if (s < 0) return false;
    if (g_hudProbe.load(std::memory_order_relaxed) > 0)
        tally_draw(g_bound[s].rtv0, gfx_of(g_bound[s].vsCb0, g_bound[s].vsCb0First) != nullptr, cb_bytes(g_bound[s].vsCb0));
    if (!g_hudDraw) return false;
    g_hudArmed.fetch_add(1, std::memory_order_relaxed);
    if (!view_is_hud(g_bound[s].rtv0)) return false;
    g_hudDraws.fetch_add(1, std::memory_order_relaxed);
    const float* cb = gfx_of(g_bound[s].vsCb0, g_bound[s].vsCb0First);
    if (!cb) { g_hudNoCb.fetch_add(1, std::memory_order_relaxed); return false; }
    const bool took = g_hudDraw(c, g_bound[s].vs, cb, gfx_rows(g_bound[s].vsCb0, g_bound[s].vsCb0First), indexed, count, start, base, inst);
    (void)bvr::ds3::markers::take_flat_override();   // (batched HUD draws are on the immediate context in DS3)
    { ID3D11GeometryShader* g = nullptr; ID3D11Buffer* b = nullptr; (void)bvr::ds3::markers::take_flat_gs(&g, &b); }
    if (took) g_hudTaken.fetch_add(1, std::memory_order_relaxed);
    return took;
}
using VSShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
VSShaderFn oVSShader = nullptr;
void STDMETHODCALLTYPE hkVSShader(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_on.load(std::memory_order_relaxed)) if (const int s = ctx_slot(c); s >= 0) g_bound[s].vs = vs;
    oVSShader(c, vs, ci, n);
}
using VSCBFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
VSCBFn oVSCB = nullptr;
void STDMETHODCALLTYPE hkVSCB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
    if (g_on.load(std::memory_order_relaxed) && b && start == 0 && n >= 1)
        if (const int s = ctx_slot(c); s >= 0) { g_bound[s].vsCb0 = b[0]; g_bound[s].vsCb0First = 0; }
    oVSCB(c, start, n, b);
}
// VSSetConstantBuffers1: the engine hands each HUD piece its own WINDOW into one big buffer (measured 2026-10-30).
using VSCB1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
VSCB1Fn oVSCB1 = nullptr;
void STDMETHODCALLTYPE hkVSCB1(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b, const UINT* first,
                               const UINT* num) {
    if (g_on.load(std::memory_order_relaxed) && b && start == 0 && n >= 1)
        if (const int s = ctx_slot(c); s >= 0) { g_bound[s].vsCb0 = b[0]; g_bound[s].vsCb0First = first ? first[0] : 0; }
    oVSCB1(c, start, n, b, first, num);
}

// ---- HUDSHEET (DS3, JJ 2026-10-30: "The game HUD should be a floating panel, not locked to your head tracking") ----
// Sekiro draws its Scaleform HUD into a sheet of its own; Dark Souls III draws it straight into the shared final
// picture (split dump: writes 11 -> 12 add only the HUD), so it rode the head in the right eye and was missing from
// the left. The engine records on deferred contexts, so the framework's draw-time HUD capture (immediate context)
// never sees these draws; instead each HUD draw is re-bound AS IT IS RECORDED into a sheet of ours, with a
// depth-stencil of its own (Scaleform clips with the stencil - Batman City HUDDSV: never lend it the scene's), and
// that sheet goes to the existing room-fixed HUD panel.
// A HUD draw = the shared picture bound as its target AND a HUD vertex shader. The four HUD vertex shaders are the
// ones the geo-11 fix names ([ShaderOverrideHUDVS1..4] in its d3dx.ini); any other Scaleform shader (the menus) is
// recognised by its own constant block ("Constants" holding "mvp"), the signature v1 DS3 used.
constexpr uint64_t kHudVs[] = {0xf10647ba22ca9348ull, 0xaa4d136778e07873ull, 0xfa3d4612573b6d52ull, 0x6c9e5a5657fff33dull};
std::atomic<int> g_hudRedirect{1};
std::atomic<bool> g_hudOn{false};   // the HUD sheet runs (native pair or AFW) - see set_hud_on
std::atomic<ID3D11Texture2D*> g_myHud{nullptr};
std::atomic<ID3D11RenderTargetView*> g_myHudRtv{nullptr};
std::atomic<ID3D11DepthStencilView*> g_myHudDsv{nullptr};
std::atomic<uint32_t> g_hudRedirected{0}, g_hudByHash{0}, g_hudBySig{0}, g_hudLists{0};
bool g_hudBound[kCtx] = {};
std::mutex g_hudVsMx;
std::unordered_map<const void*, uint8_t> g_hudVsKind;   // 0 not HUD, 1 by the fix's hash, 2 by Scaleform signature

uint8_t hud_vs_kind(ID3D11VertexShader* vs) {
    if (!vs) return 0;
    {
        std::lock_guard<std::mutex> lk(g_hudVsMx);
        auto it = g_hudVsKind.find(vs);
        if (it != g_hudVsKind.end()) return it->second;
    }
    uint8_t kind = 0;
    const uint64_t h = shaderid::hash_of(vs);
    for (uint64_t k : kHudVs) if (h == k) kind = 1;
    if (!kind) {
        const void* code = nullptr; size_t len = 0;
        if (shaderid::vs_code(vs, &code, &len) && code && len) {
            ID3D11ShaderReflection* r = nullptr;
            if (SUCCEEDED(D3DReflect(code, len, IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&r))) && r) {
                ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName("Constants");
                D3D11_SHADER_BUFFER_DESC bd{};
                if (cb && SUCCEEDED(cb->GetDesc(&bd))) {
                    // "mvp": one Scaleform piece per draw; "vfuniforms": Scaleform's batched draws (many pieces, each
                    // with its own placement in the array) - the item-slot frames and the souls counter (hudtex 2026-10-30)
                    for (const char* name : {"mvp", "vfuniforms"}) {
                        ID3D11ShaderReflectionVariable* v = cb->GetVariableByName(name);
                        D3D11_SHADER_VARIABLE_DESC vd{};
                        if (v && SUCCEEDED(v->GetDesc(&vd))) kind = 2;
                    }
                }
                r->Release();
            }
        }
        if (kind == 2) BVR_LOG("hudsheet: Scaleform vertex shader %016llx (not one of the geo-11 fix's four) is a HUD shader too",
                               static_cast<unsigned long long>(h));
    }
    std::lock_guard<std::mutex> lk(g_hudVsMx);
    g_hudVsKind[vs] = kind;
    return kind;
}

// POSTLOG (`ds postlog N`, 2026-10-30, JJ: "fix the volumetric light"): Dark Souls III's volumetric light is a
// screen-space light-shaft pass (the geo-11 fix's "Sun shafts" VS b68f3b72bc36908c shifts per eye only while one of
// its SunRT buffers is bound - made from one viewpoint and laid over the picture). None of Sekiro's pass hashes exist
// in this game (no "shaderid: the game created" line in any DS3 log), so the post chain is mapped from scratch: for N
// frames, every draw WITHOUT a depth buffer (full-screen passes) and every draw into the shared picture, in the order
// the GPU runs them (deferred lists at their replay), with the view owning its target / inputs and where the left
// eye's capture point falls.
struct PRec {
    uint64_t vs, ps; ID3D11Resource* rt; UINT rw, rh; int rf; ID3D11Resource* t0; UINT tw, th; int tf;
    int8_t rtOwner; uint8_t inBits; uint8_t isT; uint8_t mark;   // mark: 0 draw, 1 list replayed (T-write), 2 left captured
    int replay; int tag;
    uint8_t cs; char ins[160];   // compute dispatch; every bound input's size/format (wide mode)
};
std::atomic<int> g_postFrames{0};
std::vector<PRec> g_plRec[kCtx];             // per deferred context, while it records
std::mutex g_plMx;
std::unordered_map<const void*, std::vector<PRec>> g_plPending;   // finished lists awaiting their replay
std::vector<PRec> g_plFrame;                 // this frame, GPU order (immediate thread only)
void pl_desc(ID3D11Resource* r, UINT& w, UINT& h, int& f) {
    w = h = 0; f = 0;
    if (!r) return;
    D3D11_RESOURCE_DIMENSION d{};
    r->GetType(&d);
    if (d != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { f = -static_cast<int>(d); return; }
    D3D11_TEXTURE2D_DESC td{};
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
    w = td.Width; h = td.Height; f = static_cast<int>(td.Format);
}
PRec pl_make(ID3D11VertexShader* vs, uint64_t psHash, ID3D11RenderTargetView* rtv, ID3D11ShaderResourceView* const* srv, int nsrv) {
    PRec p{};
    p.vs = shaderid::hash_of(vs); p.ps = psHash;
    ID3D11Resource* r = nullptr;
    if (rtv) { rtv->GetResource(&r); if (r) r->Release(); }
    p.rt = r; pl_desc(r, p.rw, p.rh, p.rf);
    p.rtOwner = static_cast<int8_t>(r ? owner_of(r) : -1);
    p.isT = r && r == g_T.load(std::memory_order_relaxed);
    for (int i = 0; i < nsrv; ++i) if (srv[i]) {
        ID3D11Resource* t = nullptr; srv[i]->GetResource(&t);
        if (t) { const int o = owner_of(t); if (o >= 0) p.inBits |= static_cast<uint8_t>(1u << o); t->Release(); }
        if (i == 0) p.t0 = t;
    }
    pl_desc(p.t0, p.tw, p.th, p.tf);
    int k = 0;
    for (int i = 0; i < nsrv && k < 140; ++i) if (srv[i]) {
        ID3D11Resource* t = nullptr; srv[i]->GetResource(&t);
        UINT w = 0, h = 0; int f = 0; pl_desc(t, w, h, f);
        if (t) t->Release();
        k += sprintf_s(p.ins + k, sizeof(p.ins) - k, " t%d:%ux%u/%d", i, w, h, f);
    }
    return p;
}
void postlog_draw(int s) {   // deferred context, before the draw
    if (s < 0 || g_postFrames.load(std::memory_order_relaxed) <= 0) return;
    const Bound& b = g_bound[s];
    const bool isT = view_is_T(b.rtv0);
    if (!b.rtv0 || g_plRec[s].size() >= 400) return;
    if (b.dsv && !isT) {   // with a depth buffer: only half-size targets (the volumetric works at half the eye size)
        ID3D11Texture2D* T = g_T.load(std::memory_order_relaxed);
        ID3D11Resource* r = nullptr; b.rtv0->GetResource(&r);
        UINT w = 0, h = 0; int f = 0; pl_desc(r, w, h, f);
        if (r) r->Release();
        D3D11_TEXTURE2D_DESC td{}; if (T) T->GetDesc(&td);
        if (!T || w * 2 > td.Width + 2 || w * 2 + 2 < td.Width) return;
    }
    g_plRec[s].push_back(pl_make(b.vs, b.psHash, b.rtv0, b.psSrv, 8));
}
void postlog_dispatch(int s) {   // deferred context, before a compute dispatch
    if (s < 0 || g_postFrames.load(std::memory_order_relaxed) <= 0 || g_plRec[s].size() >= 400) return;
    const Bound& b = g_bound[s];
    PRec p = pl_make(nullptr, b.csHash, nullptr, b.csSrv, kTrackSrv < 8 ? kTrackSrv : 8);
    p.cs = 1;
    ID3D11Resource* r = res_of(b.csUav[0]);
    p.rt = r; pl_desc(r, p.rw, p.rh, p.rf);
    p.rtOwner = static_cast<int8_t>(r ? owner_of(r) : -1);
    g_plRec[s].push_back(p);
}
void postlog_imm(ID3D11DeviceContext* c) {   // immediate context, before the draw
    if (g_postFrames.load(std::memory_order_relaxed) <= 0 || g_plFrame.size() >= 1500) return;
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    const bool isT = view_is_T(rtv);
    if (rtv && (!dsv || isT)) {
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
        c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr);
        ID3D11ShaderResourceView* srv[8] = {};
        c->PSGetShaderResources(0, 8, srv);
        PRec p = pl_make(vs, shaderid::hash_of(ps), rtv, srv, 8);
        p.replay = -1;
        g_plFrame.push_back(p);
        for (auto* v : srv) if (v) v->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
    }
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
}
void postlog_finish(int s, ID3D11CommandList* list) {
    if (s < 0) return;
    if (g_plRec[s].empty()) return;
    if (list && g_postFrames.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> lk(g_plMx);
        if (g_plPending.size() < 512) g_plPending[list] = std::move(g_plRec[s]);
    }
    g_plRec[s].clear();
}
void postlog_replay(ID3D11CommandList* list, int replay) {
    if (g_postFrames.load(std::memory_order_relaxed) <= 0) return;
    std::vector<PRec> v;
    {
        std::lock_guard<std::mutex> lk(g_plMx);
        auto it = g_plPending.find(list);
        if (it == g_plPending.end()) return;
        v = std::move(it->second);
        g_plPending.erase(it);
    }
    for (auto& p : v) { p.replay = replay; if (g_plFrame.size() < 1500) g_plFrame.push_back(p); }
}
void postlog_mark(uint8_t mark, int replay, int tag) {
    if (g_postFrames.load(std::memory_order_relaxed) <= 0 || g_plFrame.size() >= 1500) return;
    PRec p{}; p.mark = mark; p.replay = replay; p.tag = tag;
    g_plFrame.push_back(p);
}
void postlog_present() {   // Present thread = the immediate context's thread
    const int n = g_postFrames.load(std::memory_order_relaxed);
    if (n <= 0) return;
    static int frame = 0;
    std::vector<ID3D11Resource*> ids;   // short ids for resources, per frame
    auto id = [&](ID3D11Resource* r) -> int {
        if (!r) return -1;
        for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == r) return static_cast<int>(i);
        ids.push_back(r); return static_cast<int>(ids.size() - 1);
    };
    BVR_LOG("postlog frame %d: %zu entries (draws with no depth buffer or into the picture; GPU order)", frame, g_plFrame.size());
    for (const auto& p : g_plFrame) {
        if (p.mark == 1) { BVR_LOG("postlog   ---- replay #%d writes the picture (view tag %d)", p.replay, p.tag); continue; }
        if (p.mark == 2) { BVR_LOG("postlog   ==== LEFT EYE CAPTURED here (after replay #%d)", p.replay); continue; }
        const char* rto = p.isT ? "PICTURE" : p.rtOwner == 0 ? "game-view" : p.rtOwner == 1 ? "our-view" : "-";
        char where[24];
        if (p.replay < 0) sprintf_s(where, "immediate"); else sprintf_s(where, "replay #%d", p.replay);
        BVR_LOG("postlog   %-10s %s %016llx ps %016llx -> R%d %ux%u f%d %s <- R%d %ux%u f%d | reads %s |%s", where, p.cs ? "CS" : "vs", static_cast<unsigned long long>(p.vs), static_cast<unsigned long long>(p.ps),
                id(p.rt), p.rw, p.rh, p.rf, rto, id(p.t0), p.tw, p.th, p.tf,
                p.inBits == 1 ? "GAME view's own" : p.inBits == 2 ? "OUR view's own" : p.inBits == 3 ? "BOTH views'" : "shared", p.ins);
    }
    g_plFrame.clear();
    ++frame;
    if (g_postFrames.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lk(g_plMx);
        g_plPending.clear();
        BVR_LOG("postlog: done");
    }
}

// SUNPROBE (`ds sunprobe`, 2026-10-30): the god rays (geo-11 fix: "God-Rays. Real fix 2." = PS c34d41dd76ade6b1, a
// bright pass faded by the distance to the sun's place on screen, then radial blurs c94d9282 / e452e695 and the
// "Sun shafts" upsample b68f3b72 / a9700ac6) run once per view on the immediate context (POSTLOG 2026-10-30). Their
// constants come from the CPU. For one frame, every draw of those shaders has its vertex and pixel b0 read back; the
// two views' copies of each pass are compared register by register - the sun's screen position must differ between
// two eyes turned ~14 degrees apart.
// SKIPPS (`ds skipps <hash>` / `ds skipps 0`): drop every immediate-context draw of that pixel shader - capture with
// and without to see what the pass adds to each eye.
constexpr uint64_t kSunVs[] = {0xc94d9282ae84964aull, 0xb68f3b72bc36908cull, 0x832917f1c122b75bull,
                               0x3e287dd7bac197b6ull, 0xb716edfc7532156dull};   // + sun starburst source, radial blur
std::atomic<int> g_sunProbe{0};
std::atomic<uint64_t> g_skipPs{0};
std::atomic<uint32_t> g_skipped{0};
struct SunRec { uint64_t vs, ps; std::vector<float> vcb, pcb; };
std::vector<SunRec> g_sunRecs;
std::vector<float> read_cb(ID3D11DeviceContext* c, ID3D11Buffer* b) {
    std::vector<float> out;
    if (!b) return out;
    D3D11_BUFFER_DESC d{}; b->GetDesc(&d);
    if (!d.ByteWidth || d.ByteWidth > 65536) return out;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (!dev) return out;
    D3D11_BUFFER_DESC sd{}; sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr;
    if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &st)) && st) {
        c->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            out.assign(static_cast<const float*>(m.pData), static_cast<const float*>(m.pData) + d.ByteWidth / 4);
            c->Unmap(st, 0);
        }
        st->Release();
    }
    dev->Release();
    return out;
}
// RAYSCALE (`ds rayscale K`, 2026-10-30): at the VR picture shape the game builds the god-ray radial blur ~3x
// shorter than at 16:9 (vertex taps of VS b716edfc: last step 0.05 vs 0.15, same sun centre), so the rays never leave
// the sky. Test: stretch each tap away from its vertex by K before the draw (read back, rewritten into a buffer of ours).
std::atomic<float> g_rayScale{1.0f};
std::atomic<uint32_t> g_rayDraws{0};
ID3D11Buffer* g_rayVb = nullptr; UINT g_rayVbBytes = 0;
void ray_scale(ID3D11DeviceContext* c, float K) {
    ID3D11Buffer* vb = nullptr; UINT st = 0, of = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &st, &of);
    if (!vb) return;
    std::vector<float> v = read_cb(c, vb);
    D3D11_BUFFER_DESC d{}; vb->GetDesc(&d);
    vb->Release();
    const size_t sf = st / 4, f0 = of / 4;
    if (v.empty() || sf < 10 || f0 + sf * 3 > v.size()) return;
    for (int i = 0; i < 3; ++i) {
        float* q = &v[f0 + i * sf];
        const float px = q[2], py = q[3];          // the vertex's own coordinate
        for (int k = 0; k < 4; ++k) { q[4 + 2 * k] = px + K * (q[4 + 2 * k] - px); q[5 + 2 * k] = py + K * (q[5 + 2 * k] - py); }
    }
    if (!g_rayVb || g_rayVbBytes != d.ByteWidth) {
        if (g_rayVb) { g_rayVb->Release(); g_rayVb = nullptr; }
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        D3D11_BUFFER_DESC nd{}; nd.ByteWidth = d.ByteWidth; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (dev) { dev->CreateBuffer(&nd, nullptr, &g_rayVb); dev->Release(); }
        g_rayVbBytes = d.ByteWidth;
    }
    if (!g_rayVb) return;
    c->UpdateSubresource(g_rayVb, 0, nullptr, v.data(), 0, 0);
    c->IASetVertexBuffers(0, 1, &g_rayVb, &st, &of);
    g_rayDraws.fetch_add(1, std::memory_order_relaxed);
}
bool sun_tap(ID3D11DeviceContext* c) {   // immediate context, before the draw; true = drop it
    const uint64_t skip = g_skipPs.load(std::memory_order_relaxed);
    if (const float K = g_rayScale.load(std::memory_order_relaxed); K != 1.0f) {
        ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
        const bool ray = vs && shaderid::hash_of(vs) == 0xb716edfc7532156dull;
        if (vs) vs->Release();
        if (ray) ray_scale(c, K);
    }
    if (!skip && g_sunProbe.load(std::memory_order_relaxed) <= 0) return false;
    if (g_sunProbe.load(std::memory_order_relaxed) > 0) {   // CALLERS: who in the game issues the god-ray draws
        ID3D11PixelShader* p0 = nullptr; c->PSGetShader(&p0, nullptr, nullptr);
        const uint64_t h0 = shaderid::hash_of(p0);
        if (p0) p0->Release();
        static int logged = 0;
        if ((h0 == 0x2bacb17b4a9c2d6bull || h0 == 0x495b84e00370772bull || h0 == 0x94bde11cabdffdb1ull) && logged < 6) {
            ++logged;
            void* fr[24] = {};
            const USHORT n = RtlCaptureStackBackTrace(0, 24, fr, nullptr);
            const uintptr_t lo = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            char line[600]; int k = sprintf_s(line, "sunprobe   callers of ps %016llx (DarkSoulsIII.exe + offset, 0x140000000-based):", (unsigned long long)h0);
            for (USHORT i = 0; i < n && k < 560; ++i) {
                const uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
                if (a >= lo && a < lo + 0x4000000) k += sprintf_s(line + k, sizeof(line) - k, " %llX", (unsigned long long)(a - lo + 0x140000000ull));
            }
            BVR_LOG("%s", line);
        }
    }
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    const uint64_t ph = shaderid::hash_of(ps);
    if (ps) ps->Release();
    if (skip && ph == skip) { g_skipped.fetch_add(1, std::memory_order_relaxed); return true; }
    if (g_sunProbe.load(std::memory_order_relaxed) <= 0) return false;
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    const uint64_t vh = shaderid::hash_of(vs);
    if (vs) vs->Release();
    bool want = ph == 0xc34d41dd76ade6b1ull || ph == 0x94bde11cabdffdb1ull || ph == 0x2bacb17b4a9c2d6bull ||
                ph == 0x495b84e00370772bull || ph == 0xaea79c8ea3912aa4ull || ph == 0xb033b118d9ea8394ull ||   // the god-ray chain + composite
                // V12f: the light-shaft / radial-blur passes that read g_LightShaftScreenCenter (cb0[17]) / g_screenCenter
                // (cb0[7].xy), set on the CPU - per eye, or the first view's (or a centred lens's) sun position?
                ph == 0x6dabf1ba38657d75ull || ph == 0xcdf7b0e5c2dd5aacull || ph == 0xd23fe4b271c13c06ull || ph == 0xf232f31d78fb652full;
    for (uint64_t k : kSunVs) if (vh == k) want = true;
    if (!want || g_sunRecs.size() >= 64) return false;
    if (ph == 0xc34d41dd76ade6b1ull || ph == 0x94bde11cabdffdb1ull || ph == 0x2bacb17b4a9c2d6bull ||
        vh == 0xb716edfc7532156dull) {   // quads whose vertices carry the sun's screen position
        ID3D11Buffer* vb[3] = {}; UINT st[3] = {}, of[3] = {};
        c->IAGetVertexBuffers(0, 3, vb, st, of);
        for (int i = 0; i < 3; ++i) {
            if (!vb[i]) continue;
            std::vector<float> v = read_cb(c, vb[i]);
            char line[900]; int k = sprintf_s(line, "sunprobe   ps %016llx vs %016llx vertex stream %d stride %u offset %u (%zu floats):",
                                              (unsigned long long)ph, (unsigned long long)vh, i, st[i], of[i], v.size());
            const size_t first = of[i] / 4, n = (st[i] / 4) * 3;   // three vertices (the full-screen triangle)
            for (size_t q = 0; q < n && first + q < v.size() && k < 860; ++q) k += sprintf_s(line + k, sizeof(line) - k, " %.4f", v[first + q]);
            BVR_LOG("%s", line);
            vb[i]->Release();
        }
    }
    ID3D11Buffer* vb = nullptr; ID3D11Buffer* pb = nullptr;
    c->VSGetConstantBuffers(0, 1, &vb); c->PSGetConstantBuffers(0, 1, &pb);
    g_sunRecs.push_back({vh, ph, read_cb(c, vb), read_cb(c, pb)});
    if (vb) vb->Release();
    if (pb) pb->Release();
    return false;
}
void sun_present() {
    if (g_sunProbe.load(std::memory_order_relaxed) <= 0) return;
    if (g_sunProbe.fetch_sub(1) != 1) { g_sunRecs.clear(); return; }   // the LAST armed frame is the one logged
    // Pair each pass of the first view's chain with the same pass of the second's (same shaders, same order).
    std::vector<int> used(g_sunRecs.size(), 0);
    BVR_LOG("sunprobe: %zu god-ray draws this frame", g_sunRecs.size());
    for (size_t i = 0; i < g_sunRecs.size(); ++i) {
        if (used[i]) continue;
        const SunRec& a = g_sunRecs[i];
        size_t j = i + 1;
        int skip = 0;
        // the partner = the (k+1)-th later draw of the same pair of shaders, where k = how many of them came before i
        for (size_t q = 0; q < i; ++q) if (g_sunRecs[q].vs == a.vs && g_sunRecs[q].ps == a.ps && !used[q]) ++skip;
        for (j = i + 1; j < g_sunRecs.size(); ++j)
            if (!used[j] && g_sunRecs[j].vs == a.vs && g_sunRecs[j].ps == a.ps) { if (skip-- == 0) break; }
        used[i] = 1;
        const bool solo = j >= g_sunRecs.size();
        if (solo) BVR_LOG("sunprobe   vs %016llx ps %016llx (VS b0 / PS b0): no partner", (unsigned long long)a.vs, (unsigned long long)a.ps);
        else used[j] = 1;
        const SunRec& b = solo ? a : g_sunRecs[j];
        for (int stage = 0; stage < 2; ++stage) {
            const std::vector<float>& x = stage ? a.pcb : a.vcb;
            const std::vector<float>& y = stage ? b.pcb : b.vcb;
            int diffs = 0;
            const size_t n = x.size() < y.size() ? x.size() : y.size();
            for (size_t r = 0; r + 3 < n; r += 4) {
                bool d = false;
                for (int k = 0; k < 4; ++k) if (fabsf(x[r + k] - y[r + k]) > 1e-5f) d = true;
                if (!d) continue;
                if (++diffs <= 10)
                    BVR_LOG("sunprobe   vs %016llx ps %016llx %s c%zu: first view (%.5f %.5f %.5f %.5f) second (%.5f %.5f %.5f %.5f)",
                            (unsigned long long)a.vs, (unsigned long long)a.ps, stage ? "PS" : "VS", r / 4, x[r], x[r + 1], x[r + 2], x[r + 3],
                            y[r], y[r + 1], y[r + 2], y[r + 3]);
            }
            BVR_LOG("sunprobe   vs %016llx ps %016llx %s b0: %zu bytes, %d registers differ between the views%s",
                    (unsigned long long)a.vs, (unsigned long long)a.ps, stage ? "PS" : "VS", n * 4, diffs,
                    diffs ? "" : "  <- IDENTICAL in both eyes");
        }
        // the first view's NON-ZERO registers (all of them, a few per line), to find the sun's screen position
        for (int stage = 0; stage < 2; ++stage) {
            const std::vector<float>& x = stage ? a.pcb : a.vcb;
            char line[600]; int k = 0;
            for (size_t r = 0; r * 4 + 3 < x.size(); ++r) {
                if (!(x[r * 4] || x[r * 4 + 1] || x[r * 4 + 2] || x[r * 4 + 3])) continue;
                k += sprintf_s(line + k, sizeof(line) - k, " c%zu(%.4f %.4f %.4f %.4f)", r, x[r * 4], x[r * 4 + 1], x[r * 4 + 2], x[r * 4 + 3]);
                if (k > 480) { BVR_LOG("sunprobe     %s first view:%s", stage ? "PS" : "VS", line); k = 0; }
            }
            if (k) BVR_LOG("sunprobe     %s first view:%s", stage ? "PS" : "VS", line);
        }
    }
    g_sunRecs.clear();
}

// SHAFTPROBE (`ds shaftprobe N`, 2026-10-30): for N frames, every draw of a pixel shader that uses the light-shaft
// constants (shaderid SHAFTS) - per shader: how many draws, into what size of target, and the constant block it binds at
// b0, read back at the frame's end (the effects' common block is written once a frame).
struct ShaftRow { uint64_t ps; uint32_t n; UINT rw, rh; ID3D11Buffer* cb; };
std::atomic<int> g_shaftFrames{0};
std::mutex g_shaftMx2;
std::vector<ShaftRow> g_shaftRows;
std::atomic<uint32_t> g_shaftSeen{0};
void shaft_note(const void* ps, uint64_t psHash, ID3D11RenderTargetView* rtv, ID3D11Buffer* cb) {
    if (g_shaftFrames.load(std::memory_order_relaxed) <= 0 || !shaderid::is_shaft_ps(ps)) return;
    g_shaftSeen.fetch_add(1, std::memory_order_relaxed);
    UINT w = 0, h = 0; int f = 0;
    if (rtv) { ID3D11Resource* r = nullptr; rtv->GetResource(&r); pl_desc(r, w, h, f); if (r) r->Release(); }
    std::lock_guard<std::mutex> lk(g_shaftMx2);
    for (auto& row : g_shaftRows) if (row.ps == psHash && row.rw == w) { ++row.n; row.cb = cb ? cb : row.cb; return; }
    if (g_shaftRows.size() < 64) g_shaftRows.push_back({psHash, 1, w, h, cb});
}
void shaft_present(ID3D11DeviceContext* imm) {
    const int n = g_shaftFrames.load(std::memory_order_relaxed);
    if (n <= 0) return;
    if (g_shaftFrames.fetch_sub(1) != 1) return;
    std::lock_guard<std::mutex> lk(g_shaftMx2);
    BVR_LOG("shaftprobe: %u light-shaft draws in the armed frames, %zu kinds", g_shaftSeen.exchange(0), g_shaftRows.size());
    for (const auto& row : g_shaftRows) {
        std::vector<float> c = imm ? read_cb(imm, row.cb) : std::vector<float>();
        auto reg = [&](int r, float* o) { for (int k = 0; k < 4; ++k) o[k] = static_cast<size_t>(r * 4 + k) < c.size() ? c[r * 4 + k] : -999.f; };
        float a[4], b[4], d[4], e[4], g[4];
        reg(17, a); reg(31, b); reg(32, d); reg(33, e); reg(0, g);
        BVR_LOG("shaftprobe   ps %016llx x%u into %ux%u | b0 %zu bytes: c17 screen centre (%.3f %.3f %.3f %.3f) c31 dir (%.3f %.3f %.3f %.3f) "
                "c32 (%.3f %.3f %.3f %.3f) c33 (%.3f %.3f %.3f %.3f) c0 colour (%.3f %.3f %.3f %.3f)",
                static_cast<unsigned long long>(row.ps), row.n, row.rw, row.rh, c.size() * 4, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3],
                d[0], d[1], d[2], d[3], e[0], e[1], e[2], e[3], g[0], g[1], g[2], g[3]);
    }
    g_shaftRows.clear();
}

// HUD pieces that belong ON a world object stay out of the sheet (JJ 2026-10-30: "HUD elements that are attached to
// world objects need to be at those objects' depth"): the geo-11 fix names them by the texture at PS t0 -
// [TextureOverrideLockOn1-5], [TextureOverrideHUD1EnemyBars1] and the bow's [TextureOverrideCrosshair1], all of which
// it moves to the scene depth (its HUD vertex shaders, filter_index 1-3). They stay in the picture for now (M8 markers).
// The filter only works inside the fix's four HUD shaders: its batched Scaleform shader (c73569859d992da5) draws the
// souls counter's frame from the same "enemy bars" texture and the fix keeps that flat (hudtex 2026-10-30), so a piece is
// on-object only when one of the four (kind 1) draws it.
constexpr uint32_t kHudWorldTex[] = {0x068a476e, 0xfee34e47, 0x3f078170, 0x3971a8ac, 0x0b874300,   // lock-on
                                     0xcaa9c961,                                                    // enemy bars
                                     0x63176f83};                                                   // crosshair
std::atomic<uint32_t> g_hudWorld{0}, g_hudImm{0}, g_hudInst{0};
std::atomic<uint32_t> g_hudClearedAt{~0u}, g_presentSeq{0}, g_hudSeenAt{0};
std::atomic<int> g_hudWorldKeep{0};
bool hud_world(ID3D11ShaderResourceView* srv) {
    if (!srv) return false;
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    uint32_t h = 0;
    const bool named = r && shaderid::tex_hash(r, &h);
    if (r) r->Release();
    if (named) for (uint32_t k : kHudWorldTex) if (h == k) return true;
    return false;
}
// HUDTEX (`ds hudtex N`): for N frames, every distinct (vertex shader, PS t0 texture) drawn into the shared picture -
// to name pieces the 2016 fix's lists may not cover in this game version.
struct HudTexRow { uint64_t vs; uint32_t tex; UINT w, h; uint8_t kind, world, imm; uint32_t n; };
std::mutex g_hudTexMx;
std::vector<HudTexRow> g_hudTexRows;
std::atomic<int> g_hudTexFrames{0};
// TEXDUMP (`ds texdump <hex> ...`, 2026-11-01, the bonfire haze): the next time a HUD piece draws with one of these texture
// hashes, its view is kept and saved at the Present as a picture in DS3VR2\captures (drawn into an 8-bit target first -
// HUD textures are usually block-compressed), so the haze can be named by looking at it, not by more rests at a bonfire.
constexpr int kTexDump = 8;
std::atomic<int> g_texDumpN{0};
uint32_t g_texDumpHash[kTexDump] = {};
std::atomic<ID3D11ShaderResourceView*> g_texDumpSrv[kTexDump] = {};
void texdump_note(ID3D11ShaderResourceView* srv, uint32_t th) {
    for (int i = 0; i < g_texDumpN.load(std::memory_order_relaxed); ++i)
        if (g_texDumpHash[i] == th && !g_texDumpSrv[i].load(std::memory_order_relaxed)) {
            ID3D11ShaderResourceView* e = nullptr;
            if (g_texDumpSrv[i].compare_exchange_strong(e, srv)) srv->AddRef();
        }
}
void hudtex_note(ID3D11VertexShader* vs, ID3D11ShaderResourceView* srv, uint8_t kind, bool world, bool imm) {
    uint32_t th = 0; UINT w = 0, h = 0;
    if (srv) {
        ID3D11Resource* r = nullptr;
        srv->GetResource(&r);
        if (r) {
            shaderid::tex_hash(r, &th);
            D3D11_RESOURCE_DIMENSION d{};
            r->GetType(&d);
            if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td{};
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
                w = td.Width; h = td.Height;
            }
            r->Release();
        }
        if (th && g_texDumpN.load(std::memory_order_relaxed)) texdump_note(srv, th);
    }
    if (g_hudTexFrames.load(std::memory_order_relaxed) <= 0) return;
    const uint64_t vh = shaderid::hash_of(vs);
    std::lock_guard<std::mutex> lk(g_hudTexMx);
    for (auto& row : g_hudTexRows) if (row.vs == vh && row.tex == th && row.w == w && row.imm == static_cast<uint8_t>(imm)) { ++row.n; return; }
    if (g_hudTexRows.size() < 256)
        g_hudTexRows.push_back({vh, th, w, h, kind, static_cast<uint8_t>(world), static_cast<uint8_t>(imm), 1});
}
// One clear of the sheet per frame, by whichever context draws the frame's first HUD piece.
bool hud_clear_due() {
    const uint32_t f = g_presentSeq.load(std::memory_order_relaxed);
    g_hudSeenAt.store(f, std::memory_order_relaxed);
    return g_hudClearedAt.exchange(f, std::memory_order_relaxed) != f;
}
void hud_clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
    static const float kZero[4] = {0, 0, 0, 0};
    c->ClearRenderTargetView(rtv, kZero);
    c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    g_hudLists.fetch_add(1, std::memory_order_relaxed);
}

// Before a draw is recorded: send it to our sheet if it is a flat HUD draw into the shared picture, and put the
// game's own binding back before the first other draw that follows. Only while both views are listed (3D gameplay):
// at the title and on loading screens the picture is shown on the room screen WITH its menus, so nothing is moved.
void hud_redirect(ID3D11DeviceContext* c, int s, bool inst = false) {
    if (s < 0 || !g_hudRedirect.load(std::memory_order_relaxed) || !g_hudOn.load(std::memory_order_relaxed)) return;
    ID3D11RenderTargetView* rtv = g_myHudRtv.load(std::memory_order_relaxed);
    ID3D11DepthStencilView* dsv = g_myHudDsv.load(std::memory_order_relaxed);
    if (!rtv || !dsv) return;
    Bound& b = g_bound[s];
    const bool toT = g_hudBound[s] || view_is_T(b.rtv0);   // while ours is bound, b.rtv0 still holds the game's target
    if (!toT) return;
    const uint8_t kind = hud_vs_kind(b.vs);
    const bool world = kind == 1 && hud_world(b.psSrv[0]);
    if (g_hudTexFrames.load(std::memory_order_relaxed) > 0 || g_texDumpN.load(std::memory_order_relaxed)) hudtex_note(b.vs, b.psSrv[0], kind, world, false);
    if (kind && !world) {
        if (!g_hudBound[s]) {
            if (hud_clear_due()) hud_clear(c, rtv, dsv);   // the frame's first HUD piece: start on a clear sheet
            oSetRT(c, 1, &rtv, dsv);
            g_hudBound[s] = true;
        }
        g_hudRedirected.fetch_add(1, std::memory_order_relaxed);
        (kind == 1 ? g_hudByHash : g_hudBySig).fetch_add(1, std::memory_order_relaxed);
        if (inst) g_hudInst.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (world) g_hudWorld.fetch_add(1, std::memory_order_relaxed);
    if (g_hudBound[s]) {
        oSetRT(c, 4, b.rtvs, b.dsv);     // the game's own binding, as it last set it
        g_hudBound[s] = false;
    }
}

// The same on the IMMEDIATE context (other code than the deferred one, so its draws reach us through the frame
// inspector's draw taps, not our hooks). Its bound state is read back with the Get calls; the game's binding is kept
// (AddRef'd) while ours is in place.
struct ImmHud { bool bound = false; ID3D11RenderTargetView* rtv[4] = {}; ID3D11DepthStencilView* dsv = nullptr; };
ImmHud g_imm;
thread_local bool t_ownSet = false;
// V12i (crash 16:17 with PDB: hud_redirect_imm -> OMSetRenderTargets): our OWN draws inside a HUD draw (the marker
// layers, the flat redraws) went back through the draw / render-target hooks - imm_rt_changed then forgot the game's saved
// targets mid-redirect and the tap re-ran on our draws (lock-on and weapon icon flicker, then a freed target bound).
thread_local int t_ourDraw = 0;
void imm_forget() {
    for (auto& r : g_imm.rtv) if (r) { r->Release(); r = nullptr; }
    if (g_imm.dsv) { g_imm.dsv->Release(); g_imm.dsv = nullptr; }
    g_imm.bound = false;
}
void imm_restore(ID3D11DeviceContext* c) {
    if (!g_imm.bound) return;
    t_ownSet = true;
    c->OMSetRenderTargets(4, g_imm.rtv, g_imm.dsv);
    t_ownSet = false;
    imm_forget();
}
void imm_rt_changed() { if (!t_ourDraw && !t_ownSet && g_imm.bound) imm_forget(); }   // the game bound its own targets: ours is gone
// AFW (V9, JJ: "AFW is looking great except for the HUD"): the HUD sheet was tied to g_pair (both views listed), so in AFW
// the HUD stayed in the drawn eye's picture and was warped with the scene. Its own switch: stereo gameplay, native or AFW.
thread_local bool g_immSkip = false;
thread_local ID3D11Buffer* t_flatOver = nullptr;
thread_local ID3D11GeometryShader* t_flatGs = nullptr;   // MKREGION (V12g)
thread_local ID3D11Buffer* t_flatGsCb = nullptr;   // BATCH: this draw's flat copy of b0 (marker pieces collapsed)   // M8: hud_redirect_imm placed this draw as a marker card
void hud_redirect_imm(ID3D11DeviceContext* c) {
    if (!g_hudRedirect.load(std::memory_order_relaxed) || !g_hudOn.load(std::memory_order_relaxed)) { imm_restore(c); return; }
    ID3D11RenderTargetView* rtv = g_myHudRtv.load(std::memory_order_relaxed);
    ID3D11DepthStencilView* dsv = g_myHudDsv.load(std::memory_order_relaxed);
    if (!rtv || !dsv) return;
    bool toT = g_imm.bound;
    if (!toT) {
        ID3D11RenderTargetView* cur = nullptr;
        c->OMGetRenderTargets(1, &cur, nullptr);
        toT = view_is_T(cur);
        if (cur) cur->Release();
    }
    if (!toT) return;
    ID3D11VertexShader* vs = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    const uint8_t kind = hud_vs_kind(vs);
    ID3D11ShaderResourceView* srv = nullptr;
    c->PSGetShaderResources(0, 1, &srv);
    // V12o (JJ: the bar / lock-on "popping between the 3D and the 2D HUD"): pieces named by the geo-11 fix's on-object texture
    // list (lock-on, enemy bars) were kept in the PICTURE - flat, at their screen spot, ~4 draws a frame - from before the
    // marker cards worked. With the cards on, they go the sheet way like every other piece (their marker triangles are on
    // the card, the rest is dropped). `ds hudworld 1` = the old behaviour.
    const bool world = kind == 1 && g_hudWorldKeep.load(std::memory_order_relaxed) && hud_world(srv);
    if (g_hudTexFrames.load(std::memory_order_relaxed) > 0 || g_texDumpN.load(std::memory_order_relaxed)) hudtex_note(vs, srv, kind, world, true);
    if (srv) srv->Release();
    if (vs) vs->Release();   // still bound: the pointer stays valid as a key
    // M8: a HUD piece that belongs to a world marker is drawn on its object in each eye's marker layer instead (the
    // game's own draw is then skipped). DS3 draws its whole HUD on the IMMEDIATE context (hudsheet: every piece), where
    // the deferred-context path never sees it.
    if (kind && g_hudDraw) {
        ID3D11Buffer* cb = nullptr; UINT first = 0, num = 0;
        ID3D11DeviceContext1* c1 = nullptr;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {
            c1->VSGetConstantBuffers1(0, 1, &cb, &first, &num);
            c1->Release();
        } else {
            c->VSGetConstantBuffers(0, 1, &cb);
        }
        const float* cbData = cb ? gfx_of(cb, first) : nullptr;
        const unsigned cbRows = cb ? gfx_rows(cb, first) : 0;
        if (cb) cb->Release();   // still bound
        if (cbData) {
            bool indexed = false; unsigned count = 0, start = 0, inst = 0; int base = 0;
            bvr::frame_inspector::current_draw(indexed, count, start, base, inst);
            g_hudDraws.fetch_add(1, std::memory_order_relaxed);
            ++t_ourDraw;
            const bool tookIt = g_hudDraw(c, vs, cbData, cbRows, indexed, count, start, base, inst);
            --t_ourDraw;
            if (tookIt) {
                g_hudTaken.fetch_add(1, std::memory_order_relaxed);
                g_immSkip = true;   // drawn on its object: the game's flat draw is skipped
                return;
            }
            t_flatOver = bvr::ds3::markers::take_flat_override();   // BATCH: the flat draw keeps only the non-marker pieces
            bvr::ds3::markers::take_flat_gs(&t_flatGs, &t_flatGsCb);   // MKREGION: ... or only the triangles of no marker
        } else {
            g_hudNoCb.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (kind && !world) {
        if (!g_imm.bound) {
            c->OMGetRenderTargets(4, g_imm.rtv, &g_imm.dsv);
            if (hud_clear_due()) hud_clear(c, rtv, dsv);
            t_ownSet = true;
            c->OMSetRenderTargets(1, &rtv, dsv);
            t_ownSet = false;
            g_imm.bound = true;
        }
        g_hudRedirected.fetch_add(1, std::memory_order_relaxed);
        g_hudImm.fetch_add(1, std::memory_order_relaxed);
        (kind == 1 ? g_hudByHash : g_hudBySig).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (world) g_hudWorld.fetch_add(1, std::memory_order_relaxed);
    imm_restore(c);
}
std::atomic<int> g_boxFrames{0};   // BOXPROBE (defined with GetData below)
void box_note(ID3D11DeviceContext* c, bool imm);
uintptr_t exe_lo();
// HUDHIDE (`ds hudhide WxH`, 2026-11-01, the bonfire haze): drop the immediate-context HUD pieces whose pixel-shader t0
// texture is exactly W x H (0x0 = no texture) - a live "is it this piece?" test. `ds hudhide off`.
std::atomic<uint32_t> g_hideW{0}, g_hideH{0}, g_hideOn{0}, g_hidden{0};
// HUDHIDE2 / HUDFADE (2026-11-01, JJ: "when I talk to characters there's a vignette at the bottom of the HUD sheet where the
// subtitles are" / "when traveling there's a black window that appears before the loading screen"). Pieces going onto OUR
// HUD sheet (3D only - room-screen menus and loading screens are never touched) whose PS t0 texture has one of these
// 3Dmigoto hashes are not drawn: HIDE = dropped (the subtitle backdrop, Sekiro's was a 256x1 gradient - SEKIRO_PLAN F8
// 01:12:21); FADE = dropped AND counted as "the game is fading", which stereo turns into the full-view fade layer instead
// of a 16:9 dark box on the panel (Sekiro FADEVIEW, memory game-fade-lands-on-hud-panel). Saved as hud_hide_0..7 /
// hud_fade_0..7 (`ds hudhide <hex>` / `ds hudfade <hex>`, `... clear`).
constexpr int kTexList = 8;
std::atomic<uint32_t> g_hideTex[kTexList] = {}, g_fadeTex[kTexList] = {};
std::atomic<int> g_hideTexN{0}, g_fadeTexN{0}, g_texListsLoaded{0};
std::atomic<uint32_t> g_hiddenByTex{0}, g_fadeTaken{0};
std::atomic<ULONGLONG> g_fadeSeenMs{0};
// HUDHIDEVS (N2, JJ 11:27: hiding EVERY 256x1 gradient piece removed the subtitle vignette - too blunt to keep, the HUD uses
// those gradients elsewhere). DS3 makes the gradients at run time, so they have no texture hash: the rule is (vertex shader,
// texture size). `ds hudhide vs <16 hex> <W>x<H>` (saved hud_hidevs_N / _w / _h, the shader hash folded to 32 bits).
std::atomic<uint32_t> g_hideVs[kTexList] = {}, g_hideVsW[kTexList] = {}, g_hideVsH[kTexList] = {};
std::atomic<int> g_hideVsN{0};
std::atomic<uint32_t> g_hiddenByVs{0};
uint32_t fold(uint64_t h) { return static_cast<uint32_t>(h ^ (h >> 32)); }
void tex_lists_load() {
    if (g_texListsLoaded.exchange(1)) return;
    char k[32];
    for (int i = 0; i < kTexList; ++i) {
        snprintf(k, sizeof(k), "hud_hide_%d", i); if (const int v = settings::i(k, 0)) g_hideTex[g_hideTexN++].store(static_cast<uint32_t>(v));
        snprintf(k, sizeof(k), "hud_fade_%d", i); if (const int v = settings::i(k, 0)) g_fadeTex[g_fadeTexN++].store(static_cast<uint32_t>(v));
        snprintf(k, sizeof(k), "hud_hidevs_%d", i);
        if (const int v = settings::i(k, 0)) {
            const int j = g_hideVsN++;
            g_hideVs[j].store(static_cast<uint32_t>(v));
            snprintf(k, sizeof(k), "hud_hidevs_%d_w", i); g_hideVsW[j].store(static_cast<uint32_t>(settings::i(k, 0)));
            snprintf(k, sizeof(k), "hud_hidevs_%d_h", i); g_hideVsH[j].store(static_cast<uint32_t>(settings::i(k, 0)));
        }
    }
    if (g_hideVsN) BVR_LOG("hudhide: %d (shader, texture size) rule(s) from settings", g_hideVsN.load());
    if (g_hideTexN || g_fadeTexN) BVR_LOG("hudhide: %d texture(s) hidden on the HUD panel, %d taken as the game's fade (from settings)", g_hideTexN.load(), g_fadeTexN.load());
}
bool in_list(const std::atomic<uint32_t>* l, int n, uint32_t h) { for (int i = 0; i < n; ++i) if (l[i].load(std::memory_order_relaxed) == h) return true; return false; }
bool hud_hide_tex(ID3D11DeviceContext* c) {
    tex_lists_load();
    const int nh = g_hideTexN.load(std::memory_order_relaxed), nf = g_fadeTexN.load(std::memory_order_relaxed);
    const int nv = g_hideVsN.load(std::memory_order_relaxed);
    if (!nh && !nf && !nv) return false;
    ID3D11RenderTargetView* rtv = nullptr; c->OMGetRenderTargets(1, &rtv, nullptr);
    const bool sheet = rtv && rtv == g_myHudRtv.load(std::memory_order_relaxed);
    if (rtv) rtv->Release();
    if (!sheet) return false;
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    uint32_t th = 0; UINT tw = 0, tht = 0;
    if (v) {
        ID3D11Resource* r = nullptr; v->GetResource(&r);
        if (r) {
            shaderid::tex_hash(r, &th);
            D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); tw = td.Width; tht = td.Height; }
            r->Release();
        }
        v->Release();
    }
    if (nv && tw) {
        ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
        const uint32_t vf = vs ? fold(shaderid::hash_of(vs)) : 0;
        if (vs) vs->Release();
        for (int i = 0; i < nv; ++i)
            if (g_hideVs[i].load(std::memory_order_relaxed) == vf && g_hideVsW[i].load(std::memory_order_relaxed) == tw &&
                g_hideVsH[i].load(std::memory_order_relaxed) == tht) { g_hiddenByVs.fetch_add(1, std::memory_order_relaxed); return true; }
    }
    if (!th) return false;
    if (nh && in_list(g_hideTex, nh, th)) { g_hiddenByTex.fetch_add(1, std::memory_order_relaxed); return true; }
    if (nf && in_list(g_fadeTex, nf, th)) { g_fadeTaken.fetch_add(1, std::memory_order_relaxed); g_fadeSeenMs.store(GetTickCount64(), std::memory_order_relaxed); return true; }
    return false;
}
bool hud_hide(ID3D11DeviceContext* c) {
    if (hud_hide_tex(c)) return true;
    if (!g_hideOn.load(std::memory_order_relaxed)) return false;
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    UINT w = 0, h = 0;
    if (v) {
        ID3D11Resource* r = nullptr; v->GetResource(&r);
        D3D11_RESOURCE_DIMENSION d{}; if (r) r->GetType(&d);
        if (r && d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); w = td.Width; h = td.Height; }
        if (r) r->Release();
        v->Release();
    }
    if (w == g_hideW.load(std::memory_order_relaxed) && h == g_hideH.load(std::memory_order_relaxed)) { g_hidden.fetch_add(1, std::memory_order_relaxed); return true; }
    return false;
}
// HAZEFILL (2026-11-01, JJ: resting at a bonfire shows a hazy smoke "inside a window which rotates with your head"; in the
// flat game it covers the whole screen). Eye-burst 00:53:55: the GAME's own eye picture already holds the smoke as a box
// - full width, ~9/16 of the width tall - drawn by a Scaleform pass (vs c73569859d992da5, ~6 s per rest, postlog) into the
// eye picture: a 16:9 stage fitted to the WIDTH of our near-square eye (memory scaleform-stage-fills-width-only). For that
// pass into an eye-sized target (never our HUD sheet) the viewport is scaled about its centre so the 16:9 band covers the
// whole eye (sides cropped - soft smoke); scissor opened to the target; both restored after the draw.
// `ds hazefill 0|1|<scale>` (settings haze_fill, default 1 = auto scale H / (W * 9/16)).
constexpr uint64_t kHazeVs = 0xc73569859d992da5ull;
std::atomic<float> g_hazeFill{0.0f};   // 0 off, 1 auto, > 1 that scale (V13z8: off - this vs is the loading screen)
std::atomic<uint32_t> g_hazeHits{0}, g_hazeLogged{0};
thread_local bool t_hazeVp = false, t_hazeSc = false;
thread_local D3D11_VIEWPORT t_hazeOldVp{};
thread_local D3D11_RECT t_hazeOldSc{};
// FESIGNAL (V13z8, 2026-11-01): the hazefill log fired DURING THE LOADING SCREEN (01:01:29) - this vertex shader draws the
// game's full-screen Scaleform screens (the loading screen) into the picture. Counted as a hook-free "loading screen drawn"
// signal (the movie-draw hook is not possible on DS3: its code is never executable where we see it).
std::atomic<ULONGLONG> g_feMs{0};
std::atomic<uint32_t> g_feDraws{0}, g_feLogged{0};
void haze_before(ID3D11DeviceContext* c) {
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    const bool hit = vs && shaderid::hash_of(vs) == kHazeVs;
    if (vs) vs->Release();
    if (!hit) return;
    ID3D11RenderTargetView* rtv = nullptr; c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    const bool sheet = rtv == g_myHudRtv.load(std::memory_order_relaxed);
    D3D11_TEXTURE2D_DESC td{};
    { ID3D11Resource* r = nullptr; rtv->GetResource(&r); if (r) { static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); r->Release(); } }
    rtv->Release();
    if (sheet || !td.Width || td.Height < td.Width * 0.8f) return;   // an eye picture (near-square), not the HUD sheet
    g_feMs.store(GetTickCount64(), std::memory_order_relaxed);       // FESIGNAL
    g_feDraws.fetch_add(1, std::memory_order_relaxed);
    if (g_feLogged.fetch_add(1) < 3) {
        ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
        BVR_LOG("fesignal: a full-screen Scaleform screen drawn into the %ux%u picture (ps %016llx)", td.Width, td.Height,
                (unsigned long long)shaderid::hash_of(ps));
        if (ps) ps->Release();
    }
    const float f = g_hazeFill.load(std::memory_order_relaxed);
    if (f <= 0.0f) return;
    D3D11_VIEWPORT vp{}; UINT n = 1; c->RSGetViewports(&n, &vp);
    if (n != 1 || vp.Width < td.Width * 0.95f) return;               // the pass covers the whole target
    const float k = f > 1.0f ? f : vp.Height / (vp.Width * 9.0f / 16.0f);
    if (k <= 1.01f) return;
    t_hazeOldVp = vp;
    D3D11_VIEWPORT nv = vp;
    nv.Width = vp.Width * k; nv.Height = vp.Height * k;
    nv.TopLeftX = vp.TopLeftX + 0.5f * (vp.Width - nv.Width); nv.TopLeftY = vp.TopLeftY + 0.5f * (vp.Height - nv.Height);
    bvr::frame_inspector::raw_viewports()(c, 1, &nv);
    t_hazeVp = true;
    ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs);
    D3D11_RASTERIZER_DESC rd{}; if (rs) { rs->GetDesc(&rd); rs->Release(); }
    if (rd.ScissorEnable) {
        UINT ns = 1; c->RSGetScissorRects(&ns, &t_hazeOldSc);
        const D3D11_RECT full{0, 0, static_cast<LONG>(td.Width), static_cast<LONG>(td.Height)};
        c->RSSetScissorRects(1, &full);
        t_hazeSc = true;
    }
    g_hazeHits.fetch_add(1, std::memory_order_relaxed);
    if (g_hazeLogged.fetch_add(1) < 4)
        BVR_LOG("hazefill: the smoke pass into a %ux%u eye picture - viewport %.0fx%.0f scaled x%.2f%s", td.Width, td.Height, vp.Width,
                vp.Height, k, rd.ScissorEnable ? " (scissor opened)" : "");
}
void haze_after(ID3D11DeviceContext* c) {
    if (t_hazeVp) { bvr::frame_inspector::raw_viewports()(c, 1, &t_hazeOldVp); t_hazeVp = false; }
    if (t_hazeSc) { c->RSSetScissorRects(1, &t_hazeOldSc); t_hazeSc = false; }
}
bool hud_imm_draw_tap(ID3D11DeviceContext* c, unsigned) {
    if (t_ourDraw) return false;   // V12i: one of our own draws - not the game's
    if (hud_hide(c)) return true;  // HUDHIDE
    if (g_boxFrames.load(std::memory_order_relaxed) > 0) {   // BOXPROBE on the immediate context
        bool ix = false; unsigned n = 0, st = 0, inst = 0; int base = 0;
        bvr::frame_inspector::current_draw(ix, n, st, base, inst);
        if (ix && n == 14 && st == 0 && base == 0) box_note(c, true);
    }
    if (sun_tap(c)) return true;   // SKIPPS
    if (g_shaftFrames.load(std::memory_order_relaxed) > 0) {   // SHAFTPROBE on the immediate context
        ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
        if (ps && shaderid::is_shaft_ps(ps)) {
            ID3D11RenderTargetView* rtv = nullptr; ID3D11Buffer* cb = nullptr;
            c->OMGetRenderTargets(1, &rtv, nullptr); c->PSGetConstantBuffers(0, 1, &cb);
            shaft_note(ps, shaderid::hash_of(ps), rtv, cb);
            if (rtv) rtv->Release();
            if (cb) cb->Release();   // still bound: kept only as an identity until this frame's read-back
        }
        if (ps) ps->Release();
    }
    postlog_imm(c);
    g_immSkip = false;
    t_flatOver = nullptr;
    t_flatGs = nullptr; t_flatGsCb = nullptr;
    hud_redirect_imm(c);
    if (!g_immSkip && t_flatGs && t_flatGsCb) {
        // MKREGION: the flat side - the game's draw (to wherever hud_redirect_imm sent it) through the GS that leaves out every
        // triangle now on a marker's card; then the game's own geometry stage goes back
        ID3D11GeometryShader* oldGs = nullptr; ID3D11Buffer* oldCb = nullptr;
        c->GSGetShader(&oldGs, nullptr, nullptr);
        c->GSGetConstantBuffers(13, 1, &oldCb);
        c->GSSetShader(t_flatGs, nullptr, 0);
        c->GSSetConstantBuffers(13, 1, &t_flatGsCb);
        bool indexed = false; unsigned count = 0, start = 0, inst = 0; int base = 0;
        bvr::frame_inspector::current_draw(indexed, count, start, base, inst);
        ++t_ourDraw;
        if (indexed) { if (inst) c->DrawIndexedInstanced(count, inst, start, base, 0); else c->DrawIndexed(count, start, base); }
        else { if (inst) c->DrawInstanced(count, inst, start, 0); else c->Draw(count, start); }
        --t_ourDraw;
        c->GSSetConstantBuffers(13, 1, &oldCb);
        c->GSSetShader(oldGs, nullptr, 0);
        if (oldCb) oldCb->Release();
        if (oldGs) oldGs->Release();
        g_immSkip = true;
        t_flatGs = nullptr; t_flatGsCb = nullptr;
        t_flatOver = nullptr;
    }
    if (!g_immSkip && t_flatOver) {
        // BATCH: some pieces of this batch were drawn on their objects - the flat draw (wherever hud_redirect_imm sent it)
        // runs with those pieces collapsed, then the game's own constants go back
        ID3D11DeviceContext1* c1 = nullptr;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {
            ID3D11Buffer* old = nullptr; UINT f = 0, n = 0;
            c1->VSGetConstantBuffers1(0, 1, &old, &f, &n);
            c->VSSetConstantBuffers(0, 1, &t_flatOver);
            bool indexed = false; unsigned count = 0, start = 0, inst = 0; int base = 0;
            bvr::frame_inspector::current_draw(indexed, count, start, base, inst);
            ++t_ourDraw;
            if (indexed) { if (inst) c->DrawIndexedInstanced(count, inst, start, base, 0); else c->DrawIndexed(count, start, base); }
            else { if (inst) c->DrawInstanced(count, inst, start, 0); else c->Draw(count, start); }
            --t_ourDraw;
            c1->VSSetConstantBuffers1(0, 1, &old, &f, &n);
            if (old) old->Release();
            c1->Release();
            g_immSkip = true;
        }
        t_flatOver = nullptr;
    }
    if (!g_immSkip) haze_before(c);   // HAZEFILL - after the HUD redirect, so pieces bound for the HUD sheet are left alone
    return g_immSkip;   // M8: a marker piece already drawn on its object (or the batch drawn here)
}

// SMOKEREC census (2026-11-01, the bonfire smoke "billboard"): for N frames, every deferred draw into a view's own targets
// keyed by (vertex shader, pixel shader, PS t0 texture view); per key the frames it was drawn in. The report lists the keys
// drawn in only PART of the window (5-90 %) with their frame range - recorded together with an eye-burst, the keys that come
// and go with the smoke are the billboard.
// SMOKEFILL (2026-11-01, JJ: the bonfire-rest smoke "still appears as a billboard"). SMOKEREC 01:10:00: the smoke is on screen
// in eye-burst frames ~150-330; the only draws that come and go exactly then are vs 0ffeda6405442fd1 / ps 2a34cf994bf2c392
// (textures 512x512 + 128x128, frames 140-389) - a smoke billboard in front of the game camera sized for its 16:9 view. For
// those draws (deferred contexts) the viewport is scaled about its centre by g_smokeK so the billboard covers the whole VR
// eye; restored right after the draw. `ds smokefill 0|K` (settings smoke_fill, default 2.0).
// SMOKELOCK: where the game camera's forward lands in eye `eye`, relative to where the eye's own forward lands, as a fraction
// of the viewport (u right, v down). Eye cameras: camera::latest (head included); head-free: camera::game_camera; the eye's
// lens edges (tangents): offaxis::window.
std::atomic<int> g_smokeLock{1};   // 1 on, -1 mirrored (test), 0 off
std::atomic<uint32_t> g_smokeLockLogged{0};
int view_of_target(int s);
bool smoke_lock_offset(int eye, float& du, float& dv) {
    if (eye != 0 && eye != 1) return false;
    camera::Cam e{}, g{};
    if (!camera::latest(eye, e) || !camera::game_camera(g) || !e.valid || !g.valid) return false;
    float w[4] = {}; offaxis::window(eye, w);   // left, right, bottom, top tangents
    if (!(w[1] > w[0]) || !(w[3] > w[2])) return false;
    const float* gf = g.r[2];
    const float x = gf[0] * e.r[0][0] + gf[1] * e.r[0][1] + gf[2] * e.r[0][2];
    const float y = gf[0] * e.r[1][0] + gf[1] * e.r[1][1] + gf[2] * e.r[1][2];
    const float z = gf[0] * e.r[2][0] + gf[1] * e.r[2][1] + gf[2] * e.r[2][2];
    if (z < 0.2f) return false;   // head turned too far from the game camera: leave it centred
    const float tx = x / z, ty = y / z;
    du = (tx - 0.0f) / (w[1] - w[0]);   // game forward vs the eye's own forward (tangent 0), as a fraction of the width
    dv = -(ty - 0.0f) / (w[3] - w[2]);  // screen y runs down
    if (g_smokeLockLogged.fetch_add(1) < 4)
        BVR_LOG("smokelock: eye %d - game forward at tangent %.3f, %.3f in the eye (window %.2f..%.2f, %.2f..%.2f) -> moved %.3f, %.3f of the picture",
                eye, tx, ty, w[0], w[1], w[2], w[3], du, dv);
    return true;
}
constexpr uint64_t kSmokePs = 0x2a34cf994bf2c392ull, kSmokeVs = 0x0ffeda6405442fd1ull;
// GSPROBE (2026-11-01): the first smoke draw logs the stages after its pass-through vertex shader (geometry / hull / domain) with
// the sizes of the geometry shader's constant buffers b0-b3, and saves that geometry shader's bytecode to captures\gs_<hash>.bin.
std::atomic<bool> g_smokeProbed{false};
void smoke_probe(ID3D11DeviceContext* c) {
    if (g_smokeProbed.exchange(true)) return;
    ID3D11GeometryShader* gs = nullptr; c->GSGetShader(&gs, nullptr, nullptr);
    ID3D11HullShader* hs = nullptr; c->HSGetShader(&hs, nullptr, nullptr);
    ID3D11DomainShader* ds = nullptr; c->DSGetShader(&ds, nullptr, nullptr);
    ID3D11Buffer* cb[4] = {}; c->GSGetConstantBuffers(0, 4, cb);
    UINT sz[4] = {};
    for (int i = 0; i < 4; ++i) if (cb[i]) { D3D11_BUFFER_DESC d{}; cb[i]->GetDesc(&d); sz[i] = d.ByteWidth; cb[i]->Release(); }
    const uint64_t gh = shaderid::hash_of(gs);
    D3D11_PRIMITIVE_TOPOLOGY top{}; c->IAGetPrimitiveTopology(&top);
    BVR_LOG("gsprobe: the smoke draw - geometry shader %p hash %016llx, hull %p, domain %p, topology %d | GS b0 %u B, b1 %u B, b2 %u B, b3 %u B",
            (void*)gs, (unsigned long long)gh, (void*)hs, (void*)ds, (int)top, sz[0], sz[1], sz[2], sz[3]);
    {   // GSPROBE 2: the domain shader builds the card - its constant buffers and both tessellation shaders' code
        ID3D11Buffer* dcb[4] = {}; c->DSGetConstantBuffers(0, 4, dcb);
        UINT dsz[4] = {};
        for (int i = 0; i < 4; ++i) if (dcb[i]) { D3D11_BUFFER_DESC d{}; dcb[i]->GetDesc(&d); dsz[i] = d.ByteWidth; dcb[i]->Release(); }
        ID3D11Buffer* hcb[4] = {}; c->HSGetConstantBuffers(0, 4, hcb);
        UINT hsz[4] = {};
        for (int i = 0; i < 4; ++i) if (hcb[i]) { D3D11_BUFFER_DESC d{}; hcb[i]->GetDesc(&d); hsz[i] = d.ByteWidth; hcb[i]->Release(); }
        BVR_LOG("gsprobe: domain shader %016llx b0 %u B, b1 %u B, b2 %u B, b3 %u B | hull shader %016llx b0 %u B, b1 %u B, b2 %u B, b3 %u B",
                (unsigned long long)shaderid::hash_of(ds), dsz[0], dsz[1], dsz[2], dsz[3], (unsigned long long)shaderid::hash_of(hs), hsz[0], hsz[1], hsz[2], hsz[3]);
        const void* objs[2] = {ds, hs}; const char* kinds[2] = {"ds", "hs"};
        for (int k = 0; k < 2; ++k) {
            std::vector<uint8_t> cc;
            if (objs[k] && shaderid::gs_code(objs[k], cc)) {
                wchar_t path[MAX_PATH];
                swprintf_s(path, L"%s\\captures\\%hs_%016llx.bin", bvr::log::data_dir(), kinds[k], (unsigned long long)shaderid::hash_of(objs[k]));
                if (FILE* f = _wfopen(path, L"wb")) { fwrite(cc.data(), 1, cc.size(), f); fclose(f); BVR_LOG("gsprobe: %s code saved (%zu bytes)", kinds[k], cc.size()); }
            } else if (objs[k]) BVR_LOG("gsprobe: %s code not kept", kinds[k]);
        }
    }
    std::vector<uint8_t> code;
    if (gs && shaderid::gs_code(gs, code)) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\captures\\gs_%016llx.bin", bvr::log::data_dir(), (unsigned long long)gh);
        if (FILE* f = _wfopen(path, L"wb")) { fwrite(code.data(), 1, code.size(), f); fclose(f); BVR_LOG("gsprobe: geometry shader code saved (%zu bytes)", code.size()); }
    } else if (gs) BVR_LOG("gsprobe: geometry shader code not kept (created before the hook?)");
    if (gs) gs->Release();
    if (hs) hs->Release();
    if (ds) ds->Release();
}
std::atomic<float> g_smokeK{0.0f};   // V13z13: off - with BBFIX 2 the billboard is locked; enlarging it about the eye centre would slide it
std::atomic<uint32_t> g_smokeHits{0};
void(STDMETHODCALLTYPE* g_rawRSVP)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*) = nullptr;   // set at install (oRSVP)

// SMOKEFACE (V13z16, JJ: "the bonfire smoke is just a billboard that rotates on its Y-axis when you rotate your head ... lock off
// its rotation"). GSPROBE 2 (01:41:48): the smoke card is built by domain shader 7211bcbdc0fd262c from each particle's own matrix
// (TEXCOORD5-7, already turned to the eye) - BBFIX never reached it. Our copy of that shader (smoke_ds.inc, signatures checked
// identical) turns the card's two edge directions by fixRow = game camera^T x eye camera, so the card faces the head-free game
// camera. `ds smokeface 0|1|2`: 0 the game's shader, 1 ours with no turn (must look IDENTICAL - the self-check), 2 locked.
#include "smoke_ds.inc"
std::atomic<int> g_smokeFace{2};
std::atomic<uint32_t> g_smokeFaced{0};
ID3D11DomainShader* g_smokeDs = nullptr;
ID3D11Buffer* g_smokeFixCb[2] = {};
bool g_smokeDsTried = false;
bool smoke_ds_ensure(ID3D11DeviceContext* c) {
    if (g_smokeDs) return true;
    if (g_smokeDsTried) return false;
    g_smokeDsTried = true;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (!dev) return false;
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(kSmokeDsSrc, sizeof(kSmokeDsSrc) - 1, "smoke_ds", nullptr, nullptr, "main", "ds_5_0", 0, 0, &code, &err);
    if (FAILED(hr) || !code) {
        BVR_LOG("smokeface: shader did not compile: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        dev->Release();
        return false;
    }
    if (err) err->Release();
    dev->CreateDomainShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_smokeDs);
    code->Release();
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (auto& b : g_smokeFixCb) dev->CreateBuffer(&bd, nullptr, &b);
    dev->Release();
    BVR_LOG("smokeface: our particle-card domain shader %s", g_smokeDs && g_smokeFixCb[0] && g_smokeFixCb[1] ? "ready" : "NOT made");
    return g_smokeDs && g_smokeFixCb[0] && g_smokeFixCb[1];
}
// fix = G^T E: a direction in the eye's frame -> the same direction in the head-free game camera's frame
std::atomic<float> g_smokeScale{2.5f};   // SMOKESCALE (V13z17, JJ: "the smoke stays put now but it's too small")
bool smoke_fix_rows(int eye, int mode, float out[12]) {
    for (int i = 0; i < 12; ++i) out[i] = (i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f;   // identity
    out[3] = 1.0f;   // card size: mode 1 (the self-check) keeps the game's
    if (mode != 2) return true;
    out[3] = g_smokeScale.load(std::memory_order_relaxed);
    camera::Cam e{}, g{};
    if (eye < 0 || !camera::latest(eye, e) || !camera::game_camera(g) || !e.valid || !g.valid) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 4 + j] = g.r[0][i] * e.r[0][j] + g.r[1][i] * e.r[1][j] + g.r[2][i] * e.r[2][j];
    return true;
}
struct SmokeGuard {
    ID3D11DeviceContext* c = nullptr; D3D11_VIEWPORT old{}; bool on = false;
    ID3D11DomainShader* oldDs = nullptr; ID3D11Buffer* oldCb = nullptr; bool faced = false;
    SmokeGuard(ID3D11DeviceContext* ctx, int s) {
        if (s < 0 || g_bound[s].psHash != kSmokePs) return;
        if (shaderid::hash_of(g_bound[s].vs) != kSmokeVs) return;
        smoke_probe(ctx);   // GSPROBE: once - which stages build the card, and the geometry shader's code
        const int face = g_smokeFace.load(std::memory_order_relaxed);
        if (face > 0 && smoke_ds_ensure(ctx)) {   // SMOKEFACE
            ID3D11DomainShader* cur = nullptr; ctx->DSGetShader(&cur, nullptr, nullptr);
            if (cur && shaderid::hash_of(cur) == 0x7211bcbdc0fd262cull) {
                const int eye = view_of_target(s);
                float rows[16] = {};   // the cb is 64 bytes; fixRow uses the first 48
                if (smoke_fix_rows(eye, face, rows) && eye >= 0) {
                    ctx->UpdateSubresource(g_smokeFixCb[eye], 0, nullptr, rows, 0, 0);
                    oldDs = cur; cur = nullptr;
                    ctx->DSGetConstantBuffers(13, 1, &oldCb);
                    ctx->DSSetShader(g_smokeDs, nullptr, 0);
                    ctx->DSSetConstantBuffers(13, 1, &g_smokeFixCb[eye]);
                    c = ctx; faced = true;
                    g_smokeFaced.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (cur) cur->Release();
        }
        const float k = g_smokeK.load(std::memory_order_relaxed);
        if (k <= 1.01f || !g_rawRSVP) return;
        UINT n = 1; ctx->RSGetViewports(&n, &old);
        if (n != 1 || old.Width < 64.0f) return;
        D3D11_VIEWPORT nv = old;
        nv.Width = old.Width * k; nv.Height = old.Height * k;
        nv.TopLeftX = old.TopLeftX + 0.5f * (old.Width - nv.Width); nv.TopLeftY = old.TopLeftY + 0.5f * (old.Height - nv.Height);
        // SMOKELOCK (V13z11, JJ: "it fills the whole view, but it's rotating with the head movements, it should be locked off"):
        // the billboard is built in front of the EYE camera (which carries the head), so it is centred on the eye's forward.
        // Move it to where the head-free GAME camera's forward lands in this eye, so it stays put while the head turns.
        float du = 0.0f, dv = 0.0f;
        if (g_smokeLock.load(std::memory_order_relaxed) != 0 && smoke_lock_offset(view_of_target(s), du, dv)) {
            const float sg = static_cast<float>(g_smokeLock.load(std::memory_order_relaxed));
            nv.TopLeftX += sg * du * old.Width * k; nv.TopLeftY += sg * dv * old.Height * k;
        }
        g_rawRSVP(ctx, 1, &nv);
        c = ctx; on = true;
        g_smokeHits.fetch_add(1, std::memory_order_relaxed);
    }
    ~SmokeGuard() {
        if (on) g_rawRSVP(c, 1, &old);
        if (faced) {
            c->DSSetShader(oldDs, nullptr, 0);
            c->DSSetConstantBuffers(13, 1, &oldCb);
            if (oldDs) oldDs->Release();
            if (oldCb) oldCb->Release();
        }
    }
};
int view_of_target(int s);
struct CensusRec { uint32_t frames = 0, first = ~0u, last = 0, lastSeen = ~0u; uint8_t views = 0; uint32_t tex = 0; UINT tw = 0, th = 0; uint64_t vs = 0, ps = 0; };
std::mutex g_censusMx;
std::unordered_map<uint64_t, CensusRec> g_census;
std::atomic<int> g_censusLeft{0};
std::atomic<uint32_t> g_censusFrame{0};
void census_note(ID3D11DeviceContext* c, int s) {
    if (s < 0) return;
    const int view = view_of_target(s);
    if (view < 0) return;
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    const uint64_t vh = shaderid::hash_of(vs);
    if (vs) vs->Release();
    ID3D11ShaderResourceView* srv = g_bound[s].psSrv[0];
    const uint64_t key = (vh * 1099511628211ull) ^ (g_bound[s].psHash * 0x9E3779B97F4A7C15ull) ^ reinterpret_cast<uintptr_t>(srv);
    const uint32_t f = g_censusFrame.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_censusMx);
    auto [it, fresh] = g_census.try_emplace(key);
    CensusRec& r = it->second;
    r.views |= static_cast<uint8_t>(1u << view);
    if (r.lastSeen != f) { r.lastSeen = f; ++r.frames; if (f < r.first) r.first = f; r.last = f; }
    if (fresh) {
        r.vs = vh; r.ps = g_bound[s].psHash;
        if (srv) {
            ID3D11Resource* res = nullptr; srv->GetResource(&res);
            if (res) {
                shaderid::tex_hash(res, &r.tex);
                D3D11_RESOURCE_DIMENSION d{}; res->GetType(&d);
                if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(res)->GetDesc(&td); r.tw = td.Width; r.th = td.Height; }
                res->Release();
            }
        }
    }
}
void census_present() {
    if (g_censusLeft.load(std::memory_order_relaxed) <= 0) return;
    g_censusFrame.fetch_add(1, std::memory_order_relaxed);
    if (g_censusLeft.fetch_sub(1) != 1) return;
    const uint32_t n = g_censusFrame.load();
    std::lock_guard<std::mutex> lk(g_censusMx);
    BVR_LOG("smokerec: census of %u frames - %zu (shader, shader, texture) keys; the ones drawn in only part of the window:", n, g_census.size());
    int shown = 0;
    for (const auto& kv : g_census) {
        const CensusRec& r = kv.second;
        if (r.frames * 20 < n || r.frames * 10 > n * 9) continue;
        if (shown++ < 60)
            BVR_LOG("smokerec   %4u frames %4u-%4u views %u | vs %016llx ps %016llx tex %08x %ux%u", r.frames, r.first, r.last, r.views,
                    (unsigned long long)r.vs, (unsigned long long)r.ps, r.tex, r.tw, r.th);
    }
    g_census.clear();
}
void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* c, UINT a, UINT b) {
    const int s = g_on.load(std::memory_order_relaxed) ? ctx_slot(c) : -1;
    if (g_censusLeft.load(std::memory_order_relaxed) > 0) census_note(c, s);   // SMOKEREC
    SmokeGuard smokeGuard(c, s);   // SMOKEFILL
    if (s >= 0 && g_skipPs.load(std::memory_order_relaxed) && g_bound[s].psHash == g_skipPs.load(std::memory_order_relaxed)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);   // SKIPPS on the recorded lists too
        return;
    }
    postlog_draw(s);      // POSTLOG
    if (s >= 0) shaft_note(g_bound[s].ps, g_bound[s].psHash, g_bound[s].rtv0, g_bound[s].psCb[0]);   // SHAFTPROBE
    hud_redirect(c, s);   // HUDSHEET
    if (hud_piece(c, s, false, a, b, 0, 0)) return;   // MARKERS: drawn on its object in both eyes instead
    if (s >= 0 && g_sdFrames.load(std::memory_order_relaxed) > 0) sd_draw(s, false);
    if (s >= 0 && g_bound[s].psHash == kShadowCopyPs) learn_x(s);
    if (s >= 0 && g_instState.load(std::memory_order_relaxed) == 1 && g_frameNo.load(std::memory_order_relaxed) == g_instFrame.load(std::memory_order_relaxed))
        inst_capture(c, g_bound[s], _ReturnAddress());
    if (s >= 0 && g_writeFrames.load(std::memory_order_relaxed) > 0) check_writer(g_bound[s], false);
    const bool mask = s >= 0 && g_bound[s].psHash == contact::kMaskPs;   // CONTACT: the sun-shadow mask pass
    const int pi = s >= 0 ? g_bound[s].psPass : -1;
    if (pi < 0) {
        if (s >= 0 && g_inHash.load(std::memory_order_relaxed) && g_inHash.load(std::memory_order_relaxed) == g_bound[s].psHash) inputs_before(c, g_bound[s], false);
        oDraw(c, a, b);
        if (mask) contact::after_mask(c, false, a, b, 0, oDraw, oDrawIdx);
        if (s >= 0 && g_ppHash.load(std::memory_order_relaxed) && g_ppHash.load(std::memory_order_relaxed) == g_bound[s].psHash) pass_probe_after(c, g_bound[s], false);
        return;
    }
    int8_t view = -1;
    pass_before(c, s, pi, view);
    oDraw(c, a, b);
    if (mask) contact::after_mask(c, false, a, b, 0, oDraw, oDrawIdx);
    pass_after(c, s, pi, view);
}
// HUDSHEET: instanced draws only for the HUD redirect (no other probe here needs them)
using DrawIdxInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
DrawIdxInstFn oDrawIdxInst = nullptr;
DrawInstFn oDrawInst = nullptr;
// POPNAME (JJ 2026-10-31: "let's try it" - name the scenery that vanishes at the right eye's outer edge with the off-centre
// lens, instead of guessing at cullers). While a phase records, every indexed draw on a deferred context is keyed by its
// mesh (index + vertex buffer and offsets, counts, vertex shader) and filed under the view whose own depth / target it
// draws into, with the CPU callers of its first sighting. Phase 0 runs with the symmetric lens, phase 1 with the off-centre
// one, same pose; the report lists meshes the right view drew in phase 0 that NO view drew in phase 1 (or drew with fewer
// instances) = the popped objects and the code that submits them.
struct PopRec { uint8_t views = 0; uint32_t inst[2] = {}; uint32_t nv[2] = {}; uint64_t vs = 0; UINT count = 0; uint32_t n = 0; uint32_t stack[10] = {}; };
std::mutex g_popMx;
std::unordered_map<uint64_t, PopRec> g_pop[2];
std::atomic<int> g_popPhase{-1}, g_popFrames{0};
std::atomic<uint32_t> g_popNoView{0};
int view_of_target(int s) {
    ID3D11View* v = g_bound[s].dsv ? static_cast<ID3D11View*>(g_bound[s].dsv) : static_cast<ID3D11View*>(g_bound[s].rtv0);
    for (int pass = 0; pass < 2 && v; ++pass) {
        ID3D11Resource* r = nullptr; v->GetResource(&r);
        const int o = r ? owner_of(r) : -1;
        if (r) r->Release();
        if (o >= 0) return o;
        v = pass == 0 && g_bound[s].dsv ? static_cast<ID3D11View*>(g_bound[s].rtv0) : nullptr;
    }
    return -1;
}
void pop_note(ID3D11DeviceContext* c, int s, UINT count, UINT start, INT base, UINT inst) {
    const int ph = g_popPhase.load(std::memory_order_relaxed);
    if (ph < 0 || s < 0) return;
    const int view = view_of_target(s);
    if (view < 0) { g_popNoView.fetch_add(1, std::memory_order_relaxed); return; }
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT f{}; UINT ioff = 0; c->IAGetIndexBuffer(&ib, &f, &ioff);
    ID3D11Buffer* vb = nullptr; UINT stride = 0, voff = 0; c->IAGetVertexBuffers(0, 1, &vb, &stride, &voff);
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    const uint64_t vh = shaderid::hash_of(vs);
    uint64_t id = 1469598103934665603ull;
    for (uint64_t x : {(uint64_t)(uintptr_t)ib, (uint64_t)ioff, (uint64_t)(uintptr_t)vb, (uint64_t)voff, (uint64_t)count, (uint64_t)start,
                       (uint64_t)(uint32_t)base, vh})
        id = (id ^ x) * 1099511628211ull;
    if (ib) ib->Release();
    if (vb) vb->Release();
    if (vs) vs->Release();
    std::lock_guard<std::mutex> lk(g_popMx);
    auto [it, fresh] = g_pop[ph].try_emplace(id);
    PopRec& r = it->second;
    r.views |= static_cast<uint8_t>(1u << view);
    if (inst > r.inst[view]) r.inst[view] = inst;
    ++r.n; ++r.nv[view];
    if (fresh) {
        r.vs = vh; r.count = count;
        void* fr[24] = {};
        const USHORT k = RtlCaptureStackBackTrace(2, 24, fr, nullptr);
        int j = 0;
        for (USHORT i = 0; i < k && j < 10; ++i) {
            const uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
            if (a >= exe_lo() && a < exe_lo() + 0x4000000) r.stack[j++] = static_cast<uint32_t>(a - exe_lo());
        }
    }
}
void STDMETHODCALLTYPE hkDrawIdxInst(ID3D11DeviceContext* c, UINT a, UINT n, UINT b, INT d, UINT e) {
    const int s = g_on.load(std::memory_order_relaxed) ? ctx_slot(c) : -1;
    if (g_censusLeft.load(std::memory_order_relaxed) > 0) census_note(c, s);   // SMOKEREC
    SmokeGuard smokeGuard(c, s);   // SMOKEFILL
    if (g_popPhase.load(std::memory_order_relaxed) >= 0) pop_note(c, s, a, b, d, n);   // POPNAME
    postlog_draw(s);
    if (s >= 0) shaft_note(g_bound[s].ps, g_bound[s].psHash, g_bound[s].rtv0, g_bound[s].psCb[0]);
    hud_redirect(c, s, true);
    oDrawIdxInst(c, a, n, b, d, e);
}
void STDMETHODCALLTYPE hkDrawInst(ID3D11DeviceContext* c, UINT a, UINT n, UINT b, UINT e) {
    const int s = g_on.load(std::memory_order_relaxed) ? ctx_slot(c) : -1;
    if (g_censusLeft.load(std::memory_order_relaxed) > 0) census_note(c, s);   // SMOKEREC
    SmokeGuard smokeGuard(c, s);   // SMOKEFILL
    postlog_draw(s);
    if (s >= 0) shaft_note(g_bound[s].ps, g_bound[s].psHash, g_bound[s].rtv0, g_bound[s].psCb[0]);
    hud_redirect(c, s, true);
    oDrawInst(c, a, n, b, e);
}
void STDMETHODCALLTYPE hkDrawIdx(ID3D11DeviceContext* c, UINT a, UINT b, INT d) {
    if (a == 14 && b == 0 && d == 0 && g_boxFrames.load(std::memory_order_relaxed) > 0) box_note(c, false);   // BOXPROBE
    const int s = g_on.load(std::memory_order_relaxed) ? ctx_slot(c) : -1;
    if (g_popPhase.load(std::memory_order_relaxed) >= 0) pop_note(c, s, a, b, d, 1);   // POPNAME
    if (g_censusLeft.load(std::memory_order_relaxed) > 0) census_note(c, s);   // SMOKEREC
    SmokeGuard smokeGuard(c, s);   // SMOKEFILL
    if (s >= 0 && g_skipPs.load(std::memory_order_relaxed) && g_bound[s].psHash == g_skipPs.load(std::memory_order_relaxed)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);   // SKIPPS on the recorded lists too
        return;
    }
    postlog_draw(s);      // POSTLOG
    if (s >= 0) shaft_note(g_bound[s].ps, g_bound[s].psHash, g_bound[s].rtv0, g_bound[s].psCb[0]);   // SHAFTPROBE
    hud_redirect(c, s);   // HUDSHEET
    if (hud_piece(c, s, true, a, b, d, 0)) return;   // MARKERS: drawn on its object in both eyes instead
    if (s >= 0 && g_sdFrames.load(std::memory_order_relaxed) > 0) sd_draw(s, false);
    if (s >= 0 && g_bound[s].psHash == kShadowCopyPs) learn_x(s);
    if (s >= 0 && g_instState.load(std::memory_order_relaxed) == 1 && g_frameNo.load(std::memory_order_relaxed) == g_instFrame.load(std::memory_order_relaxed))
        inst_capture(c, g_bound[s], _ReturnAddress());
    if (s >= 0 && g_writeFrames.load(std::memory_order_relaxed) > 0) check_writer(g_bound[s], false);
    const bool mask = s >= 0 && g_bound[s].psHash == contact::kMaskPs;   // CONTACT: the sun-shadow mask pass
    const int pi = s >= 0 ? g_bound[s].psPass : -1;
    if (pi < 0) {
        if (s >= 0 && g_inHash.load(std::memory_order_relaxed) && g_inHash.load(std::memory_order_relaxed) == g_bound[s].psHash) inputs_before(c, g_bound[s], false);
        oDrawIdx(c, a, b, d);
        if (mask) contact::after_mask(c, true, a, b, d, oDraw, oDrawIdx);
        if (s >= 0 && g_ppHash.load(std::memory_order_relaxed) && g_ppHash.load(std::memory_order_relaxed) == g_bound[s].psHash) pass_probe_after(c, g_bound[s], false);
        return;
    }
    int8_t view = -1;
    pass_before(c, s, pi, view);
    oDrawIdx(c, a, b, d);
    if (mask) contact::after_mask(c, true, a, b, d, oDraw, oDrawIdx);
    pass_after(c, s, pi, view);
}
void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    const int s = g_on.load(std::memory_order_relaxed) ? ctx_slot(c) : -1;
    postlog_dispatch(s);   // POSTLOG (wide)
    if (s >= 0 && g_sdFrames.load(std::memory_order_relaxed) > 0) sd_draw(s, true);
    if (s >= 0 && g_writeFrames.load(std::memory_order_relaxed) > 0) check_writer(g_bound[s], true);
    const int pi = s >= 0 ? g_bound[s].csPass : -1;
    if (pi < 0) {
        if (s >= 0 && g_inHash.load(std::memory_order_relaxed) && g_inHash.load(std::memory_order_relaxed) == g_bound[s].csHash) inputs_before(c, g_bound[s], true);
        oDispatch(c, x, y, z);
        if (s >= 0 && g_ppHash.load(std::memory_order_relaxed) && g_ppHash.load(std::memory_order_relaxed) == g_bound[s].csHash) pass_probe_after(c, g_bound[s], true);
        return;
    }
    int8_t view = -1;
    pass_before(c, s, pi, view);
    oDispatch(c, x, y, z);
    pass_after(c, s, pi, view);
}

// ---- CBSPY (L5 2026-10-29): the per-view constant buffer the lighting reads ----
// Dark Souls III's own names come from the geo11 fix's shader dumps (<game>\ShaderFixes\*.txt): row 28 FC_ShadowMapParam,
// 29 FC_ShadowColor, 30 FC_ShadowDir, 31-34 VC_ShadowMapMatrix, 35-37 shadow scales,
// 38-53 FC_ProjSpaceToShadowMatrix[4] (the four cascade matrices), 54 FC_CascadeSelectDist. The lighting pass samples
// the sun shadow map as g_ShadowTexture at t32. The game fills this buffer with Map(WRITE_DISCARD)/Unmap on a
// deferred context, so we read it at UNMAP (ACOVR P2: the game writes into the mapped pointer after Map returns).
constexpr uint32_t kCbShadowRow = 28, kCbLastRow = 54;
constexpr uint32_t kCbShadowOff = kCbShadowRow * 16, kCbShadowLen = (kCbLastRow + 1 - kCbShadowRow) * 16;
std::atomic<bool> g_cbSpy{false};
std::atomic<uint32_t> g_cbWant{1216};

std::atomic<int> g_cbDump{0};   // dump this many whole writes (rows as floats), then stop
// SUNTRACE (2026-10-29, JJ: "I think it's more sun direction"): the scene buffer is written three times a frame and
// the writes differ - one carries the sun direction (row 15, a unit vector) and the four cascade matrices, another
// carries zeros there. If a lighting draw is replayed against a zeroed write, the sun is not applied: near surfaces
// go dark while the sky, which does not depend on it, stays put. That is what the two frames JJ stood still for show
// (sky -2 % while the ground +6 %, then the other way round). This logs every write's sun row, frame by frame.
constexpr uint32_t kSunRow = 15, kCascade0Row = 18, kSelectRow = 34;
std::atomic<int> g_sunTrace{0};
struct SunWrite { float dir[4]; float c0; float sel; };
SunWrite g_sunWrites[8];
int g_sunN = 0;   // cbSceneParam: 71 rows (the geo11 fix's dump of the lighting shaders)

// resource class cache: 0 unknown, 1 a big constant buffer, 2 anything else
constexpr int kResCache = 4096;
std::atomic<void*> g_resKey[kResCache];
std::atomic<uint8_t> g_resVal[kResCache];
uint8_t res_class(ID3D11Resource* r) {
    const uintptr_t h = (reinterpret_cast<uintptr_t>(r) >> 4) * 2654435761u;
    for (int probe = 0; probe < 8; ++probe) {
        const int i = static_cast<int>((h + probe) & (kResCache - 1));
        void* k = g_resKey[i].load(std::memory_order_acquire);
        if (k == r) return g_resVal[i].load(std::memory_order_relaxed);
        if (!k) {
            uint8_t v = 2;
            D3D11_RESOURCE_DIMENSION d{};
            r->GetType(&d);
            if (d == D3D11_RESOURCE_DIMENSION_BUFFER) {
                ID3D11Buffer* b = nullptr;
                if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&b))) && b) {
                    D3D11_BUFFER_DESC bd{}; b->GetDesc(&bd);
                    if ((bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && bd.ByteWidth >= 512) v = static_cast<uint8_t>(bd.ByteWidth / 16 + 16);  // class = rows + 16
                    b->Release();
                }
            }
            void* expected = nullptr;
            if (g_resKey[i].compare_exchange_strong(expected, r)) g_resVal[i].store(v, std::memory_order_release);
            return v;
        }
    }
    return 2;
}

struct MapRec { ID3D11Resource* r; void* data; UINT size; };
// INSTSPY (L7): the per-object instance block (cbInstanceData, 2 x 272 bytes) carries envData = the light-set index the
// G-buffer writes into GBuffer3.a and the sun pass uses to pick FC_DirLight[index]. Log, per view, the values the engine
// writes and which engine function writes them.
constexpr UINT kInstBytes = 544;
std::atomic<int> g_instSpy{0};               // frames left
struct InstCaller { void* ret; uint32_t n[2]; uint32_t bad[2]; };
InstCaller g_instCallers[16]; int g_instCallerN = 0;
uint32_t g_instHist[2][8] = {};              // per view: envData 0..6, 7 = larger
uint32_t g_instUnknownView = 0;
std::mutex g_instMutex;
MapRec g_ctxMap[kCtx] = {};
std::mutex g_cbMutex;
int g_cbWrites = 0;                      // big-constant-buffer unmaps this frame
float g_cbFirst[kCbShadowLen / 4] = {};  // the first write's shadow rows (the main view's)
bool g_cbHaveFirst = false;
int g_cbLogged = 0;

// ROLLSCAN (V5, JJ: smoke / steam sprites roll with the head): which uploaded constants carry the EYE camera's own axes.
// For `ds rollscan [ms]` every constant-buffer upload (UpdateSubresource and Map, deferred and immediate) is searched for
// a unit vector equal (+-) to an eye camera's right / up / forward row (stride 1) or column (stride 4), and to the
// head-free game camera's (what 6-8). Roll the head while it runs: billboard constants follow the eye, not the game.
// Batman City BILLBOARD found its sprite axes the same way (BATMAN_CITY_PLAN.md).
std::atomic<ULONGLONG> g_rsUntil{0};
struct RsRow { uint32_t bytes; uint16_t off; uint8_t what, stride; uint32_t n; uint32_t ret; };
RsRow g_rs[512]; int g_rsN = 0; uint32_t g_rsScans = 0;
std::mutex g_rsMutex;
void roll_scan(const void* data, UINT bytes, void* ret) {
    const ULONGLONG until = g_rsUntil.load(std::memory_order_relaxed);
    if (!until || !data || bytes < 12 || bytes > 8192) return;
    if (GetTickCount64() > until) {
        if (!g_rsUntil.exchange(0)) return;
        std::lock_guard<std::mutex> lk(g_rsMutex);
        static const char* kWhat[9] = {"L right", "L up", "L fwd", "R right", "R up", "R fwd", "game right", "game up", "game fwd"};
        BVR_LOG("rollscan: %u uploads searched, %d distinct places hold a camera axis:", g_rsScans, g_rsN);
        for (int i = 0; i < g_rsN; ++i)
            BVR_LOG("rollscan   %4u-byte buffer, float %4u (row %3u.%u) %s %-10s x%-6u writer ds3+0x%x", g_rs[i].bytes, g_rs[i].off,
                    g_rs[i].off / 4, g_rs[i].off % 4, g_rs[i].stride == 1 ? "row   " : "column", kWhat[g_rs[i].what % 9], g_rs[i].n, g_rs[i].ret);
        g_rsN = 0; g_rsScans = 0;
        return;
    }
    camera::Cam c[3];
    if (!camera::latest(0, c[0]) || !camera::latest(1, c[1]) || !camera::game_camera(c[2])) return;
    const float* f = static_cast<const float*>(data);
    const UINT nf = bytes / 4;
    const uint32_t rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    std::lock_guard<std::mutex> lk(g_rsMutex);
    ++g_rsScans;
    for (UINT st : {1u, 4u})
        for (UINT i = 0; i + 2 * st < nf; ++i) {
            const float x = f[i], y = f[i + st], z = f[i + 2 * st];
            const float len = x * x + y * y + z * z;
            if (len < 0.98f || len > 1.02f) continue;
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a) {
                    const float* v = c[k].r[a];
                    const float d = x * v[0] + y * v[1] + z * v[2];
                    if (fabsf(d) < 0.99995f) continue;   // within ~0.6 deg
                    const uint8_t what = static_cast<uint8_t>(k * 3 + a);
                    int j = 0;
                    for (; j < g_rsN; ++j)
                        if (g_rs[j].bytes == bytes && g_rs[j].off == i && g_rs[j].what == what && g_rs[j].stride == st) { ++g_rs[j].n; break; }
                    if (j == g_rsN && g_rsN < 512) g_rs[g_rsN++] = {bytes, static_cast<uint16_t>(i), what, static_cast<uint8_t>(st), 1, rva};
                }
        }
}
MapRec g_rsImm{};   // ROLLSCAN: the immediate context's open map

// BBFIX (V6, JJ: smoke / steam sprites roll with the head). GPU particles take their facing from gcbSceneParam's
// m_gBillBoardMatrix (offset 144, a 272-byte block; CS 8f22be2a builds the draws from it), filled from the EYE camera, so
// the sprites roll with the head. Every row (or column) of that 3x3 block that equals an eye axis (+-) is replaced by the
// same axis with the roll taken out (right' = world up x forward, up' = forward x right'): the sprites still face the eye
// but stay upright. Matched by value, so the block's layout and sign convention do not matter. `ds bbfix 0|1`.
std::atomic<int> g_bbFix{2};   // V13z13: 2 = face the head-free game camera (JJ), 1 = face the eye upright, 0 = game's own
std::atomic<uint32_t> g_bbSeen{0}, g_bbFixed{0}, g_bbUnmatched{0};
constexpr UINT kBbBytes = 272, kBbFloat = 144 / 4;
bool bb_fix(float* d, UINT bytes) {
    if (bytes != kBbBytes || !d || !g_bbFix.load(std::memory_order_relaxed)) return false;
    camera::Cam e{};
    if (!camera::latest(0, e)) return false;
    const float* R[3] = {e.r[0], e.r[1], e.r[2]};
    const float* f = R[2];
    // BBFIX 2 (V13z13, JJ: "the bonfire smoke is just a billboard that rotates on its Y-axis when you rotate your head from left
    // to right. You just need to lock off its rotation"): the new axes come from the head-free GAME camera (still upright), so
    // billboards face where the game camera looks and no longer turn with the head. Mode 1 = face the eye, upright (V6).
    camera::Cam g{};
    if (g_bbFix.load(std::memory_order_relaxed) == 2 && camera::game_camera(g) && g.valid) f = g.r[2];
    float rr[3] = {f[2], 0.0f, -f[0]};                       // (0,1,0) x forward
    const float rl = sqrtf(rr[0] * rr[0] + rr[2] * rr[2]);
    if (rl < 1e-3f) return false;                            // looking straight up / down: no upright to keep
    for (float& v : rr) v /= rl;
    if (rr[0] * R[0][0] + rr[1] * R[0][1] + rr[2] * R[0][2] < 0) for (float& v : rr) v = -v;
    float uu[3] = {f[1] * rr[2] - f[2] * rr[1], f[2] * rr[0] - f[0] * rr[2], f[0] * rr[1] - f[1] * rr[0]};
    if (uu[0] * R[1][0] + uu[1] * R[1][1] + uu[2] * R[1][2] < 0) for (float& v : uu) v = -v;
    const float* N[3] = {rr, uu, f};
    g_bbSeen.fetch_add(1, std::memory_order_relaxed);
    float* m = d + kBbFloat;
    int hits = 0;
    for (int st : {1, 4}) {
        for (int k = 0; k < 3; ++k) {
            float* v0 = st == 1 ? m + 4 * k : m + k;
            const float v[3] = {v0[0], v0[st], v0[2 * st]};
            for (int a = 0; a < 3; ++a) {
                const float dt = v[0] * R[a][0] + v[1] * R[a][1] + v[2] * R[a][2];
                if (fabsf(dt) < 0.9995f) continue;
                const float s = dt > 0 ? 1.0f : -1.0f;
                v0[0] = s * N[a][0]; v0[st] = s * N[a][1]; v0[2 * st] = s * N[a][2];
                ++hits;
                break;
            }
        }
        if (hits) break;   // rows matched: the columns are the same matrix
    }
    (hits ? g_bbFixed : g_bbUnmatched).fetch_add(1, std::memory_order_relaxed);
    return hits > 0;
}

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
MapFn oMap = nullptr;
UnmapFn oUnmap = nullptr;

HRESULT STDMETHODCALLTYPE hkMap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                D3D11_MAPPED_SUBRESOURCE* out) {
    const HRESULT hr = oMap(c, r, sub, type, flags, out);
    if (g_hudDraw && SUCCEEDED(hr) && out && r && sub == 0) gfx_map(c, r, out->pData);   // MARKERS
    if ((g_cbSpy.load(std::memory_order_relaxed) || g_instSpy.load(std::memory_order_relaxed) > 0 ||
         lend3() || g_rsUntil.load(std::memory_order_relaxed) || g_bbFix.load(std::memory_order_relaxed)) && SUCCEEDED(hr) && out && r && sub == 0) {
        const uint8_t cls = res_class(r);
        const int s = cls > 16 ? ctx_slot(c) : -1;
        if (s >= 0) { g_ctxMap[s].r = r; g_ctxMap[s].data = out->pData; g_ctxMap[s].size = (cls - 16u) * 16u; }
    }
    return hr;
}

// IMMSPY (L7): the per-object instance blocks are not written on the deferred contexts; watch the immediate one.
std::atomic<int> g_immSpy{0};          // frames left
MapFn oMapI = nullptr; UnmapFn oUnmapI = nullptr;
using UpdSubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
UpdSubFn oUpdD = nullptr, oUpdI = nullptr;
ID3D11Resource* g_immMapR = nullptr; void* g_immMapData = nullptr; UINT g_immMapType = 0;
int g_immLogged = 0;
uint32_t g_immCount = 0, g_immCountUpd = 0, g_immCountDefUpd = 0;
int g_execN_now();
bool is_inst_cb(ID3D11Resource* r) { return r && res_class(r) == kInstBytes / 16 + 16; }
void imm_log(const char* how, ID3D11Resource* r, const void* data, void* ret, UINT type) {
    const uint8_t* d = static_cast<const uint8_t*>(data);
    HMODULE exe = GetModuleHandleW(nullptr);
    if (g_immLogged++ < 160)
        BVR_LOG("immspy %s buf %p type %u after replay #%d from ds3+0x%llx | envData %u / %u | pos %.1f %.1f %.1f", how, r, type, g_execN_now(),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(exe)),
                *reinterpret_cast<const uint32_t*>(d + 80), *reinterpret_cast<const uint32_t*>(d + 272 + 80),
                reinterpret_cast<const float*>(d)[3], reinterpret_cast<const float*>(d)[7], reinterpret_cast<const float*>(d)[11]);
}
// READBACK (`ds readback N`, 2026-10-31): no camera widening restores the right eye's batched copies, so the trim may test
// them against a picture read back from the GPU (a small depth / visibility map of the main view). For N frames every
// immediate Map for READING is tallied by resource (size, format, kind) and game caller.
struct RbRow { void* r; uint32_t ret; char what[96]; uint32_t n; };
std::atomic<int> g_rbFrames{0};
std::mutex g_rbMx;
std::vector<RbRow> g_rbRows;
void rb_note(ID3D11Resource* r, void* ret) {
    const uint32_t rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ret) - exe_lo());
    std::lock_guard<std::mutex> lk(g_rbMx);
    for (auto& w : g_rbRows) if (w.r == r && w.ret == rva) { ++w.n; return; }
    if (g_rbRows.size() >= 48) return;
    RbRow w{r, rva, {}, 1};
    D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        sprintf_s(w.what, "texture %ux%u fmt %d mips %u", d.Width, d.Height, (int)d.Format, d.MipLevels);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{}; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        sprintf_s(w.what, "buffer %u bytes (stride %u)", d.ByteWidth, d.StructureByteStride);
    } else sprintf_s(w.what, "resource kind %d", (int)dim);
    g_rbRows.push_back(w);
}
void rb_present() {
    if (g_rbFrames.load(std::memory_order_relaxed) <= 0 || g_rbFrames.fetch_sub(1) != 1) return;
    std::lock_guard<std::mutex> lk(g_rbMx);
    BVR_LOG("readback: %zu distinct (resource, caller) read on the CPU in the probed frames", g_rbRows.size());
    for (const auto& w : g_rbRows)
        BVR_LOG("readback   %s %p - %u reads, caller %llX", w.what, w.r, w.n, (unsigned long long)w.ret + 0x140000000ull);
    g_rbRows.clear();
}
HRESULT STDMETHODCALLTYPE hkMapI(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    const HRESULT hr = oMapI(c, r, sub, type, flags, out);
    if (g_rbFrames.load(std::memory_order_relaxed) > 0 && r && SUCCEEDED(hr) && (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE))
        rb_note(r, _ReturnAddress());   // READBACK
    if (g_hudDraw && SUCCEEDED(hr) && out && r && sub == 0) gfx_map(c, r, out->pData);   // MARKERS: the immediate HUD's constants
    if ((g_rsUntil.load(std::memory_order_relaxed) || g_bbFix.load(std::memory_order_relaxed)) && SUCCEEDED(hr) && out && r && sub == 0) {   // ROLLSCAN / BBFIX
        const uint8_t cls = res_class(r);
        if (cls > 16) { g_rsImm.r = r; g_rsImm.data = out->pData; g_rsImm.size = (cls - 16u) * 16u; }
    }
    if (g_immSpy.load(std::memory_order_relaxed) > 0 && SUCCEEDED(hr) && out && sub == 0 && is_inst_cb(r)) {
        g_immMapR = r; g_immMapData = out->pData; g_immMapType = type;
    }
    return hr;
}
void STDMETHODCALLTYPE hkUnmapI(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (g_hudDraw && r && sub == 0) gfx_unmap(c, r);   // MARKERS
    if (r && sub == 0 && r == g_rsImm.r && g_rsImm.data) {
        if (g_rsImm.size == kBbBytes) bb_fix(static_cast<float*>(g_rsImm.data), kBbBytes);   // BBFIX
        roll_scan(g_rsImm.data, g_rsImm.size, _ReturnAddress());
        g_rsImm.r = nullptr; g_rsImm.data = nullptr;
    }
    if (g_immSpy.load(std::memory_order_relaxed) > 0 && r && r == g_immMapR && g_immMapData) {
        ++g_immCount;
        imm_log("Map", r, g_immMapData, _ReturnAddress(), g_immMapType);
        g_immMapR = nullptr; g_immMapData = nullptr;
    }
    oUnmapI(c, r, sub);
}
PSSetSRV oPSSRVI = nullptr;
void STDMETHODCALLTYPE hkPSSRVI(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    if (g_sdFrames.load(std::memory_order_relaxed) > 0 && v && n <= 128) {
        g_sdImmCalls.fetch_add(1, std::memory_order_relaxed);
        for (UINT i = 0; i < n; ++i) if (v[i]) if (const uint8_t code = shadow_code_direct(v[i])) g_sdImm[code].fetch_add(1, std::memory_order_relaxed);
    }
    oPSSRVI(c, start, n, v);
}
// cbSceneParam (1136 bytes, DEFAULT usage): the MAX screen-shadow pass 10ada77c reads it at b8. Filled by UpdateSubresource.
// Rows (geo11 dump): 3 SC_CameraPos, 5-7 FC_MatrixView, 8-11 FC_MatrixInvViewProj, 24-27 VC_MatrixViewProj,
// 38-53 FC_ProjSpaceToShadowMatrix[4]. SHADOWX re-bases our view's 38-53 on the game's (shadow_scene_fix).
// DS3's cbSceneParam, read from the geo-11 fix's own shader dumps (memory geo11-fix-dumps-name-the-constants):
// the same constant NAMES and rows as Sekiro - SC_CameraPos c3, FC_MatrixView c5 (float4x3), VC_MatrixViewProj
// c24 - but the buffer ends at FC_ShadowScale1 (offset 948), so it is 960 bytes, not Sekiro's 1136, and there is
// no SC_RenderCameraPos row 61 at all.
constexpr UINT kSceneBytes = 960;
bool is_scene_cb(ID3D11Resource* r) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION d{}; r->GetType(&d);
    if (d != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC bd{}; static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
    return bd.ByteWidth == kSceneBytes && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER);
}
std::atomic<int> g_sceneDump{0};
// SHADOWSHARP (2026-10-30, JJ: MAX shadows "still aren't great", wants more in the panel): the soft-shadow filter width
// in cbSceneParam - row 35.z FC_ShadowKernelScale and row 37 FC_ShadowViewKernelScale (per cascade) - times this factor
// (1 = the game's, smaller = crisper edges). Both views, every write.
std::atomic<float> g_shadowSharp{1.0f};
std::atomic<bool> g_srParamLog{true};   // SHADOWRES: log row 28 once (and on `ds shadowres`)
std::atomic<UINT> g_dumpBytes{kSceneBytes};   // `ds scenedump N [bytes]`: which DEFAULT constant buffer size to dump
UINT cb_bytes(ID3D11Resource* r) {
    if (!r) return 0;
    D3D11_RESOURCE_DIMENSION d{}; r->GetType(&d);
    if (d != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC bd{}; static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
    return (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? bd.ByteWidth : 0;
}
// returns the data to pass on (a patched copy in `tmp`, or the game's)
const void* scene_update(const void* data, float* tmp, const char* how, UINT bytes = kSceneBytes, void* res = nullptr, void* ctx = nullptr) {
    if (bytes == kSceneBytes) scene_capture_vp(static_cast<const float*>(data));   // DLSS: this view's VC_MatrixViewProj
    if (g_sceneDump.load(std::memory_order_relaxed) > 0 && bytes == g_dumpBytes.load()) {
        const int left = g_sceneDump.fetch_sub(1);
        if (left > 0) {
            const float* f = static_cast<const float*>(data);
            BVR_LOG("scenedump %s write (%d left): buffer %p ctx %d", how, left - 1, res, ctx ? ctx_slot(ctx) : -1);
            for (int row = 0; row < static_cast<int>(g_dumpBytes.load() / 16); ++row)
                BVR_LOG("scenedump  row %2d  %14.6g %14.6g %14.6g %14.6g", row, f[row * 4], f[row * 4 + 1], f[row * 4 + 2], f[row * 4 + 3]);
        }
    }
    const float sharp = g_shadowSharp.load(std::memory_order_relaxed);
    // SHADOWRES: the 4-tap filter offset FC_ShadowMapParam.zw (row 28) is one texel of the game's map; on a map k times
    // bigger it is divided by k to stay one texel (crisper edges at the same cost), then times the panel's edge setting.
    // Measured 2026-10-29: rows 35.z / 37 (FC_ShadowKernelScale / FC_ShadowViewKernelScale) change nothing at MAX. Of
    // the 1334 dumped shaders that depth-test the sun map (t32), 1327 take their filter offset from row 28.zw (2026-10-30
    // scan); 35 / 37 are kept for the others. (The other 742 t32 readers read a screen-space shadow mask.)
    const float edge = sharp * shadowres::filter_scale(static_cast<const float*>(data)[28 * 4 + 2]);
    if ((lend3() || sharp != 1.0f || edge != 1.0f) && g_dumpBytes.load() == kSceneBytes) {
        memcpy(tmp, data, kSceneBytes);
        const bool a = lend3() && shadow_scene_fix(tmp);
        const bool b = lend3() && sub_scan(tmp, kSceneBytes);
        if (g_srParamLog.exchange(false))
            BVR_LOG("shadowres: FC_ShadowMapParam %g %g %g %g (zw = the filter offset, x %.3f here) | 35.z %g | 37 %g %g %g %g", tmp[28 * 4],
                    tmp[28 * 4 + 1], tmp[28 * 4 + 2], tmp[28 * 4 + 3], edge, tmp[35 * 4 + 2], tmp[37 * 4], tmp[37 * 4 + 1], tmp[37 * 4 + 2], tmp[37 * 4 + 3]);
        if (edge != 1.0f) { tmp[28 * 4 + 2] *= edge; tmp[28 * 4 + 3] *= edge; }
        if (sharp != 1.0f) {
            tmp[35 * 4 + 2] *= sharp;
            for (int c = 0; c < 4; ++c) tmp[37 * 4 + c] *= sharp;
        }
        if (a || b || sharp != 1.0f || edge != 1.0f) return tmp;
    }
    return data;
}
// OCCPROBE / OCCSCALE (`ds occprobe N`, `ds occscale K`, 2026-10-30): the god rays fade to nothing at the VR picture
// shape (stretching their blur changed 0 pixels) - a sun-visibility number the game measures. FromSoftware measures it
// with an occlusion query; here every occlusion query result the game reads (immediate GetData) is logged for N
// frames, and can be multiplied by K to test whether the rays come back.
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
GetDataFn oGetData = nullptr, oGetDataD = nullptr;
std::atomic<int> g_occFrames{0};
std::atomic<float> g_occScale{1.0f};
std::atomic<uint32_t> g_occReads{0}, g_occLogged{0};

// BOXPROBE (`ds boxprobe N`, 2026-10-31): the scenery pop at the right eye's outer edge. STATIC: 0x1400CADD0 runs once a
// frame for eye 0; per scenery object a GXVisibilityQueryHandle (vtable 0x143D5BA80) does Begin (method 0) -> the box draw
// in 0x1400B2010 (DrawIndexed(14, 0, 0) with the left eye's matrix) -> End (method 1, context slot 28), and in a later
// frame GetData (method 2 via 0x141921A10: 8 bytes, its call returns at 0x141921A54; S_OK -> code 0 = stored, S_FALSE ->
// code 10 = asked again). = one GPU OCCLUSION QUERY per object. For N frames this logs what the boxes are drawn against
// (targets, depth, states, viewport, shaders, callers), how many a frame on which context kind, and the GetData reads made
// by that handle: on which context (`ds occscale -1` only ever touched the immediate one) and with what answers.
std::atomic<uint32_t> g_boxDef{0}, g_boxImm{0}, g_boxLogged{0};
std::atomic<uint32_t> g_boxReadImm{0}, g_boxReadDef{0}, g_boxReadOk{0}, g_boxReadZero{0}, g_boxReadWait{0}, g_boxReadErr{0};
std::atomic<uint32_t> g_boxOtherOcc{0}, g_boxErrHr{0};
constexpr int kBoxDsv = 4;
std::atomic<void*> g_boxDsv[kBoxDsv];
std::atomic<uint32_t> g_boxDsvN[kBoxDsv];
uintptr_t exe_lo() { static const uintptr_t lo = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); return lo; }
void box_tex(ID3D11View* v, char* out, size_t cap) {
    if (!v) { sprintf_s(out, cap, "none"); return; }
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
        sprintf_s(out, cap, "%p %ux%u fmt %d x%u mips %u", (void*)r, d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, d.MipLevels);
        t->Release();
    } else sprintf_s(out, cap, "%p (not a 2D texture)", (void*)r);
    if (r) r->Release();
}
void box_note(ID3D11DeviceContext* c, bool imm) {
    (imm ? g_boxImm : g_boxDef).fetch_add(1, std::memory_order_relaxed);
    ID3D11RenderTargetView* rtv[2] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(2, rtv, &dsv);
    if (dsv) {   // which depth targets the boxes are tested against
        ID3D11Resource* r = nullptr; dsv->GetResource(&r);
        for (int i = 0; i < kBoxDsv && r; ++i) {
            void* k = g_boxDsv[i].load(std::memory_order_relaxed);
            if (k == r) { g_boxDsvN[i].fetch_add(1, std::memory_order_relaxed); break; }
            void* e = nullptr;
            if (!k && g_boxDsv[i].compare_exchange_strong(e, r)) { g_boxDsvN[i].fetch_add(1, std::memory_order_relaxed); break; }
        }
        if (r) r->Release();
    }
    if (g_boxLogged.fetch_add(1) < 6) {
        char t0[120], t1[120], td[120];
        box_tex(rtv[0], t0, sizeof(t0)); box_tex(rtv[1], t1, sizeof(t1)); box_tex(dsv, td, sizeof(td));
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; if (dsv) dsv->GetDesc(&dd);
        ID3D11DepthStencilState* dss = nullptr; UINT ref = 0; c->OMGetDepthStencilState(&dss, &ref);
        D3D11_DEPTH_STENCIL_DESC ds{}; if (dss) { dss->GetDesc(&ds); dss->Release(); }
        ID3D11BlendState* bs = nullptr; float bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm);
        D3D11_BLEND_DESC bd{}; if (bs) { bs->GetDesc(&bd); bs->Release(); }
        ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs);
        D3D11_RASTERIZER_DESC rd{}; if (rs) { rs->GetDesc(&rd); rs->Release(); }
        D3D11_VIEWPORT vp{}; UINT nvp = 1; c->RSGetViewports(&nvp, &vp);
        D3D11_PRIMITIVE_TOPOLOGY top{}; c->IAGetPrimitiveTopology(&top);
        ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
        ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
        const uint64_t vh = shaderid::hash_of(vs), ph = shaderid::hash_of(ps);
        ID3D11ShaderResourceView* vsrv[4] = {}; c->VSGetShaderResources(0, 4, vsrv);
        int nVsrv = 0; for (auto*& v : vsrv) if (v) { ++nVsrv; v->Release(); }
        ID3D11Buffer* vcb[2] = {}; c->VSGetConstantBuffers(0, 2, vcb);
        UINT cbBytes[2] = {};
        for (int i = 0; i < 2; ++i) if (vcb[i]) { D3D11_BUFFER_DESC b{}; vcb[i]->GetDesc(&b); cbBytes[i] = b.ByteWidth; vcb[i]->Release(); }
        BVR_LOG("boxprobe box draw (%s): RT0 %s | RT1 %s | depth %s (dsv fmt %d, flags %u = %s)", imm ? "immediate" : "deferred", t0, t1,
                td, (int)dd.Format, dd.Flags, dd.Flags ? "read-only" : "writable");
        BVR_LOG("boxprobe   depth test %d func %d write %d | stencil %d (ref %u) | colour write mask %x | cull %d depth clip %d "
                "scissor %d | viewport %.0f %.0f %.0fx%.0f z %.2f-%.2f | topology %d | vs %016llx%s ps %016llx%s | vs t0-3 bound %d, "
                "b0 %u B, b1 %u B",
                ds.DepthEnable, (int)ds.DepthFunc, (int)ds.DepthWriteMask, ds.StencilEnable, ref, bd.RenderTarget[0].RenderTargetWriteMask,
                (int)rd.CullMode, rd.DepthClipEnable, rd.ScissorEnable, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth,
                vp.MaxDepth, (int)top, (unsigned long long)vh, vs ? "" : " (none)", (unsigned long long)ph, ps ? "" : " (none)", nVsrv,
                cbBytes[0], cbBytes[1]);
        if (vs) vs->Release();
        if (ps) ps->Release();
        void* fr[16] = {};
        const USHORT n = RtlCaptureStackBackTrace(0, 16, fr, nullptr);
        char line[400]; int k = sprintf_s(line, "boxprobe   callers (0x140000000-based):");
        for (USHORT i = 0; i < n && k < 360; ++i) {
            const uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
            if (a >= exe_lo() && a < exe_lo() + 0x4000000) k += sprintf_s(line + k, sizeof(line) - k, " %llX", (unsigned long long)(a - exe_lo() + 0x140000000ull));
        }
        BVR_LOG("%s", line);
    }
    for (auto*& v : rtv) if (v) v->Release();
    if (dsv) dsv->Release();
}
// A GetData answer: from the visibility handle (its wrapper's call returns at 0x141921A54) or any other occlusion read.
void box_read(void* ret, bool imm, ID3D11Asynchronous* a, HRESULT hr, const void* data, UINT size) {
    if (reinterpret_cast<uintptr_t>(ret) != exe_lo() + 0x1921A54) {
        if (hr == S_OK && size == 8) g_boxOtherOcc.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    (imm ? g_boxReadImm : g_boxReadDef).fetch_add(1, std::memory_order_relaxed);
    if (hr == S_OK) {
        g_boxReadOk.fetch_add(1, std::memory_order_relaxed);
        if (data && size >= 8 && *static_cast<const uint64_t*>(data) == 0) g_boxReadZero.fetch_add(1, std::memory_order_relaxed);
    } else if (hr == S_FALSE) g_boxReadWait.fetch_add(1, std::memory_order_relaxed);
    else { g_boxReadErr.fetch_add(1, std::memory_order_relaxed); g_boxErrHr.store(static_cast<uint32_t>(hr), std::memory_order_relaxed); }
    (void)a;
}
void box_present() {
    if (g_boxFrames.load(std::memory_order_relaxed) <= 0) return;
    const int left = g_boxFrames.fetch_sub(1) - 1;
    char dl[200]; int k = 0; dl[0] = 0;
    for (int i = 0; i < kBoxDsv; ++i)
        if (void* r = g_boxDsv[i].load(std::memory_order_relaxed)) k += sprintf_s(dl + k, sizeof(dl) - k, " %p x%u", r, g_boxDsvN[i].exchange(0));
    BVR_LOG("boxprobe frame: box draws %u deferred / %u immediate | the handle's reads: %u immediate / %u deferred - %u answered "
            "(%u of them 0 pixels), %u not ready, %u errors (last %08x) | other occlusion reads %u | depth targets:%s%s",
            g_boxDef.exchange(0), g_boxImm.exchange(0), g_boxReadImm.exchange(0), g_boxReadDef.exchange(0), g_boxReadOk.exchange(0),
            g_boxReadZero.exchange(0), g_boxReadWait.exchange(0), g_boxReadErr.exchange(0), g_boxErrHr.load(), g_boxOtherOcc.exchange(0),
            dl[0] ? dl : " none", left ? "" : " (probe done)");
}

// BOXVIS (`ds boxvis 0|1`, default off): the playbook's override for a per-object occlusion query taken in one eye's view
// (FAIL-STR-025, ch. 14 #per-view-decisions): a 0-pixel answer to the scenery handle's own read (and only that read)
// becomes "visible". Costs draws, can never hide geometry.
std::atomic<int> g_boxVis{0};
std::atomic<uint32_t> g_boxVisForced{0}, g_boxVisSeen{0};
void texdump_present(ID3D11DeviceContext* imm) {
    int left = 0;
    for (int i = 0; i < g_texDumpN.load(); ++i) {
        ID3D11ShaderResourceView* v = g_texDumpSrv[i].load();
        if (!v) { if (g_texDumpHash[i]) ++left; continue; }
        if (v == reinterpret_cast<ID3D11ShaderResourceView*>(1)) continue;   // done
        ID3D11Resource* r = nullptr; v->GetResource(&r);
        D3D11_TEXTURE2D_DESC td{};
        if (r) static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
        ID3D11Device* dev = nullptr; imm->GetDevice(&dev);
        ID3D11Texture2D* out = nullptr; ID3D11RenderTargetView* rtv = nullptr;
        D3D11_TEXTURE2D_DESC od{}; od.Width = td.Width; od.Height = td.Height; od.MipLevels = 1; od.ArraySize = 1;
        od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; od.SampleDesc.Count = 1; od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_RENDER_TARGET;
        bool saved = false;
        if (dev && td.Width && SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &out)) && SUCCEEDED(dev->CreateRenderTargetView(out, nullptr, &rtv))) {
            const float clear[4] = {0, 0, 0, 0};
            imm->ClearRenderTargetView(rtv, clear);
            bvr::blit::shift_copy(imm, rtv, td.Width, td.Height, v, 0.0f);
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\captures\\tex_%08x_%ux%u_fmt%d.bmp", bvr::log::data_dir(), g_texDumpHash[i], td.Width, td.Height, (int)td.Format);
            saved = bvr::gfx_hud::save_texture_bmp(imm, out, path);
        }
        BVR_LOG("texdump: texture %08x %ux%u format %d %s", g_texDumpHash[i], td.Width, td.Height, (int)td.Format, saved ? "saved in captures" : "NOT saved");
        if (rtv) rtv->Release();
        if (out) out->Release();
        if (dev) dev->Release();
        if (r) r->Release();
        v->Release();
        g_texDumpSrv[i].store(reinterpret_cast<ID3D11ShaderResourceView*>(1));
    }
    (void)left;
}
// POPNAME: one recording phase ends after its frames (Present thread)
void pop_present() {
    if (g_popPhase.load(std::memory_order_relaxed) < 0) return;
    if (g_popFrames.fetch_sub(1) == 1) {
        const int ph = g_popPhase.exchange(-1);
        std::lock_guard<std::mutex> lk(g_popMx);
        uint32_t per[2] = {};
        for (auto& [id, r] : g_pop[ph]) for (int v = 0; v < 2; ++v) if (r.views & (1u << v)) ++per[v];
        BVR_LOG("popname: phase %d recorded - %zu meshes (left view %u, right view %u), draws on no view's target %u", ph, g_pop[ph].size(),
                per[0], per[1], g_popNoView.exchange(0));
    }
}

HRESULT occ_after(void* ret, bool imm, ID3D11Asynchronous* a, HRESULT hr, void* data, UINT size) {
    if (g_boxFrames.load(std::memory_order_relaxed) > 0 && a) box_read(ret, imm, a, hr, data, size);
    if (hr == S_OK && data && size >= 8 && g_boxVis.load(std::memory_order_relaxed) &&
        reinterpret_cast<uintptr_t>(ret) == exe_lo() + 0x1921A54) {
        g_boxVisSeen.fetch_add(1, std::memory_order_relaxed);
        uint64_t& n = *static_cast<uint64_t*>(data);
        if (n == 0) { n = 4096; g_boxVisForced.fetch_add(1, std::memory_order_relaxed); }
    }
    if (hr != S_OK || !data || !a) return hr;
    const float K = g_occScale.load(std::memory_order_relaxed);
    const bool log = g_occFrames.load(std::memory_order_relaxed) > 0;
    if (K == 1.0f && !log) return hr;
    ID3D11Query* q = nullptr;
    if (FAILED(a->QueryInterface(__uuidof(ID3D11Query), reinterpret_cast<void**>(&q))) || !q) return hr;
    D3D11_QUERY_DESC qd{}; q->GetDesc(&qd);
    q->Release();
    if (qd.Query == D3D11_QUERY_OCCLUSION && size >= 8) {
        uint64_t& n = *static_cast<uint64_t*>(data);
        g_occReads.fetch_add(1, std::memory_order_relaxed);
        if (log && g_occLogged.fetch_add(1) < 40) BVR_LOG("occprobe   query %p: %llu samples passed%s", (void*)a, (unsigned long long)n, K != 1.0f ? " (before scaling)" : "");
        if (K < 0.0f) { if (n < 4096) n = 4096; }   // V13b: `ds occscale -1` = every object reads as visible
        else if (K != 1.0f) n = static_cast<uint64_t>(static_cast<double>(n) * K);
    } else if (qd.Query == D3D11_QUERY_OCCLUSION_PREDICATE) {
        if (log && g_occLogged.fetch_add(1) < 40) BVR_LOG("occprobe   predicate %p: %d", (void*)a, size >= 4 ? *static_cast<int*>(data) : -1);
        if (K < 0.0f && size >= 4) *static_cast<int*>(data) = 1;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hkGetData(ID3D11DeviceContext* c, ID3D11Asynchronous* a, void* data, UINT size, UINT flags) {
    return occ_after(_ReturnAddress(), true, a, oGetData(c, a, data, size, flags), data, size);
}
// BOXPROBE: the deferred implementation's GetData, only hooked when it is other code than the immediate one's
HRESULT STDMETHODCALLTYPE hkGetDataD(ID3D11DeviceContext* c, ID3D11Asynchronous* a, void* data, UINT size, UINT flags) {
    return occ_after(_ReturnAddress(), false, a, oGetDataD(c, a, data, size, flags), data, size);
}
// V13b PREDICATION (JJ: gravestones pop at the right eye's outer edge; no fov widening of any culler changes it): if the game
// skips draws with GPU predication from the FIRST view's occlusion queries, objects only the second eye sees are skipped in
// both. Counted always; `ds occscale -1` drops the predicate (everything drawn).
using SetPredFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Predicate*, BOOL);
SetPredFn oSetPred = nullptr;
std::atomic<uint32_t> g_predCalls{0}, g_predDropped{0};
void STDMETHODCALLTYPE hkSetPred(ID3D11DeviceContext* c, ID3D11Predicate* p, BOOL v) {
    if (p) g_predCalls.fetch_add(1, std::memory_order_relaxed);
    if (p && g_occScale.load(std::memory_order_relaxed) < 0.0f) { g_predDropped.fetch_add(1, std::memory_order_relaxed); p = nullptr; }
    oSetPred(c, p, v);
}
SetPredFn oSetPredI = nullptr;
void STDMETHODCALLTYPE hkSetPredI(ID3D11DeviceContext* c, ID3D11Predicate* p, BOOL v) {
    if (p) g_predCalls.fetch_add(1, std::memory_order_relaxed);
    if (p && g_occScale.load(std::memory_order_relaxed) < 0.0f) { g_predDropped.fetch_add(1, std::memory_order_relaxed); p = nullptr; }
    oSetPredI(c, p, v);
}

void STDMETHODCALLTYPE hkUpdI(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    if (g_hudDraw && data && !box && sub == 0) gfx_note(r, data, cb_bytes(r));   // MARKERS
    if (g_rsUntil.load(std::memory_order_relaxed) && data && !box && sub == 0) roll_scan(data, cb_bytes(r), _ReturnAddress());   // ROLLSCAN
    if (g_bbFix.load(std::memory_order_relaxed) && data && !box && sub == 0 && cb_bytes(r) == kBbBytes) {   // BBFIX
        alignas(16) float tmp[kBbBytes / 4];
        memcpy(tmp, data, kBbBytes);
        if (bb_fix(tmp, kBbBytes)) { oUpdI(c, r, sub, box, tmp, rp, dp); return; }
    }
    if (data && !box && sub == 0 && (g_sceneDump.load(std::memory_order_relaxed) > 0 || lend3() || g_vpWant.load(std::memory_order_relaxed) ||
                                           g_shadowSharp.load(std::memory_order_relaxed) != 1.0f || shadowres::factor() != 1.0f)) {
        const UINT bytes = cb_bytes(r);
        if (bytes && g_sceneDump.load(std::memory_order_relaxed) > 0 && bytes == g_dumpBytes.load() && bytes != kSceneBytes) {
            alignas(16) float tmp[1024];
            if (bytes <= sizeof(tmp)) scene_update(data, tmp, "immediate", bytes, r, c);   // dump only
        } else if (bytes == kSceneBytes) {
            alignas(16) float tmp[kSceneBytes / 4];
            oUpdI(c, r, sub, box, scene_update(data, tmp, "immediate"), rp, dp);
            return;
        } else if (bytes && bytes <= 4096 && lend3()) {   // SHADOWSUB
            alignas(16) float tmp[1024];
            memcpy(tmp, data, bytes);
            if (sub_scan(tmp, bytes)) { oUpdI(c, r, sub, box, tmp, rp, dp); return; }
        }
    }
    if (g_immSpy.load(std::memory_order_relaxed) > 0 && data && !box && is_inst_cb(r)) { ++g_immCountUpd; imm_log("UpdateSubresource(imm)", r, data, _ReturnAddress(), 0); }
    oUpdI(c, r, sub, box, data, rp, dp);
}
// STACKS: distinct engine call chains that fill instance blocks, with per-view envData counts
struct UpdStack { uint32_t rva[10]; int depth; uint32_t n[2]; uint32_t env[2][6]; };
UpdStack g_updStacks[24]; int g_updStackN = 0;
std::mutex g_updMutex;
void upd_stack(ID3D11DeviceContext* c, const void* data) {
    void* fr[16] = {};
    const USHORT got = RtlCaptureStackBackTrace(1, 16, fr, nullptr);
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint32_t rva[10] = {}; int d = 0;
    for (USHORT i = 0; i < got && d < 10; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
        if (a > exe && a < exe + 0x8000000) rva[d++] = static_cast<uint32_t>(a - exe);   // game frames only
    }
    int view = -1;
    if (const int s = ctx_slot(c); s >= 0) for (auto* rt : g_bound[s].rtvs) if (rt) { const int o = hist_owner(res_of(rt)); if (o >= 0) { view = o; break; } }
    const uint32_t env = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data) + 80);
    std::lock_guard<std::mutex> lk(g_updMutex);
    UpdStack* u = nullptr;
    for (int i = 0; i < g_updStackN; ++i) if (g_updStacks[i].depth == d && !memcmp(g_updStacks[i].rva, rva, sizeof(rva))) u = &g_updStacks[i];
    if (!u && g_updStackN < 24) { u = &g_updStacks[g_updStackN++]; *u = {}; memcpy(u->rva, rva, sizeof(rva)); u->depth = d; }
    if (u && view >= 0) { ++u->n[view]; ++u->env[view][env < 5 ? env : 5]; }
}
void upd_stacks_log() {
    std::lock_guard<std::mutex> lk(g_updMutex);
    for (int i = 0; i < g_updStackN; ++i) {
        const UpdStack& u = g_updStacks[i];
        char line[400]; int k = sprintf_s(line, "updstack %d: GAME %u (env 0:%u 1:%u 2:%u 3:%u 4:%u >4:%u) OURS %u (env 0:%u 1:%u 2:%u 3:%u 4:%u >4:%u) | chain", i,
                                          u.n[0], u.env[0][0], u.env[0][1], u.env[0][2], u.env[0][3], u.env[0][4], u.env[0][5],
                                          u.n[1], u.env[1][0], u.env[1][1], u.env[1][2], u.env[1][3], u.env[1][4], u.env[1][5]);
        for (int d = 0; d < u.depth && k < 380; ++d) k += sprintf_s(line + k, sizeof(line) - k, " +0x%x", u.rva[d]);
        BVR_LOG("%s", line);
    }
    g_updStackN = 0;
}

std::atomic<int> g_updWho{0};   // `ds updwho N`: log the caller and source of the next N cbSceneParam updates
void STDMETHODCALLTYPE hkUpdD(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    if (g_hudDraw && data && !box && sub == 0) gfx_note(r, data, cb_bytes(r));   // MARKERS
    if (g_rsUntil.load(std::memory_order_relaxed) && data && !box && sub == 0) roll_scan(data, cb_bytes(r), _ReturnAddress());   // ROLLSCAN
    if (g_bbFix.load(std::memory_order_relaxed) && data && !box && sub == 0 && cb_bytes(r) == kBbBytes) {   // BBFIX
        alignas(16) float tmp[kBbBytes / 4];
        memcpy(tmp, data, kBbBytes);
        if (bb_fix(tmp, kBbBytes)) { oUpdD(c, r, sub, box, tmp, rp, dp); return; }
    }
    if (g_updWho.load(std::memory_order_relaxed) > 0 && data && cb_bytes(r) == kSceneBytes && g_updWho.fetch_sub(1) > 0) {
        void* fr[8] = {}; const USHORT got = RtlCaptureStackBackTrace(0, 8, fr, nullptr);
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        char st[200]; int k = 0;
        for (USHORT i = 0; i < got && k < 180; ++i) {
            const uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
            k += sprintf_s(st + k, sizeof(st) - k, a > exe && a < exe + 0x8000000 ? " ds3+%llx" : " %llx",
                           static_cast<unsigned long long>(a > exe && a < exe + 0x8000000 ? a - exe : a));
        }
        const float* f = static_cast<const float*>(data);
        BVR_LOG("updwho: cbSceneParam from %p (row 54 = %g %g %g) stack%s", data, f[54 * 4], f[54 * 4 + 1], f[54 * 4 + 2], st);
    }
    if (data && !box && sub == 0 && (g_sceneDump.load(std::memory_order_relaxed) > 0 || lend3() || g_vpWant.load(std::memory_order_relaxed) ||
                                           g_shadowSharp.load(std::memory_order_relaxed) != 1.0f || shadowres::factor() != 1.0f)) {
        const UINT bytes = cb_bytes(r);
        if (bytes && g_sceneDump.load(std::memory_order_relaxed) > 0 && bytes == g_dumpBytes.load() && bytes != kSceneBytes) {
            alignas(16) float tmp[1024];
            if (bytes <= sizeof(tmp)) scene_update(data, tmp, "deferred", bytes, r, c);   // dump only
        } else if (bytes == kSceneBytes) {
            alignas(16) float tmp[kSceneBytes / 4];
            oUpdD(c, r, sub, box, scene_update(data, tmp, "deferred"), rp, dp);
            return;
        } else if (bytes && bytes <= 4096 && lend3()) {   // SHADOWSUB
            alignas(16) float tmp[1024];
            memcpy(tmp, data, bytes);
            if (sub_scan(tmp, bytes)) { oUpdD(c, r, sub, box, tmp, rp, dp); return; }
        }
    }
    if (g_immSpy.load(std::memory_order_relaxed) > 0 && data && !box && is_inst_cb(r)) {
        ++g_immCountDefUpd;
        upd_stack(c, data);
        if (g_immLogged < 40) imm_log("UpdateSubresource(deferred)", r, data, _ReturnAddress(), 0);
    }
    oUpdD(c, r, sub, box, data, rp, dp);
}

void inst_unmap(ID3D11DeviceContext* c, int s, void* ret) {
    const uint8_t* d = static_cast<const uint8_t*>(g_ctxMap[s].data);
    int view = -1;
    for (auto* rt : g_bound[s].rtvs) if (rt) { const int o = hist_owner(res_of(rt)); if (o >= 0) { view = o; break; } }
    std::lock_guard<std::mutex> lk(g_instMutex);
    if (view < 0) { ++g_instUnknownView; return; }
    InstCaller* ic = nullptr;
    for (int i = 0; i < g_instCallerN; ++i) if (g_instCallers[i].ret == ret) ic = &g_instCallers[i];
    if (!ic && g_instCallerN < 16) { ic = &g_instCallers[g_instCallerN++]; *ic = {ret, {0, 0}, {0, 0}}; }
    for (int k = 0; k < 2; ++k) {
        const uint32_t env = *reinterpret_cast<const uint32_t*>(d + k * 272 + 80);
        ++g_instHist[view][env < 7 ? env : 7];
        if (ic) { ++ic->n[view]; if (env > 1) ++ic->bad[view]; }
    }
    (void)c;
}

void STDMETHODCALLTYPE hkUnmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (g_hudDraw && r && sub == 0) gfx_unmap(c, r);   // MARKERS: the placement of the pieces about to be drawn
    if (g_bbFix.load(std::memory_order_relaxed) && r && sub == 0) {   // BBFIX
        const int s = ctx_slot(c);
        if (s >= 0 && g_ctxMap[s].r == r && g_ctxMap[s].data && g_ctxMap[s].size == kBbBytes) bb_fix(static_cast<float*>(g_ctxMap[s].data), kBbBytes);
    }
    if (g_rsUntil.load(std::memory_order_relaxed) && r && sub == 0) {   // ROLLSCAN
        const int s = ctx_slot(c);
        if (s >= 0 && g_ctxMap[s].r == r && g_ctxMap[s].data) {
            roll_scan(g_ctxMap[s].data, g_ctxMap[s].size, _ReturnAddress());
            if (!g_cbSpy.load(std::memory_order_relaxed) && !lend3() && g_instSpy.load(std::memory_order_relaxed) <= 0) { g_ctxMap[s].r = nullptr; g_ctxMap[s].data = nullptr; }
        }
    }
    if (lend3() && r && sub == 0) {   // SHADOWX: our cascades re-based on the game's
        const int s = ctx_slot(c);
        if (s >= 0 && g_ctxMap[s].r == r && g_ctxMap[s].data && g_ctxMap[s].size == 1216) {
            shadow_mat_fix(static_cast<float*>(g_ctxMap[s].data));
            sub_scan(static_cast<float*>(g_ctxMap[s].data), 1216);
            if (!g_cbSpy.load(std::memory_order_relaxed)) { g_ctxMap[s].r = nullptr; g_ctxMap[s].data = nullptr; }
        }
    }
    if (g_instSpy.load(std::memory_order_relaxed) > 0 && r && sub == 0) {
        const int s = ctx_slot(c);
        if (s >= 0 && g_ctxMap[s].r == r && g_ctxMap[s].data && g_ctxMap[s].size == kInstBytes) inst_unmap(c, s, _ReturnAddress());
    }
    if (g_cbSpy.load(std::memory_order_relaxed) && r && sub == 0) {
        const int s = ctx_slot(c);
        if (s >= 0 && g_ctxMap[s].r == r && g_ctxMap[s].data) {
            const UINT sz = g_ctxMap[s].size;
            {   // log each distinct constant-buffer size once, to find cbSceneParam (71 rows = 1136 bytes)
                static std::atomic<uint32_t> seen[32];
                bool have = false;
                for (auto& q : seen) { const uint32_t v = q.load(std::memory_order_relaxed); if (v == sz) { have = true; break; } }
                if (!have) for (auto& q : seen) { uint32_t e = 0; if (q.compare_exchange_strong(e, sz)) { BVR_LOG("cbspy: a constant buffer of %u bytes (%u rows) is written on a deferred context", sz, sz / 16); break; } }
            }
            if (sz != g_cbWant.load(std::memory_order_relaxed)) { g_ctxMap[s].r = nullptr; g_ctxMap[s].data = nullptr; oUnmap(c, r, sub); return; }
            const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(g_ctxMap[s].data) + kCbShadowOff);
            std::lock_guard<std::mutex> lk(g_cbMutex);
            ++g_cbWrites;
            if (g_sunTrace.load() > 0 && g_sunN < 8) {
                const float* all = reinterpret_cast<const float*>(g_ctxMap[s].data);
                SunWrite& w = g_sunWrites[g_sunN++];
                for (int k = 0; k < 4; ++k) w.dir[k] = all[kSunRow * 4 + k];
                w.c0 = all[kCascade0Row * 4];
                w.sel = all[kSelectRow * 4];
            }
            if (g_cbDump.load() > 0) {           // the whole buffer, row by row, for both writes of a frame
                g_cbDump.fetch_sub(1);
                const float* all = reinterpret_cast<const float*>(g_ctxMap[s].data);
                BVR_LOG("cbdump write %d of this frame (%u rows):", g_cbWrites, sz / 16);
                for (uint32_t row = 0; row < sz / 16; ++row)
                    BVR_LOG("cbdump  w%d row %2u  %14.6g %14.6g %14.6g %14.6g", g_cbWrites, row,
                            all[row * 4], all[row * 4 + 1], all[row * 4 + 2], all[row * 4 + 3]);
            }
            if (g_sdFrames.load() > 0) {   // SHADOWDIFF: the whole shadow block of every write, and where it goes
                const int w = g_sdWrite++;
                if (w < 32) g_sdCtxWrite[s].fetch_or(1u << w, std::memory_order_relaxed);
                bool have = false;
                for (auto& b : g_sdCbBuf) if (b.load() == r) have = true;
                if (!have) for (auto& b : g_sdCbBuf) { void* e = nullptr; if (b.compare_exchange_strong(e, r)) break; }
                const float* dir = f + (30 - kCbShadowRow) * 4;
                const float* sel = f + (54 - kCbShadowRow) * 4;
                const float* vc = f + (31 - kCbShadowRow) * 4;
                BVR_LOG("shadowdiff w%d ctx %d buf %p: FC_ShadowDir %.5f %.5f %.5f | select %.4g %.4g %.4g %.4g | VC_ShadowMapMatrix r0 %.5g %.5g %.5g %.5g",
                        w, s, r, dir[0], dir[1], dir[2], sel[0], sel[1], sel[2], sel[3], vc[0], vc[1], vc[2], vc[3]);
                for (int m = 0; m < 4; ++m) {
                    const float* M = f + (38 + m * 4 - kCbShadowRow) * 4;
                    BVR_LOG("shadowdiff w%d cascade %d: %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g",
                            w, m, M[0], M[1], M[2], M[3], M[4], M[5], M[6], M[7], M[8], M[9], M[10], M[11], M[12], M[13], M[14], M[15]);
                }
            }
            if (g_cbWrites <= 2 && g_cbLogged < 8) {
                ++g_cbLogged;
                // row 30 = FC_ShadowDir, row 38 = the first cascade matrix's first row, row 54 = FC_CascadeSelectDist
                const float* dir = f + (30 - kCbShadowRow) * 4;
                const float* m0 = f + (38 - kCbShadowRow) * 4;
                const float* sel = f + (54 - kCbShadowRow) * 4;
                BVR_LOG("cbspy write %d: FC_ShadowDir %.4g %.4g %.4g | cascade0 row0 %.5g %.5g %.5g %.5g | FC_CascadeSelectDist %.4g %.4g %.4g %.4g",
                        g_cbWrites, dir[0], dir[1], dir[2], m0[0], m0[1], m0[2], m0[3], sel[0], sel[1], sel[2], sel[3]);
            }
            g_ctxMap[s].r = nullptr; g_ctxMap[s].data = nullptr;
        }
    }
    oUnmap(c, r, sub);
}

void note(ID3D11DeviceContext* c) {
    const int s = ctx_slot(c);
    if (s >= 0) {
        g_ctxWrote[s].store(1, std::memory_order_relaxed);
        g_ctxInT[s].store(1, std::memory_order_relaxed);
        g_ctxTag[s].fetch_or(g_ctxSrvBits[s].load(std::memory_order_relaxed), std::memory_order_relaxed); // inputs bound before
    }
    g_tBinds.fetch_add(1, std::memory_order_relaxed);
}
void leave_T(ID3D11DeviceContext* c) {
    const int s = ctx_slot(c);
    if (s >= 0) { g_ctxInT[s].store(0, std::memory_order_relaxed); g_ctxSrvBits[s].store(0, std::memory_order_relaxed); }
}
// VPJITTER (DLSS M6 feasibility, 2026-10-30): shift the viewport of geometry passes (a depth buffer bound, the viewport
// as large as the eye picture's render size) by a fraction of a pixel - every draw of the scene moves, full-screen post
// passes (no depth buffer) do not. `ds vpjitter x y` in pixels (0 0 = off).
std::atomic<float> g_vpJx{0.0f}, g_vpJy{0.0f};
std::atomic<uint32_t> g_vpShifted{0}, g_vpKept{0};
// Only viewports of the eye picture's own size are shifted once that size is known: the sun cascades render at 4096
// with a depth buffer bound too, and a jittered shadow map shimmers.
std::atomic<unsigned> g_vpW{0}, g_vpH{0};
using RSVPFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
RSVPFn oRSVP = nullptr;
using RSScFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
RSScFn oRSSc = nullptr;
// Immediate-context versions (other code than the deferred one; OMSetRenderTargets and RSSetViewports come through the
// frame inspector's taps): the engine draws on deferred contexts, but a bigger shadow map must never be drawn with the
// game's own viewport, wherever it is drawn.
RSScFn oRSScI = nullptr; OMSetRTUAV oSetRTUAVI = nullptr;

// ---- SHADOWRES (2026-10-30): draws into an enlarged sun shadow map get viewports and scissors scaled to match ----
// Per context: the viewports / scissors the GAME asked for (unscaled) and the scale now applied. A deferred context's state
// starts empty with every command list (FinishCommandList without restore); the immediate one after each replayed list.
struct SrState { float k = 1.0f; UINT nvp = 0; D3D11_VIEWPORT vp[8]; UINT nsc = 0; D3D11_RECT sc[8]; };
SrState g_sr[kCtx];
std::atomic<uint32_t> g_srBinds{0}, g_srVp{0}, g_srSc{0}, g_srCopies{0}, g_srImm{0};
std::atomic<int> g_srLog{0};   // `ds shadowres log N`: every viewport set on an enlarged map for N frames
void sr_scaled_vp(const SrState& st, D3D11_VIEWPORT* out) {
    for (UINT i = 0; i < st.nvp; ++i) {
        out[i] = st.vp[i];
        out[i].TopLeftX *= st.k; out[i].TopLeftY *= st.k; out[i].Width *= st.k; out[i].Height *= st.k;
    }
}
void sr_scaled_sc(const SrState& st, D3D11_RECT* out) {
    for (UINT i = 0; i < st.nsc; ++i) {
        out[i].left = static_cast<LONG>(lroundf(st.sc[i].left * st.k)); out[i].top = static_cast<LONG>(lroundf(st.sc[i].top * st.k));
        out[i].right = static_cast<LONG>(lroundf(st.sc[i].right * st.k)); out[i].bottom = static_cast<LONG>(lroundf(st.sc[i].bottom * st.k));
    }
}
// After a render-target bind: the depth target decides the scale; if it changed, the game's last viewports and scissors
// are set again at the new scale (the game may set them before or after the bind).
void sr_bind(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, RSVPFn setVp, RSScFn setSc, bool imm) {
    const int s = ctx_slot(c);
    if (s < 0) return;
    SrState& st = g_sr[s];
    const float k = shadowres::dsv_scale(dsv);
    if (k != 1.0f) { g_srBinds.fetch_add(1, std::memory_order_relaxed); if (imm) g_srImm.fetch_add(1, std::memory_order_relaxed); }
    if (k == st.k) return;
    st.k = k;
    if (st.nvp && setVp) { D3D11_VIEWPORT v[8]; sr_scaled_vp(st, v); setVp(c, st.nvp, v); }
    if (st.nsc && setSc) { D3D11_RECT r[8]; sr_scaled_sc(st, r); setSc(c, st.nsc, r); }
}
// Returns true if the viewports were set here (scaled).
bool sr_viewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vp, RSVPFn setVp) {
    const int s = ctx_slot(c);
    if (s < 0) return false;
    SrState& st = g_sr[s];
    st.nvp = vp && n <= 8 ? n : 0;
    for (UINT i = 0; i < st.nvp; ++i) st.vp[i] = vp[i];
    if (st.k == 1.0f || !st.nvp) return false;
    shadowres::viewport_seen(vp[0].TopLeftX + vp[0].Width, vp[0].TopLeftY + vp[0].Height);
    if (shadowres::engine_follows()) { st.k = 1.0f; return false; }
    D3D11_VIEWPORT v[8]; sr_scaled_vp(st, v);
    if (g_srLog.load(std::memory_order_relaxed) > 0)
        for (UINT i = 0; i < n; ++i)
            BVR_LOG("shadowres vp ctx %d: the game's %.0f %.0f %.0fx%.0f -> %.0f %.0f %.0fx%.0f", s, vp[i].TopLeftX, vp[i].TopLeftY,
                    vp[i].Width, vp[i].Height, v[i].TopLeftX, v[i].TopLeftY, v[i].Width, v[i].Height);
    g_srVp.fetch_add(1, std::memory_order_relaxed);
    setVp(c, n, v);
    return true;
}
bool sr_scissors(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r, RSScFn setSc) {
    const int s = ctx_slot(c);
    if (s < 0) return false;
    SrState& st = g_sr[s];
    st.nsc = r && n <= 8 ? n : 0;
    for (UINT i = 0; i < st.nsc; ++i) st.sc[i] = r[i];
    if (st.k == 1.0f || !st.nsc) return false;
    D3D11_RECT v[8]; sr_scaled_sc(st, v);
    g_srSc.fetch_add(1, std::memory_order_relaxed);
    setSc(c, n, v);
    return true;
}
void sr_reset(void* c) { if (const int s = ctx_slot(c); s >= 0) g_sr[s] = SrState{}; }

void STDMETHODCALLTYPE hkRSSc(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) {
    if (shadowres::active() && sr_scissors(c, n, r, oRSSc)) return;
    oRSSc(c, n, r);
}
void STDMETHODCALLTYPE hkRSScI(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) {
    if (shadowres::active() && sr_scissors(c, n, r, oRSScI)) return;
    oRSScI(c, n, r);
}
// The immediate context's OMSetRenderTargets / RSSetViewports are already hooked by the frame inspector (MinHook cannot
// hook a function twice): those two arrive through its taps; the viewports are set again through its raw function.
RSVPFn imm_vp() { return reinterpret_cast<RSVPFn>(bvr::frame_inspector::raw_viewports()); }
RSScFn imm_sc() { return oRSScI ? oRSScI : oRSSc; }
bool sr_imm_vp_tap(ID3D11DeviceContext* c, unsigned n, void* vps) {   // edits in place, true = use the edited copy
    if (!shadowres::active()) return false;
    const int s = ctx_slot(c);
    if (s < 0) return false;
    SrState& st = g_sr[s];
    auto* v = static_cast<D3D11_VIEWPORT*>(vps);
    st.nvp = n <= 8 ? n : 0;
    for (UINT i = 0; i < st.nvp; ++i) st.vp[i] = v[i];
    if (st.k == 1.0f || !st.nvp) return false;
    shadowres::viewport_seen(v[0].TopLeftX + v[0].Width, v[0].TopLeftY + v[0].Height);
    if (shadowres::engine_follows()) { st.k = 1.0f; return false; }
    sr_scaled_vp(st, v);
    g_srVp.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void sr_imm_rt_tap(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv) {
    imm_rt_changed();   // HUDSHEET
    if (shadowres::active()) sr_bind(c, dsv, imm_vp(), imm_sc(), true);
}
void STDMETHODCALLTYPE hkSetRTUAVI(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv,
                                   UINT us, UINT un, ID3D11UnorderedAccessView* const* uav, const UINT* init) {
    oSetRTUAVI(c, n, rtv, dsv, us, un, uav, init);
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) imm_rt_changed();   // HUDSHEET
    if (shadowres::active() && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) sr_bind(c, dsv, imm_vp(), imm_sc(), true);
}

void STDMETHODCALLTYPE hkRSVP(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vp) {
    if (shadowres::active() && sr_viewports(c, n, vp, oRSVP)) return;   // SHADOWRES (never jittered: not the eye's size)
    const float jx = g_vpJx.load(std::memory_order_relaxed), jy = g_vpJy.load(std::memory_order_relaxed);
    if ((jx != 0.0f || jy != 0.0f) && n >= 1 && n <= 8 && vp && g_on.load(std::memory_order_relaxed)) {
        const int s = ctx_slot(c);
        const unsigned pw = g_vpW.load(std::memory_order_relaxed), ph = g_vpH.load(std::memory_order_relaxed);
        const bool sized = pw && ph ? (fabsf(vp[0].Width - static_cast<float>(pw)) <= 8.0f && fabsf(vp[0].Height - static_cast<float>(ph)) <= 8.0f)
                                    : (vp[0].Width >= 1000.0f && vp[0].Height >= 1000.0f);
        if (s >= 0 && g_bound[s].dsv && sized) {
            D3D11_VIEWPORT v[8];
            for (UINT i = 0; i < n; ++i) { v[i] = vp[i]; v[i].TopLeftX += jx; v[i].TopLeftY += jy; }
            g_vpShifted.fetch_add(1, std::memory_order_relaxed);
            oRSVP(c, n, v);
            return;
        }
        g_vpKept.fetch_add(1, std::memory_order_relaxed);
    }
    oRSVP(c, n, vp);
}
void STDMETHODCALLTYPE hkSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    g_rtCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_on.load(std::memory_order_relaxed)) {
        if (const int s = ctx_slot(c); s >= 0) {
            g_bound[s].rtv0 = n && rtv ? rtv[0] : nullptr;
            for (UINT i = 0; i < 4; ++i) g_bound[s].rtvs[i] = rtv && i < n ? rtv[i] : nullptr;
            g_bound[s].dsv = dsv;
            g_hudBound[s] = false;   // HUDSHEET: the game's own binding replaces ours
        }
        bool hit = false;
        if (rtv) for (UINT i = 0; i < n && i < 8; ++i) if (view_is_T(rtv[i])) { hit = true; break; }
        if (hit) note(c); else leave_T(c);
    }
    oSetRT(c, n, rtv, dsv);
    if (shadowres::active()) sr_bind(c, dsv, oRSVP, oRSSc, false);   // SHADOWRES
}
void STDMETHODCALLTYPE hkSetRTUAV(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv,
                                  UINT us, UINT un, ID3D11UnorderedAccessView* const* uav, const UINT* init) {
    if (g_on.load(std::memory_order_relaxed)) {
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            if (const int s = ctx_slot(c); s >= 0) {
                g_bound[s].rtv0 = n && rtv ? rtv[0] : nullptr;
                for (UINT i = 0; i < 4; ++i) g_bound[s].rtvs[i] = rtv && i < n ? rtv[i] : nullptr;
                g_bound[s].dsv = dsv;
            }
        bool hit = false;
        if (rtv && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            for (UINT i = 0; i < n && i < 8 && !hit; ++i) hit = view_is_T(rtv[i]);
        if (uav && un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
            for (UINT i = 0; i < un && i < 8 && !hit; ++i) hit = view_is_T(uav[i]);
        if (hit) note(c);
    }
    oSetRTUAV(c, n, rtv, dsv, us, un, uav, init);
    if (shadowres::active() && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) sr_bind(c, dsv, oRSVP, oRSSc, false);   // SHADOWRES
}
void STDMETHODCALLTYPE hkCSUAV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* uav, const UINT* init) {
    if (g_on.load(std::memory_order_relaxed) && uav)
        if (const int k = ctx_slot(c); k >= 0)
            for (UINT i = 0; i < n; ++i) if (s + i < kTrackUav) g_bound[k].csUav[s + i] = uav[i];
    if (g_on.load(std::memory_order_relaxed) && uav)
        for (UINT i = 0; i < n && i < 8; ++i) if (view_is_T(uav[i])) { note(c); break; }
    oCSUAV(c, s, n, uav, init);
}
// SHADOWRES: a copy touching an enlarged map would mix sizes (a whole-texture copy between sizes is dropped by D3D11, a
// box copy lands at the game's coordinates). None was seen in the static scan; counted and logged in case one exists.
void sr_copy(const char* how, ID3D11Resource* dst, ID3D11Resource* src, const D3D11_BOX* box) {
    if (!shadowres::active() || !(shadowres::is_big(dst) || shadowres::is_big(src))) return;
    if (g_srCopies.fetch_add(1, std::memory_order_relaxed) < 8)
        BVR_LOG("shadowres: %s %p <- %p touches an enlarged shadow map%s (box %d %d - %d %d)", how, dst, src,
                shadowres::is_big(dst) == shadowres::is_big(src) ? " (both enlarged: same size)" : " - SIZES DIFFER",
                box ? static_cast<int>(box->left) : -1, box ? static_cast<int>(box->top) : -1, box ? static_cast<int>(box->right) : -1,
                box ? static_cast<int>(box->bottom) : -1);
}
void STDMETHODCALLTYPE hkCopyRes(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_on.load(std::memory_order_relaxed) && dst && dst == g_T.load(std::memory_order_relaxed)) note(c);
    sr_copy("CopyResource", dst, src, nullptr);
    oCopyRes(c, dst, src);
}
void STDMETHODCALLTYPE hkCopySub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT a, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT b,
                                 const D3D11_BOX* box) {
    if (g_on.load(std::memory_order_relaxed) && dst && dst == g_T.load(std::memory_order_relaxed)) note(c);
    sr_copy("CopySubresourceRegion", dst, src, box);
    oCopySub(c, dst, a, x, y, z, src, b, box);
}
HRESULT STDMETHODCALLTYPE hkFinish(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out) {
    const HRESULT hr = oFinish(c, restore, out);
    if (!restore) sr_reset(c);   // SHADOWRES: the deferred context starts the next list with empty state
    g_finishes.fetch_add(1, std::memory_order_relaxed);
    const int s = ctx_slot(c);
    if (s >= 0) g_hudBound[s] = false;   // HUDSHEET: each list starts with the game's own binding
    postlog_finish(s, SUCCEEDED(hr) && out ? *out : nullptr);   // POSTLOG
    const uint8_t tag = s >= 0 ? g_ctxTag[s].exchange(0, std::memory_order_relaxed) : 0;
    if (s >= 0 && g_sdFrames.load(std::memory_order_relaxed) > 0) {   // SHADOWDIFF: what this list did, by the views it read
        const uint8_t bits = g_sdCtxBits[s].exchange(0) & 3;
        const uint32_t codes = g_sdCtxCode[s].exchange(0), writes = g_sdCtxWrite[s].exchange(0), cbs = g_sdCtxCb[s].exchange(0);
        for (int st = 0; st < 2; ++st)
            for (int q = 1; q <= 2 * kShadowTex; ++q)
                if (codes & (1u << (st * 16 + q - 1))) g_sdBindView[st][q][bits].fetch_add(1, std::memory_order_relaxed);
        g_sdCbByView[bits].fetch_add(cbs, std::memory_order_relaxed);
        if ((g_sdCtxEv[s].exchange(0) || writes || cbs) && SUCCEEDED(hr) && out && *out) {
            SdList& l = g_sdLists[g_sdListHead.fetch_add(1) % 64];
            l.s = s; l.bits = bits; l.list.store(*out, std::memory_order_release);
            BVR_LOG("shadowpass list %p from ctx %d finished (reads %s; shadow-constant writes mask %X, binds that buffer %u times)", *out, s,
                    bits == 1 ? "the GAME view" : bits == 2 ? "OUR view" : bits == 3 ? "BOTH views" : "no view's own textures", writes, cbs);
        }
        if (writes || codes)
            BVR_LOG("shadowdiff list ctx %d reads %s: shadow-constant writes mask %08X, binds that buffer %u times, sun maps PS %04X CS %04X "
                    "(bits 0-7 the game view's maps, 8-15 ours)", s,
                    bits == 1 ? "the GAME view" : bits == 2 ? "OUR view" : bits == 3 ? "BOTH views" : "no view's own textures",
                    writes, cbs, codes & 0xFFFF, codes >> 16);
    }
    if (s >= 0) g_ctxInT[s].store(0, std::memory_order_relaxed);
    if (s >= 0) {   // L7: the list's bindings end with it; its watched passes travel with the list to the replay
        g_bound[s] = Bound{};
        if (const uint32_t wh = g_ctxWHits[s].exchange(0); wh && SUCCEEDED(hr) && out && *out) {
            ListW& l = g_listW[g_listWHead.fetch_add(1, std::memory_order_relaxed) % 128];
            l.hits = wh; l.hash = g_ctxWHash[s].load(); l.views = g_ctxWView[s].exchange(0); l.list.store(*out, std::memory_order_release);
        }
        if (g_ctxEv[s].n && SUCCEEDED(hr) && out && *out) {
            ListEv& l = g_listEv[g_listEvHead.fetch_add(1, std::memory_order_relaxed) % kListEv];
            l.n = g_ctxEv[s].n;
            for (int i = 0; i < l.n; ++i) l.ev[i] = g_ctxEv[s].ev[i];
            l.list.store(*out, std::memory_order_release);
        }
        g_ctxEv[s].n = 0;
    }
    if (s >= 0 && g_ctxWrote[s].exchange(0, std::memory_order_relaxed) && SUCCEEDED(hr) && out && *out) {
        const uint32_t i = g_markHead.fetch_add(1, std::memory_order_relaxed) % kLists;
        g_markTag[i].store(tag, std::memory_order_relaxed);
        g_marked[i].store(*out, std::memory_order_relaxed);
        g_marks.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

// ---- immediate context: copies after marked lists ----
// The target is written by several lists per view (18 per frame with two views, run 01:26). The copy points come from
// the previous frame's count n (stable frame to frame): left after list n/2, right after list n. Only two copies a frame.
constexpr int kSlots = 2;
ID3D11Texture2D* g_copy[kSlots] = {};
int g_writesThisFrame = 0;
int g_expect = 0;                    // the previous frame's count
std::mutex g_pairMutex;
ID3D11Texture2D* g_left = nullptr; ID3D11Texture2D* g_right = nullptr;
bool g_gotL = false, g_gotR = false;
int g_hist[65] = {};                 // frames by number of target-writing lists
// Which write each eye's copy was taken after (SELFCHECK: two copies after NEIGHBOURING writes = the same picture twice).
int g_idxL = 0, g_idxR = 0, g_pairL = 0, g_pairR = 0, g_pairWrites = 0;
constexpr int kDump = 40;
int g_lastTaggedWrite = 0;          // pair mode 4: the last write of this frame that carried any view label
std::atomic<uint32_t> g_pairUnstable{0};   // frames whose structure did not match the locked one
constexpr int kLockFrames = 45;     // a new structure must hold this many frames in a row before the lock moves
int g_candL = 0, g_candR = 0, g_candRun = 0;
int g_voteL[32] = {}, g_voteN = 0;   // LOCKVOTE: left-write candidates over the current 60-frame window
std::atomic<uint32_t> g_lockMoves{0};
std::atomic<ID3D11Resource*> g_hdrSrc{nullptr}, g_hdrDst{nullptr};
std::atomic<uint32_t> g_hdrCopies{0};
ID3D11Texture2D* g_dump[kDump] = {};
std::atomic<int> g_dumpState{0};     // 0 idle, 1 armed for the next frame (set at Present), 2 done
int g_dumpN = 0;
uint32_t g_frames = 0, g_pairs = 0, g_odd = 0;
bool g_pairThisFrame = false;        // EYESTAB measures only frames whose pair was complete

bool ensure_copy(ID3D11DeviceContext* ctx, int i, ID3D11Texture2D* T) {
    D3D11_TEXTURE2D_DESC d{}; T->GetDesc(&d);
    if (g_copy[i]) {
        D3D11_TEXTURE2D_DESC c{}; g_copy[i]->GetDesc(&c);
        if (c.Width == d.Width && c.Height == d.Height && c.Format == d.Format) return true;
        g_copy[i]->Release(); g_copy[i] = nullptr;
    }
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return false;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    d.MipLevels = 1; d.ArraySize = 1;
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_copy[i]));
    dev->Release();
    return ok;
}
std::atomic<uint32_t> g_tagCount[4];
// PAIRMODE (2026-10-29): only ~3 of the 19 lists that write the target each frame carry a view tag - a view's POST chain
// reads intermediate textures, not the view's own ones, so it is untagged and the eye's picture was copied mid-chain
// (index 8/9/11 of 19 for the left eye). Mode 1 = sticky: an untagged list belongs to the view of the last tagged list,
// so each eye is copied after the LAST write of its own run. Mode 0 = the old tagged-only rule (control).
std::atomic<int> g_pairMode{1};
// PAIRLEARN (2026-10-29): the view labels cannot be trusted for the pair - the two views share 19 textures, so a list
// of either view can carry the other's label, and the left eye then gets our view's picture ("the correct lighting
// flickering with another version with the wrong lighting direction", JJ in the headset). The frame's structure is
// stable instead: the target's content changes at fixed writes (previous frame | the game's view | ours). We LEARN
// those boundaries once from a recorded frame and copy at those exact writes afterwards.
std::atomic<int> g_leftIdx{0}, g_rightIdx{0}, g_learnedN{0};
int g_owner = 0;                   // 1 main, 2 ours - the view whose run we are in (reset each frame)
void exec_after(ID3D11DeviceContext* ctx, ID3D11CommandList* list) {
    if (shadowres::active()) sr_reset(ctx);   // SHADOWRES: a replay leaves the immediate context's state cleared
    if (!g_on.load(std::memory_order_relaxed)) return;
    const int ex = ++g_execN;
    postlog_replay(list, ex);   // POSTLOG
    if (g_sdFrames.load(std::memory_order_relaxed) > 0)
        for (auto& l : g_sdLists)
            if (l.list.load(std::memory_order_acquire) == list) {
                BVR_LOG("shadowpass replay #%d = list %p (ctx %d)", ex, list, l.s);
                l.list.store(nullptr);
                break;
            }
    const bool spy = g_histSpy.load(std::memory_order_relaxed) > 0;
    if (spy) spy_replay(list, ex);
    if (g_writeFrames.load(std::memory_order_relaxed) > 0)
        for (auto& l : g_listW)
            if (l.list.load(std::memory_order_acquire) == list) {
                if (const int K = g_stageK.load(std::memory_order_relaxed); K > 0) {   // STAGEPROBE snapshot
                    static int count[2] = {}; static uint32_t frameOf = 0;
                    if (frameOf != g_frameNo.load()) { frameOf = g_frameNo.load(); count[0] = count[1] = 0; }
                    for (int v = 0; v < 2; ++v) {
                        if (!(l.views >> v & 1) || ++count[v] != K) continue;
                        ID3D11Resource* r = v ? g_writeWatchB.load() : g_writeWatch.load();
                        ID3D11Resource*& t = g_probe[v][0];
                        if (!t || !same(shape_of(t), shape_of(r))) { if (t) t->Release(); t = create_like(ctx, r); }
                        if (t) { ctx->CopyResource(t, r); g_probeSrc[v][0] = r; }
                        if (g_stageLog) BVR_LOG("stageprobe   replay #%d = view %d's list %d writing it (%u draws, first shader %016llx)", ex, v, K, l.hits,
                                                static_cast<unsigned long long>(l.hash));
                    }
                    l.list.store(nullptr);
                    break;
                }
                BVR_LOG("writers   replay #%d: %u draws write it (first shader %016llx)", ex, l.hits, static_cast<unsigned long long>(l.hash));
                l.list.store(nullptr);
                break;
            }
    const int tag = is_marked(list);
    if (tag < 0) return;
    g_tagCount[tag & 3].fetch_add(1, std::memory_order_relaxed);
    // HDRSWAP (M3b probe): DS3's post chain is issued once after the view loop. To ask whether it is driven by the
    // HDR CONTENTS it reads, one view's HDR is copied over the other's HERE - after a list that belongs to a view has
    // been replayed, so the copy really lands behind the engine's own recorded work (a copy made when render-all
    // RETURNS lands before it: the engine records on deferred contexts and replays afterwards). Doing it after every
    // tagged write means the last one falls just before the post's first write.
    if (tag == 1 || tag == 2) {
        ID3D11Resource* src = g_hdrSrc.load(std::memory_order_relaxed);
        ID3D11Resource* dst = g_hdrDst.load(std::memory_order_relaxed);
        if (src && dst && src != dst) { ctx->CopyResource(dst, src); g_hdrCopies.fetch_add(1, std::memory_order_relaxed); }
    }
    ID3D11Texture2D* T = g_T.load(std::memory_order_relaxed);
    if (!T) return;
    const int i = ++g_writesThisFrame;          // 1-based: this was the i-th list writing the target this frame
    if (spy) BVR_LOG("histspy   replay #%d writes the picture (write %d)", ex, i);
    if (g_dumpState.load() == 1 && i <= kDump) {  // SPLITDUMP: one frame, a copy after every write
        if (!g_dump[i - 1]) {
            D3D11_TEXTURE2D_DESC d{}; T->GetDesc(&d);
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT; d.MipLevels = 1; d.ArraySize = 1;
            ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
            if (dev) { dev->CreateTexture2D(&d, nullptr, &g_dump[i - 1]); dev->Release(); }
        }
        if (g_dump[i - 1]) ctx->CopyResource(g_dump[i - 1], T);
        g_dumpN = i;
        // Which view this list belongs to, so the dump says WHOSE picture each stage is (DS3 2026-10-30).
        BVR_LOG("splitdump   write %d: %s (%u draws, first shader %016llx)", i,
                tag == 1 ? "the game's view" : tag == 2 ? "OUR second view" : tag == 3 ? "both views' textures" : "no view",
                0u, 0ull);
    }
    // ONEWAY (2026-10-29, JJ: "the left eye ... flickering with another version with incorrect lighting direction" =
    // our view's picture landing in the left slot). The game always draws its own view FIRST, so within a frame the
    // owner may only go main -> ours, never back: the two views share 19 textures, so a list of OURS can read one of
    // the main view's and be labelled main. Without this the left eye gets our picture on those frames.
    if ((tag == 1 || tag == 2) && tag >= g_owner) g_owner = tag;
    if (!g_pair.load(std::memory_order_relaxed)) return;
    if (g_pairMode.load(std::memory_order_relaxed) == 3) {   // learned, fixed write numbers
        const int li = g_leftIdx.load(std::memory_order_relaxed), ri = g_rightIdx.load(std::memory_order_relaxed);
        if (i == li && ensure_copy(ctx, 0, T)) { ctx->CopyResource(g_copy[0], T); copy_eye_depth(ctx, 0); g_gotL = true; g_idxL = i; }
        if (i == ri && ensure_copy(ctx, 1, T)) { ctx->CopyResource(g_copy[1], T); copy_eye_depth(ctx, 1); g_gotR = true; g_idxR = i; }
        return;
    }
    if (g_pairMode.load(std::memory_order_relaxed) == 4) {
        // DS3 (2026-10-30): the frame's writes to the shared picture go
        //   ... the game view's own composite ... | our view's ... | the frame's post chain (untagged) ...
        // The game view's picture stands in the target from its last write until the post chain replaces it with
        // OUR view's finished one. RIGHT = after the last write of the frame. LEFT = the target while it still
        // holds the GAME view's picture.
        //
        // LOCK (JJ 2026-10-30, twice: "left eye is flickering ... between two different cameras"). The view LABELS
        // are noisy - a list is labelled by the textures it reads and the views share 25 of 47 paths, so the same
        // write is "the game's" on one frame and "no view" on the next (split dumps 17:41 vs 18:43: write 5). Taking
        // the capture point from each frame's labels therefore moved it every frame (counter: 1309 changes in 30 s),
        // and whenever it fell to write 4 the left eye got LAST frame's right-eye picture - the second camera.
        // Sekiro's ONEWAY (left = last game-labelled write before ours) made that worse here, for the same reason.
        // The frame's CONTENT is steady: always 12 writes, the game view's picture standing from write 5 to 8. So the
        // capture points are LOCKED and only move when a different structure holds for kLockFrames frames in a row.
        const int li = g_leftIdx.load(std::memory_order_relaxed), ri = g_rightIdx.load(std::memory_order_relaxed);
        postlog_mark(1, i, tag);
        if (i == li) postlog_mark(2, i, tag);
        if (i == li && ensure_copy(ctx, 0, T)) { ctx->CopyResource(g_copy[0], T); copy_eye_depth(ctx, 0); g_gotL = true; g_idxL = i; }
        if (i == ri && ensure_copy(ctx, 1, T)) { ctx->CopyResource(g_copy[1], T); copy_eye_depth(ctx, 1); g_gotR = true; g_idxR = i; }
        if (tag == 1 || tag == 2) g_lastTaggedWrite = i;
        return;
    }
    if (g_pairMode.load(std::memory_order_relaxed) == 2) {
        // TWODRAW: the game's own view is drawn twice, so no list carries a view tag. The frame's writes split in two
        // equal halves (left draw, then right draw): left = the last write of the first half, right = the last of all.
        const int half = g_expect / 2;
        if (half > 0 && i == half && ensure_copy(ctx, 0, T)) { ctx->CopyResource(g_copy[0], T); copy_eye_depth(ctx, 0); g_gotL = true; g_idxL = i; }
        if (half > 0 && i > half && ensure_copy(ctx, 1, T)) { ctx->CopyResource(g_copy[1], T); copy_eye_depth(ctx, 1); g_gotR = true; g_idxR = i; }
        return;
    }
    // Whose picture does the target hold now? Sticky: the last tagged list's view owns the untagged ones after it.
    const int who = g_pairMode.load(std::memory_order_relaxed) ? g_owner : tag;
    if (who == 1 && ensure_copy(ctx, 0, T)) { ctx->CopyResource(g_copy[0], T); copy_eye_depth(ctx, 0); g_gotL = true; g_idxL = i; }
    if (who == 2 && ensure_copy(ctx, 1, T)) { ctx->CopyResource(g_copy[1], T); copy_eye_depth(ctx, 1); g_gotR = true; g_idxR = i; }
}

void* vfn(void* obj, int slot) { return (*reinterpret_cast<void***>(obj))[slot]; }
char g_status[200] = "picsplit: off";

} // namespace

bool install(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return false;
    ID3D11DeviceContext* def = nullptr;
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);
    if (FAILED(dev->CreateDeferredContext(0, &def)) || !def) { dev->Release(); return false; }
    struct H { int slot; void* det; void** orig; const char* name; } hooks[] = {
        {33, reinterpret_cast<void*>(&hkSetRT), reinterpret_cast<void**>(&oSetRT), "OMSetRenderTargets"},
        {34, reinterpret_cast<void*>(&hkSetRTUAV), reinterpret_cast<void**>(&oSetRTUAV), "OMSetRenderTargetsAndUAVs"},
        {68, reinterpret_cast<void*>(&hkCSUAV), reinterpret_cast<void**>(&oCSUAV), "CSSetUnorderedAccessViews"},
        {47, reinterpret_cast<void*>(&hkCopyRes), reinterpret_cast<void**>(&oCopyRes), "CopyResource"},
        {46, reinterpret_cast<void*>(&hkCopySub), reinterpret_cast<void**>(&oCopySub), "CopySubresourceRegion"},
        {114, reinterpret_cast<void*>(&hkFinish), reinterpret_cast<void**>(&oFinish), "FinishCommandList"},
        {8, reinterpret_cast<void*>(&hkPSSRV), reinterpret_cast<void**>(&oPSSRV), "PSSetShaderResources"},
        {14, reinterpret_cast<void*>(&hkMap), reinterpret_cast<void**>(&oMap), "Map"},
        {15, reinterpret_cast<void*>(&hkUnmap), reinterpret_cast<void**>(&oUnmap), "Unmap"},
        // L7: which pass is drawing (shaders by geo11 hash) and what it reads / writes
        {9, reinterpret_cast<void*>(&hkPSShader), reinterpret_cast<void**>(&oPSShader), "PSSetShader"},
        {16, reinterpret_cast<void*>(&hkPSCB), reinterpret_cast<void**>(&oPSCB), "PSSetConstantBuffers"},
        {123, reinterpret_cast<void*>(&hkPSCB1), reinterpret_cast<void**>(&oPSCB1), "PSSetConstantBuffers1"},
        {69, reinterpret_cast<void*>(&hkCSShader), reinterpret_cast<void**>(&oCSShader), "CSSetShader"},
        {67, reinterpret_cast<void*>(&hkCSSRV), reinterpret_cast<void**>(&oCSSRV), "CSSetShaderResources"},
        {13, reinterpret_cast<void*>(&hkDraw), reinterpret_cast<void**>(&oDraw), "Draw"},
        {12, reinterpret_cast<void*>(&hkDrawIdx), reinterpret_cast<void**>(&oDrawIdx), "DrawIndexed"},
        {20, reinterpret_cast<void*>(&hkDrawIdxInst), reinterpret_cast<void**>(&oDrawIdxInst), "DrawIndexedInstanced"},   // HUDSHEET
        {21, reinterpret_cast<void*>(&hkDrawInst), reinterpret_cast<void**>(&oDrawInst), "DrawInstanced"},              // HUDSHEET
        {41, reinterpret_cast<void*>(&hkDispatch), reinterpret_cast<void**>(&oDispatch), "Dispatch"},
        {44, reinterpret_cast<void*>(&hkRSVP), reinterpret_cast<void**>(&oRSVP), "RSSetViewports"},
        {45, reinterpret_cast<void*>(&hkRSSc), reinterpret_cast<void**>(&oRSSc), "RSSetScissorRects"},   // SHADOWRES
        {11, reinterpret_cast<void*>(&hkVSShader), reinterpret_cast<void**>(&oVSShader), "VSSetShader"},
        {7, reinterpret_cast<void*>(&hkVSCB), reinterpret_cast<void**>(&oVSCB), "VSSetConstantBuffers"},
        {119, reinterpret_cast<void*>(&hkVSCB1), reinterpret_cast<void**>(&oVSCB1), "VSSetConstantBuffers1"},
    };
    int ok = 0;
    for (auto& h : hooks) {
        void* fn = vfn(def, h.slot);
        const bool sharedWithImmediate = imm && vfn(imm, h.slot) == fn;
        const MH_STATUS s = MH_CreateHook(fn, h.det, h.orig);
        if (s == MH_OK) { MH_QueueEnableHook(fn); ++ok; }
        BVR_LOG("picsplit: deferred %s at %p%s - %s", h.name, fn, sharedWithImmediate ? " (same code as the immediate context)" : "",
                MH_StatusToString(s));
    }
    {   // IMMSPY: the immediate context's own Map / Unmap / UpdateSubresource (other code than the deferred one)
        struct HI { void* ctxv; int slot; void* det; void** orig; const char* name; } more[] = {
            {imm, 14, reinterpret_cast<void*>(&hkMapI), reinterpret_cast<void**>(&oMapI), "immediate Map"},
            {imm, 15, reinterpret_cast<void*>(&hkUnmapI), reinterpret_cast<void**>(&oUnmapI), "immediate Unmap"},
            {imm, 48, reinterpret_cast<void*>(&hkUpdI), reinterpret_cast<void**>(&oUpdI), "immediate UpdateSubresource"},
            {imm, 29, reinterpret_cast<void*>(&hkGetData), reinterpret_cast<void**>(&oGetData), "immediate GetData"},   // OCCPROBE
            {def, 29, reinterpret_cast<void*>(&hkGetDataD), reinterpret_cast<void**>(&oGetDataD), "deferred GetData"},   // BOXPROBE
            {def, 30, reinterpret_cast<void*>(&hkSetPred), reinterpret_cast<void**>(&oSetPred), "deferred SetPredication"},   // V13b
            {imm, 30, reinterpret_cast<void*>(&hkSetPredI), reinterpret_cast<void**>(&oSetPredI), "immediate SetPredication"},   // V13b
            {imm, 8, reinterpret_cast<void*>(&hkPSSRVI), reinterpret_cast<void**>(&oPSSRVI), "immediate PSSetShaderResources"},
            {imm, 69, reinterpret_cast<void*>(&hkCSShaderI), reinterpret_cast<void**>(&oCSShaderI), "immediate CSSetShader"},   // TILEFIX
            {def, 48, reinterpret_cast<void*>(&hkUpdD), reinterpret_cast<void**>(&oUpdD), "deferred UpdateSubresource"},
            // SHADOWRES: the immediate context's own target / viewport / scissor calls (skipped below if shared code)
            // (33 OMSetRenderTargets and 44 RSSetViewports: the frame inspector's hooks, reached through its taps)
            {imm, 34, reinterpret_cast<void*>(&hkSetRTUAVI), reinterpret_cast<void**>(&oSetRTUAVI), "immediate OMSetRenderTargetsAndUAVs"},
            {imm, 45, reinterpret_cast<void*>(&hkRSScI), reinterpret_cast<void**>(&oRSScI), "immediate RSSetScissorRects"},
        };
        for (auto& h : more) {
            if (!h.ctxv) continue;
            void* fn = vfn(h.ctxv, h.slot);
            // GetData (29) is not hooked on the deferred side, so it is hooked here even when the code is shared
            if (h.ctxv == imm && h.slot != 29 && fn == vfn(def, h.slot)) { BVR_LOG("picsplit: %s is the deferred code - not hooked twice", h.name); continue; }
            if (h.ctxv == def && h.slot == 29 && imm && fn == vfn(imm, 29)) { BVR_LOG("picsplit: %s is the immediate code - hooked once", h.name); continue; }
            const MH_STATUS st = MH_CreateHook(fn, h.det, h.orig);
            if (st == MH_OK) MH_QueueEnableHook(fn);
            BVR_LOG("picsplit: %s at %p - %s", h.name, fn, MH_StatusToString(st));
        }
    }
    const MH_STATUS ap = MH_ApplyQueued();
    def->Release();
    if (imm) imm->Release();
    dev->Release();
    g_rawRSVP = oRSVP;   // SMOKEFILL: the unhooked deferred RSSetViewports
    bvr::frame_inspector::set_exec_taps(nullptr, &exec_after);
    bvr::frame_inspector::set_viewport_tap(&sr_imm_vp_tap);   // SHADOWRES on the immediate context
    bvr::frame_inspector::set_rt_tap(&sr_imm_rt_tap);
    bvr::frame_inspector::set_draw_skip(&hud_imm_draw_tap);           // HUDSHEET: the immediate context's draws
    bvr::frame_inspector::set_draw_indexed_hook(&hud_imm_draw_tap);
    bvr::frame_inspector::set_draw_after(&haze_after);            // HAZEFILL: viewport / scissor back after the smoke pass
    bvr::frame_inspector::set_draw_indexed_after(&haze_after);
    shadowres::report();
    BVR_LOG("picsplit: %d of 19 deferred-context hooks (%s), replay tap set", ok, MH_StatusToString(ap));
    return ok >= 7 && ap == MH_OK;
}

void set_target(ID3D11Texture2D* t) { g_T.store(t, std::memory_order_relaxed); }
void set_own(int view, ID3D11Resource* const* res, int n) {
    if (view < 0 || view > 1) return;
    bool changed = false;
    for (int i = 0; i < kOwn; ++i) {
        ID3D11Resource* r = i < n ? res[i] : nullptr;
        if (g_own[view][i].exchange(r, std::memory_order_relaxed) != r) changed = true;
    }
    if (changed) g_ownGen.fetch_add(1);
}
void set_enabled(bool on) { g_on.store(on); }
void set_pair(bool on) { g_pair.store(on); }
void set_hud_on(bool on) { g_hudOn.store(on); }
void shadow_lend(int mode) {   // 3 = SHADOWX at MAX shadows (falls back to 1 elsewhere)
    g_shadowLend.store(mode == 3 ? 3 : mode == 2 ? 2 : mode ? 1 : 0);
    for (auto& m : g_mainShadowSrv) m.store(nullptr);
    BVR_LOG("shadowlend: our view's draws %s", mode ? "use the MAIN view's sun shadow maps (matched map for map)"
                                                    : "use their own (empty) ones");
}

void set_eye_pos(int view, const float rec[16]) {
    if (view < 0 || view > 1) return;
    std::lock_guard<std::mutex> lk(g_eyeMutex);
    memcpy(g_eyePos[view], rec + 12, sizeof(float) * 3);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) g_eyeRows[view][r][c] = rec[r * 4 + c];
    g_eyePosOk = true;
}

void set_shadow(int view, ID3D11Resource* const* res, int n) {
    if (view < 0 || view > 1) return;
    bool changed = false;
    for (int i = 0; i < kShadowTex; ++i) {
        ID3D11Resource* r = i < n ? res[i] : nullptr;
        if (g_shadowRes[view][i].exchange(r, std::memory_order_relaxed) != r) changed = true;
    }
    if (changed) { g_shadowGen.fetch_add(1); for (auto& m : g_mainShadowSrv) m.store(nullptr); }
}

void sun_trace(int frames) {
    g_sunN = 0;
    g_sunTrace.store(frames);
    g_cbSpy.store(true);
    BVR_LOG("suntrace: logging the sun row of every write of the scene buffer for %d frames", frames);
}

bool g_sdSpyWas = false;
void shadow_diff(int frames) {
    for (auto& a : g_sdBind) for (auto& b : a) for (auto& x : b) x.store(0);
    for (auto& a : g_sdBindView) for (auto& b : a) for (auto& x : b) x.store(0);
    for (auto& x : g_sdCbByView) x.store(0);
    for (auto& x : g_sdImm) x.store(0);
    for (auto& x : g_sdCtxBits) x.store(0);
    for (auto& x : g_sdCtxCode) x.store(0);
    for (auto& x : g_sdCtxWrite) x.store(0);
    for (auto& x : g_sdCtxCb) x.store(0);
    g_sdUntracked.store(0); g_sdImmCalls.store(0); g_sdWrite = 0;
    { std::lock_guard<std::mutex> lk(g_sdMutex); g_sdMaskN = 0; g_sdKeyN = 0; }
    g_sdSpyWas = g_cbSpy.load();
    g_cbSpy.store(true);
    g_sdFrames.store(frames > 0 ? frames : 3);
    BVR_LOG("shadowdiff: recording %d frames - sun-map binds, every shadow-constant write (sun direction + 4 cascade matrices), "
            "and which view's lists carry them; lookups that found the table full so far: %u", g_sdFrames.load(), g_shFull.load());
}
void shadow_diff_frame() {
    if (g_sdFrames.load() <= 0) return;
    BVR_LOG("shadowdiff ---- Present (%d shadow-constant writes this frame) ----", g_sdWrite);
    { std::lock_guard<std::mutex> lk(g_sdMutex); g_sdKeyN = 0; }
    g_sdWrite = 0;
    if (g_sdFrames.fetch_sub(1) != 1) return;
    static const char* kStage[2] = {"PS", "CS"};
    for (int st = 0; st < 2; ++st)
        for (int sl = 0; sl < 64; ++sl)
            for (int q = 1; q <= 2 * kShadowTex; ++q)
                if (const uint32_t k = g_sdBind[st][sl][q].load())
                    BVR_LOG("shadowdiff summary: %s t%d <- %s map %d, %u binds", kStage[st], sl,
                            q <= kShadowTex ? "the GAME view's" : "OUR view's", (q - 1) % kShadowTex, k);
    for (int st = 0; st < 2; ++st)
        for (int q = 1; q <= 2 * kShadowTex; ++q) {
            const auto& v = g_sdBindView[st][q];
            if (v[0].load() || v[1].load() || v[2].load() || v[3].load())
                BVR_LOG("shadowdiff summary: %s %s map %d bound in lists reading: no view %u, the GAME view %u, OUR view %u, both %u",
                        kStage[st], q <= kShadowTex ? "the GAME view's" : "OUR view's", (q - 1) % kShadowTex, v[0].load(), v[1].load(),
                        v[2].load(), v[3].load());
        }
    BVR_LOG("shadowdiff summary: the shadow-constant buffer bound in lists reading: no view %u, the GAME view %u, OUR view %u, both %u",
            g_sdCbByView[0].load(), g_sdCbByView[1].load(), g_sdCbByView[2].load(), g_sdCbByView[3].load());
    uint32_t imm = 0;
    for (auto& x : g_sdImm) imm += x.load();
    BVR_LOG("shadowdiff summary: texture binds on contexts outside our table %u; immediate-context texture calls %u, sun maps "
            "among them %u; table-full lookups so far %u. Done.", g_sdUntracked.load(), g_sdImmCalls.load(), imm, g_shFull.load());
    g_cbSpy.store(g_sdSpyWas);
}

void shadowres_probe(int frames) {   // `ds shadowres [N]`: what was enlarged, the filter row, N frames of viewports
    shadowres::report(true);
    g_srParamLog.store(true);
    g_srLog.store(frames > 0 ? frames : 0);
    BVR_LOG("shadowres: %s; logging the viewports set on enlarged maps for %d frames", shadowres::status(), frames);
}
void shadow_sharp(float f) { g_shadowSharp.store(f < 0.05f ? 0.05f : f > 2.0f ? 2.0f : f); BVR_LOG("shadowsharp: filter width x%.2f", g_shadowSharp.load()); }
float shadow_sharp_now() { return g_shadowSharp.load(); }
void vp_jitter(float x, float y) {
    g_vpJx.store(x); g_vpJy.store(y);
    BVR_LOG("vpjitter: geometry viewports shifted by %.3f %.3f px (so far shifted %u, kept %u)", x, y, g_vpShifted.exchange(0), g_vpKept.exchange(0));
}
void jitter_set(float x, float y) { g_vpJx.store(x, std::memory_order_relaxed); g_vpJy.store(y, std::memory_order_relaxed); }
void jitter_picture_size(unsigned w, unsigned h) { g_vpW.store(w); g_vpH.store(h); }
void set_hud_sheet(ID3D11Resource* sheet) { g_hudSheet.store(sheet, std::memory_order_relaxed); }

// HUDSHEET: our sheet, sized and formatted like the shared picture. Present thread; the recording threads only read
// the atomics. A later size change keeps the first sheet rather than free one a recorded list may still bind.
ID3D11Texture2D* hud_sheet_ensure(ID3D11Device* dev) {
    if (ID3D11Texture2D* cur = g_myHud.load()) return cur;
    ID3D11Texture2D* T = g_T.load(std::memory_order_relaxed);
    if (!dev || !T) return nullptr;
    D3D11_TEXTURE2D_DESC td{}; T->GetDesc(&td);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = td.Width; d.Height = td.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = td.Format;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* ds = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex)) || !tex) return nullptr;
    if (FAILED(dev->CreateRenderTargetView(tex, nullptr, &rtv)) || !rtv) { tex->Release(); return nullptr; }
    D3D11_TEXTURE2D_DESC dd = d; dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&dd, nullptr, &ds)) || !ds || FAILED(dev->CreateDepthStencilView(ds, nullptr, &dsv)) || !dsv) {
        if (ds) ds->Release();
        rtv->Release(); tex->Release();
        return nullptr;
    }
    g_myHudDsv.store(dsv); g_myHudRtv.store(rtv); g_myHud.store(tex);
    BVR_LOG("hudsheet: our HUD sheet %ux%u format %d made - the HUD goes to the floating panel", d.Width, d.Height, (int)d.Format);
    return tex;
}
void hud_redirect_set(int on) { g_hudRedirect.store(on); }
void hud_world_keep(int on) { g_hudWorldKeep.store(on); BVR_LOG("hudworld %d: on-object pieces %s", on, on ? "kept flat in the picture (old)" : "go the sheet / marker way"); }
// Present thread, once a frame: the immediate context gets the game's binding back if a HUD piece was the last thing
// drawn, the frame number moves on (one sheet clear per frame), and a sheet no HUD piece has touched for 10 frames is
// cleared, so a HUD the game stopped drawing does not stay on the panel.
void hud_present(ID3D11DeviceContext* imm) {
    postlog_present();   // POSTLOG
    sun_present();       // SUNPROBE
    box_present();       // BOXPROBE
    if (imm && g_texDumpN.load(std::memory_order_relaxed)) texdump_present(imm);   // TEXDUMP
    pop_present();       // POPNAME
    rb_present();        // READBACK
    census_present();    // SMOKEREC
    if (g_occFrames.load(std::memory_order_relaxed) > 0 && g_occFrames.fetch_sub(1) == 1)
        BVR_LOG("occprobe: %u occlusion results read in the probed frames", g_occReads.exchange(0));
    shaft_present(imm);  // SHAFTPROBE
    if (imm) imm_restore(imm);
    const uint32_t f = g_presentSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    ID3D11RenderTargetView* rtv = g_myHudRtv.load(std::memory_order_relaxed);
    ID3D11DepthStencilView* dsv = g_myHudDsv.load(std::memory_order_relaxed);
    if (imm && rtv && dsv && f - g_hudSeenAt.load(std::memory_order_relaxed) == 10) {
        g_hudClearedAt.store(f, std::memory_order_relaxed);
        hud_clear(imm, rtv, dsv);
    }
    if (const int n = g_hudTexFrames.load(std::memory_order_relaxed); n > 0 && g_hudTexFrames.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lk(g_hudTexMx);
        BVR_LOG("hudtex: %zu distinct pieces drawn into the shared picture (vertex shader / PS t0 texture by 3Dmigoto hash, "
                "%u game textures hashed so far):", g_hudTexRows.size(), shaderid::textures_hashed());
        for (const auto& r : g_hudTexRows)
            BVR_LOG("hudtex   vs %016llx  tex %08x %4ux%-4u  %s %s %s x%u", static_cast<unsigned long long>(r.vs), r.tex, r.w, r.h,
                    r.kind == 1 ? "HUD(fix hash)" : r.kind == 2 ? "HUD(signature)" : "not HUD     ",
                    r.world ? "ON-OBJECT" : "flat     ", r.imm ? "immediate" : "recorded ", r.n);
        g_hudTexRows.clear();
    }
}
void ray_scale_set(float k) { g_rayScale.store(k > 0.0f ? k : 1.0f); BVR_LOG("rayscale: god-ray blur length x%.2f (stretched so far %u)", g_rayScale.load(), g_rayDraws.load()); }
void bbfix(int on) { if (on >= 0) g_bbFix.store(on > 2 ? 2 : on); BVR_LOG("bbfix %d: billboard blocks seen %u, upright %u, not matched %u", g_bbFix.load(), g_bbSeen.load(), g_bbFixed.load(), g_bbUnmatched.load()); }
void rollscan(int ms) { g_rsUntil.store(GetTickCount64() + (ms > 0 ? ms : 3000)); BVR_LOG("rollscan: searching constant uploads for camera axes for %d ms", ms > 0 ? ms : 3000); }
void popname_record(int phase, int frames) {
    if (phase < 0 || phase > 1) return;
    {
        std::lock_guard<std::mutex> lk(g_popMx);
        g_pop[phase].clear();
    }
    g_popNoView.store(0);
    g_popFrames.store(frames > 0 ? frames : 20);
    g_popPhase.store(phase);
    BVR_LOG("popname: recording phase %d (%s lens) for %d frames", phase, phase ? "off-centre" : "symmetric", frames > 0 ? frames : 20);
}
bool popname_busy() { return g_popPhase.load() >= 0; }
void readback(int frames) { g_rbFrames.store(frames > 0 ? frames : 10); BVR_LOG("readback: logging CPU reads of GPU data for %d frames", frames > 0 ? frames : 10); }
// POPSWEEP: copies of instanced meshes (max instances > 1) each view drew, per phase - shared culling shows as L == R
void popname_meshes(uint32_t out[2][2]) {   // POPSWEEP3: distinct meshes each view drew, per phase
    std::lock_guard<std::mutex> lk(g_popMx);
    for (int ph = 0; ph < 2; ++ph) {
        out[ph][0] = out[ph][1] = 0;
        for (auto& [id, r] : g_pop[ph]) { if (r.views & 1) ++out[ph][0]; if (r.views & 2) ++out[ph][1]; }
    }
}
void popname_copies(uint32_t out[2][2]) {
    std::lock_guard<std::mutex> lk(g_popMx);
    for (int ph = 0; ph < 2; ++ph) {
        out[ph][0] = out[ph][1] = 0;
        for (auto& [id, r] : g_pop[ph])
            if (r.inst[0] > 1 || r.inst[1] > 1) { out[ph][0] += r.inst[0]; out[ph][1] += r.inst[1]; }
    }
}
void popname_report() {
    std::lock_guard<std::mutex> lk(g_popMx);
    struct Row { uint64_t id; const PopRec* a; const PopRec* b; bool gone; };
    std::vector<Row> rows;
    for (auto& [id, ra] : g_pop[0]) {
        if (!(ra.views & 2)) continue;                        // the right view drew it with the symmetric lens
        auto it = g_pop[1].find(id);
        const PopRec* rb = it == g_pop[1].end() ? nullptr : &it->second;
        if (!rb) rows.push_back({id, &ra, nullptr, true});   // nobody drew it with the off-centre lens
        else if (!(rb->views & 2) && !(rb->views & 1)) rows.push_back({id, &ra, rb, true});
        else if (ra.inst[1] > 1 && rb->inst[1] < ra.inst[1] && rb->inst[0] < ra.inst[1]) rows.push_back({id, &ra, rb, false});   // fewer instances
        else if (ra.nv[1] >= rb->nv[1] + 10 && rb->nv[0] <= ra.nv[0]) rows.push_back({id, &ra, rb, false});   // a shared mesh drawn fewer times
    }
    BVR_LOG("popname: REPORT - %zu meshes the right view drew with the symmetric lens and no view drew (or fewer instances) with "
            "the off-centre lens (phase 0: %zu meshes, phase 1: %zu)", rows.size(), g_pop[0].size(), g_pop[1].size());
    // Group by caller chain so one submit path shows once with its mesh count
    std::unordered_map<uint64_t, std::pair<int, const Row*>> byStack;
    for (const auto& r : rows) {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t s : r.a->stack) h = (h ^ s) * 1099511628211ull;
        auto& e = byStack[h]; if (!e.second) e.second = &r; ++e.first;
    }
    for (auto& [h, e] : byStack) {
        const Row& r = *e.second;
        char line[400]; int k = sprintf_s(line, "popname   %d mesh(es) via:", e.first);
        for (uint32_t s : r.a->stack) if (s) k += sprintf_s(line + k, sizeof(line) - k, " %llX", (unsigned long long)s + 0x140000000ull);
        BVR_LOG("%s", line);
    }
    int shown = 0;
    for (const auto& r : rows) {
        if (shown++ >= 40) break;
        BVR_LOG("popname   mesh %016llx vs %016llx %u indices | symmetric: views %u inst L%u R%u, draws L%u R%u | off-centre: %s",
                (unsigned long long)r.id, (unsigned long long)r.a->vs, r.a->count, r.a->views, r.a->inst[0], r.a->inst[1], r.a->nv[0], r.a->nv[1],
                [&] { static char b[80]; if (!r.b) sprintf_s(b, "not drawn"); else sprintf_s(b, "views %u inst L%u R%u, draws L%u R%u", r.b->views, r.b->inst[0], r.b->inst[1], r.b->nv[0], r.b->nv[1]); return b; }());
    }
}
uint32_t fe_draws() { return g_feDraws.load(std::memory_order_relaxed); }
ULONGLONG fe_ms_since() { const ULONGLONG t = g_feMs.load(std::memory_order_relaxed); return t ? GetTickCount64() - t : ~0ull; }
void smokerec(int frames) {
    { std::lock_guard<std::mutex> lk(g_censusMx); g_census.clear(); }
    g_censusFrame.store(0);
    g_censusLeft.store(frames > 0 ? frames : 800);
    BVR_LOG("smokerec: census of every 3D draw for %d frames", frames > 0 ? frames : 800);
}
void smokescale(float k) { g_smokeScale.store(k > 0.1f ? k : 1.0f); BVR_LOG("smokescale x%.2f: the smoke cards' size", g_smokeScale.load()); }
void smokeface(int m) {
    g_smokeFace.store(m < 0 ? 0 : m > 2 ? 2 : m);
    BVR_LOG("smokeface %d (%s): smoke cards drawn with our shader %u times", g_smokeFace.load(),
            g_smokeFace.load() == 0 ? "the game's shader" : g_smokeFace.load() == 1 ? "ours, no turn - must look identical" : "facing the game camera",
            g_smokeFaced.load());
}
void smokelock(int m) { g_smokeLock.store(m > 0 ? 1 : m < 0 ? -1 : 0); g_smokeLockLogged.store(0); BVR_LOG("smokelock %d", g_smokeLock.load()); }
void smokefill(float k) {
    g_smokeK.store(k > 1.0f ? k : 0.0f);
    BVR_LOG("smokefill %.2f: the bonfire smoke billboard %s (drawn enlarged %u times so far)", g_smokeK.load(),
            g_smokeK.load() > 1.0f ? "covers the whole eye" : "is the game's own size", g_smokeHits.load());
}
void hazefill(float f) {
    g_hazeFill.store(f < 0.0f ? 0.0f : f);
    BVR_LOG("hazefill %.2f (%s): smoke passes stretched so far %u", g_hazeFill.load(), g_hazeFill.load() <= 0.0f ? "off" : g_hazeFill.load() > 1.0f ? "fixed scale" : "auto",
            g_hazeHits.load());
}
void tex_list_cmd(const char* what, const char* args, std::atomic<uint32_t>* l, std::atomic<int>& n, const char* key) {
    tex_lists_load();
    char k[32];
    if (args && !_strnicmp(args, "clear", 5)) {
        for (int i = 0; i < kTexList; ++i) { snprintf(k, sizeof(k), "%s_%d", key, i); settings::set_i(k, 0); l[i].store(0); }
        n.store(0);
    } else if (args) {
        unsigned h = 0;
        if (sscanf_s(args, "%x", &h) == 1 && h && !in_list(l, n.load(), h) && n.load() < kTexList) {
            const int i = n.load(); l[i].store(h); n.store(i + 1);
            snprintf(k, sizeof(k), "%s_%d", key, i); settings::set_i(k, static_cast<int>(h));
        }
    }
    char s[200] = ""; for (int i = 0; i < n.load(); ++i) { char t[12]; snprintf(t, sizeof(t), " %08x", l[i].load()); strcat_s(s, t); }
    BVR_LOG("%s: textures%s (saved) | hidden so far %u, fade pieces taken %u", what, n.load() ? s : " none", g_hiddenByTex.load(), g_fadeTaken.load());
}
void hudfade(const char* args) { tex_list_cmd("hudfade", args, g_fadeTex, g_fadeTexN, "hud_fade"); }
ULONGLONG fade_ms_since() { const ULONGLONG t = g_fadeSeenMs.load(std::memory_order_relaxed); return t ? GetTickCount64() - t : ~0ull; }
void hudhide(const char* args) {
    unsigned w = 0, h = 0;
    if (args && !_strnicmp(args, "vs", 2)) {   // HUDHIDEVS: `vs <16 hex> <W>x<H>` | `vs clear`
        tex_lists_load();
        char k[32];
        unsigned long long vh = 0;
        if (strstr(args, "clear")) {
            for (int i = 0; i < kTexList; ++i) { snprintf(k, sizeof(k), "hud_hidevs_%d", i); settings::set_i(k, 0); g_hideVs[i].store(0); }
            g_hideVsN.store(0);
        } else if (sscanf_s(args + 2, "%llx %ux%u", &vh, &w, &h) == 3 && g_hideVsN.load() < kTexList) {
            const int i = g_hideVsN.load();
            g_hideVs[i].store(fold(vh)); g_hideVsW[i].store(w); g_hideVsH[i].store(h); g_hideVsN.store(i + 1);
            snprintf(k, sizeof(k), "hud_hidevs_%d", i); settings::set_i(k, static_cast<int>(fold(vh)));
            snprintf(k, sizeof(k), "hud_hidevs_%d_w", i); settings::set_i(k, static_cast<int>(w));
            snprintf(k, sizeof(k), "hud_hidevs_%d_h", i); settings::set_i(k, static_cast<int>(h));
        }
        BVR_LOG("hudhide vs: %d rule(s) (saved) | hidden so far %u", g_hideVsN.load(), g_hiddenByVs.load());
        return;
    }
    if (args && strchr(args, 'x') == nullptr && args[0] && _strnicmp(args, "off", 3)) { tex_list_cmd("hudhide", args, g_hideTex, g_hideTexN, "hud_hide"); return; }
    if (args && sscanf_s(args, "%ux%u", &w, &h) == 2) { g_hideW.store(w); g_hideH.store(h); g_hideOn.store(1); }
    else g_hideOn.store(0);
    BVR_LOG("hudhide %s: HUD pieces with a %ux%u texture hidden (so far %u)", g_hideOn.load() ? "ON" : "off", g_hideW.load(), g_hideH.load(), g_hidden.load());
}
void texdump(const char* args) {
    int n = 0;
    const char* p = args ? args : "";
    while (*p && n < kTexDump) {
        while (*p == ' ') ++p;
        if (!*p) break;
        g_texDumpHash[n] = static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 16));
        g_texDumpSrv[n].store(nullptr);
        ++n;
    }
    g_texDumpN.store(n);
    BVR_LOG("texdump: waiting for %d HUD texture(s) - each is saved the next time it is drawn", n);
}
void boxvis(int on) {
    if (on >= 0) g_boxVis.store(on ? 1 : 0);
    BVR_LOG("boxvis %d: scenery visibility reads seen %u, zero answers turned visible %u", g_boxVis.load(), g_boxVisSeen.load(),
            g_boxVisForced.load());
}
void boxprobe(int frames) {
    g_boxLogged.store(0);
    for (int i = 0; i < kBoxDsv; ++i) { g_boxDsv[i].store(nullptr); g_boxDsvN[i].store(0); }
    g_boxFrames.store(frames > 0 ? frames : 4);
    BVR_LOG("boxprobe: the scenery visibility boxes and their occlusion reads, %d frames", frames > 0 ? frames : 4);
}
void occprobe(int frames) { g_occReads.store(0);g_occLogged.store(0); g_occFrames.store(frames > 0 ? frames : 3); BVR_LOG("occprobe: logging occlusion results"); }
void occscale(float k) {
    g_occScale.store(k != 0.0f ? k : 1.0f);
    BVR_LOG("occscale: %s | occlusion results read %u, predicated draws set %u, predicates dropped %u",
            k < 0.0f ? "EVERY object reads as visible, predication off" : "occlusion results scaled", g_occReads.load(), g_predCalls.load(),
            g_predDropped.load());
}
void shaftprobe(int frames) { g_shaftSeen.store(0); g_shaftFrames.store(frames > 0 ? frames : 2); BVR_LOG("shaftprobe: counting light-shaft draws"); }
void sunprobe(int frames) { g_sunProbe.store(frames > 1 ? frames : 2); BVR_LOG("sunprobe: reading the god-ray passes' constants"); }
void skip_ps(uint64_t h) {
    g_skipPs.store(h);
    BVR_LOG("skipps: %s%016llx (dropped so far %u)", h ? "dropping every draw (immediate and recorded) of pixel shader " : "off ", (unsigned long long)h, g_skipped.load());
}
void postlog(int frames) {
    g_postFrames.store(frames > 0 ? frames : 1);
    BVR_LOG("postlog: mapping the next %d frames' full-screen passes and picture writes", g_postFrames.load());
}
void hudtex(int frames) {
    { std::lock_guard<std::mutex> lk(g_hudTexMx); g_hudTexRows.clear(); }
    g_hudTexFrames.store(frames > 0 ? frames : 3);
    BVR_LOG("hudtex: listing the pieces drawn into the shared picture for %d frames", g_hudTexFrames.load());
}
int hud_redirect_on() { return g_hudRedirect.load(); }
void hud_sheet_stats(uint32_t& draws, uint32_t& byHash, uint32_t& bySig, uint32_t& lists) {
    draws = g_hudRedirected.load(); byHash = g_hudByHash.load(); bySig = g_hudBySig.load(); lists = g_hudLists.load();
}
void hud_sheet_stats2(uint32_t& world, uint32_t& imm, uint32_t& inst) { world = g_hudWorld.load(); imm = g_hudImm.load(); inst = g_hudInst.load(); }
void set_hud_draw(HudDrawFn fn) { g_hudDraw = fn; }
void hud_stats(uint32_t& seen, uint32_t& noPlacement, uint32_t& taken) {
    seen = g_hudDraws.load(); noPlacement = g_hudNoCb.load(); taken = g_hudTaken.load();
}
void hud_probe(int frames) { g_hudProbe.store(frames > 0 ? frames : 2); BVR_LOG("hudprobe: watching the next %d frames", g_hudProbe.load()); }
uint32_t hud_armed() { return g_hudArmed.load(); }

void jitter_size_from_target() {
    ID3D11Texture2D* T = g_T.load(std::memory_order_relaxed);
    if (!T) return;
    D3D11_TEXTURE2D_DESC d{}; T->GetDesc(&d);
    if (d.Width && d.Height) { g_vpW.store(d.Width); g_vpH.store(d.Height); }
}
void jitter_stats(uint32_t& shifted, uint32_t& kept) { shifted = g_vpShifted.exchange(0); kept = g_vpKept.exchange(0); }

// ---- DLSS (M6) ----
void dlss_want(bool depth) { g_depthWant.store(depth); }
// AFW (V7): one view a frame, so at the Present the shared final picture IS the drawn eye's (the post chain finished it at
// the frame's last write, where the native split takes the right eye too). Its depth is copied now, into slot 0.
ID3D11Texture2D* afw_grab(ID3D11DeviceContext* ctx, ID3D11Resource** depth) {
    if (depth) {
        *depth = nullptr;
        if (g_depthSrc && ensure_depth(ctx, 0)) {
            ctx->CopyResource(g_depthCopy[0], g_depthSrc);
            g_depthCopies[0].fetch_add(1, std::memory_order_relaxed);
            *depth = g_depthCopy[0];
        }
    }
    return g_T.load(std::memory_order_relaxed);
}
void want_view_proj(bool on) { g_vpWant.store(on); }
void set_depth(ID3D11Resource* shared) { g_depthSrc = shared; }
ID3D11Texture2D* eye_depth(int eye) { return eye == 0 || eye == 1 ? g_depthCopy[eye] : nullptr; }
bool view_proj(int view, float out[16], float screen[4]) {
    if (view != 0 && view != 1) return false;
    std::lock_guard<std::mutex> lk(g_vpMutex);
    if (!g_viewVP[view].ok) return false;
    memcpy(out, g_viewVP[view].m, sizeof(g_viewVP[view].m));
    if (screen) memcpy(screen, g_viewVP[view].screen, sizeof(g_viewVP[view].screen));
    return true;
}
void vp_check(int n) { g_vpCheck.store(n); }
void vp_pick(int on, float tol) {
    g_vpPick.store(on ? 1 : 0); if (tol > 0.0f) g_vpTol.store(tol);
    settings::set_i("vp_pick", on ? 1 : 0);
    BVR_LOG("vppick: %s (tolerance %.3f)", on ? "ON - each eye keeps only matrices with its own lens" : "off - last write at the eye", g_vpTol.load());
}
const char* dlss_status() {
    static char s[320];
    snprintf(s, sizeof(s), "view-projection kept L %u R %u (camera not at an eye %u, not a perspective camera %u, "
                           "could not be inverted %u, another lens L %u R %u, vppick %d) | depth copies L %u R %u",
             g_vpSeen[0].load(), g_vpSeen[1].load(), g_vpUnsure.load(), g_vpRejOrtho.load(), g_vpRejSing.load(),
             g_vpRejLens[0].load(), g_vpRejLens[1].load(), g_vpPick.load(), g_depthCopies[0].load(), g_depthCopies[1].load());
    return s;
}
void upd_who(int n) { g_updWho.store(n); }
void shadowx_test(int m) { g_sxTest.store(m); BVR_LOG("shadowx test %d", m); }
void scene_dump(int writes, unsigned bytes) { g_dumpBytes.store(bytes ? bytes : kSceneBytes); g_sceneDump.store(writes); BVR_LOG("scenedump: the next %d writes of cbSceneParam will be logged whole", writes); }
bool shadow_scene_fix(float* d) { return rebase_cascades(d, 38, 1); }
// The MAX screen-shadow pass 10ada77c binds at b0 a 528-byte copy of cbSceneParam rows 0-32, whose rows 31-32
// (VC_ShadowMapMatrix, cascade 0 WORLD -> shadow) are per view: our view's eye write gets the game view's rows.
std::atomic<uint32_t> g_headFixed{0}, g_headCalls{0}, g_headNear{0};
float g_headLast[3] = {};
bool shadow_head_fix(float* d) {
    // The MAX screen-shadow pass's own 528-byte constants (b0): rows 8-19 = cascades 1-3 (3x4 view -> shadow, equal to
    // cbSceneParam rows 42-53), row 20 the split distances. No camera in it: a write whose cascades equal OUR view's
    // originals is ours and gets the re-based ones.
    g_headCalls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_matMutex);
    if (!g_oursOk || (g_sxTest.load(std::memory_order_relaxed) & 16)) return false;
    float worst = 0.0f;
    for (int r = 0; r < 12; ++r) for (int c = 0; c < 4; ++c) worst = (std::max)(worst, fabsf(d[(8 + r) * 4 + c] - g_oursOrig[r][c]));
    if (worst > 1e-6f) return false;
    g_headNear.fetch_add(1, std::memory_order_relaxed);
    memcpy(d + 8 * 4, g_oursNew, sizeof(g_oursNew));
    g_headFixed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void cb_dump(int writes) { g_cbDump.store(writes); BVR_LOG("cbdump: the next %d writes will be logged whole", writes); }

void cb_spy(bool on, uint32_t bytes) {
    if (bytes) g_cbWant.store(bytes);
    g_cbSpy.store(on);
    BVR_LOG("cbspy: watching constant buffers of %u bytes - %s", g_cbWant.load(), on ? "ON" : "off");
}

void set_hdr_swap(ID3D11Resource* src, ID3D11Resource* dst) {
    g_hdrSrc.store(src, std::memory_order_relaxed);
    g_hdrDst.store(dst, std::memory_order_relaxed);
}
uint32_t hdr_swap_copies() { return g_hdrCopies.load(std::memory_order_relaxed); }
uint32_t pair_unstable() { return g_pairUnstable.load(std::memory_order_relaxed); }

void set_pair_indices(int left, int right, int writes) {
    g_leftIdx.store(left); g_rightIdx.store(right); g_learnedN.store(writes);
    g_pairMode.store(3);
    BVR_LOG("picsplit: learned the frame's shape - the game's view is finished at write %d of %d, ours at write %d; "
            "each eye is copied there from now on", left, writes, right);
}

void set_pair_mode(int m) {
    g_pairMode.store(m);
    static const char* kHow[4] = {"that view's last TAGGED write (old rule)", "the LAST write of that view's run (sticky)",
                                  "the half-way write and the last write of the frame (two draws of one view)",
                                  "the learned write numbers"};
    BVR_LOG("picsplit: each eye's picture is copied after %s", kHow[m >= 0 && m <= 3 ? m : 1]);
}

void on_present() {
    if (const int left = g_hudProbe.load(std::memory_order_relaxed); left > 0) {
        if (g_hudProbe.fetch_sub(1) == 1) tally_report();
    }
    shadow_diff_frame();
    if (g_srLog.load(std::memory_order_relaxed) > 0 && g_srLog.fetch_sub(1) == 1) BVR_LOG("shadowres: viewport log done");
    g_frameNoFwd.fetch_add(1, std::memory_order_relaxed);
    const int n = g_writesThisFrame;
    // Pair mode 4 (DS3): take the next frame's two capture points from this frame's structure - the last write that
    // carried a view tag (the game view's finished picture still stands there) and the last write of all (the post
    // chain has by then replaced it with our view's). Only accepted when the frame really had both, so a frame with
    // one view or a loading screen leaves the last good pair alone.
    if (g_pairMode.load(std::memory_order_relaxed) == 4 && n >= 2 && g_lastTaggedWrite >= 1 && g_lastTaggedWrite < n) {
        const int wasL = g_leftIdx.load(std::memory_order_relaxed), wasR = g_rightIdx.load(std::memory_order_relaxed);
        const int candL = g_lastTaggedWrite, candR = n;
        // LOCKVOTE (V8, 2026-10-31: two sessions locked the LEFT eye at write 9 of 12 = our view's picture, "IDENTICAL eyes",
        // JJ: left eye displaced / sheared; a third locked at 8 and was fine). The first frame's structure was taken and the
        // lock only moves after kLockFrames identical frames in a row - which never happens while the structure flaps (639
        // changes in 10 s with motion blur on). Now every 60 frames the lock takes the EARLIEST left write that held in at
        // least a quarter of them: the left eye must be copied before our view's picture lands, so of the frequent
        // candidates the earliest is the safe one.
        if (candL > 0 && candL < 32) ++g_voteL[candL];
        if (++g_voteN >= 60) {
            int pick = 0;
            for (int k = 1; k < 32 && !pick; ++k) if (g_voteL[k] * 4 >= g_voteN) pick = k;
            if (pick && (pick != wasL || candR != wasR)) {
                g_leftIdx.store(pick, std::memory_order_relaxed);
                g_rightIdx.store(candR, std::memory_order_relaxed);
                g_lockMoves.fetch_add(1, std::memory_order_relaxed);
                BVR_LOG("picsplit: capture points set to writes %d and %d of %d by vote (left candidates over %d frames:%s%s)", pick,
                        candR, n, g_voteN, g_voteL[8] ? " 8" : "", g_voteL[9] ? " 9" : "");
            }
            memset(g_voteL, 0, sizeof(g_voteL));
            g_voteN = 0;
            g_candRun = 0;
        } else if (wasL <= 0 || wasR <= 0) {                // nothing locked yet: the first structure until the first vote
            g_leftIdx.store(candL, std::memory_order_relaxed);
            g_rightIdx.store(candR, std::memory_order_relaxed);
        } else if (false && (candL != wasL || candR != wasR)) {   // (the old run rule, replaced by the vote)
            g_pairUnstable.fetch_add(1, std::memory_order_relaxed);
            if (candL == g_candL && candR == g_candR) ++g_candRun; else { g_candL = candL; g_candR = candR; g_candRun = 1; }
            if (g_candRun >= kLockFrames) {                 // a genuinely new structure (another area, a menu)
                g_leftIdx.store(candL, std::memory_order_relaxed);
                g_rightIdx.store(candR, std::memory_order_relaxed);
                g_lockMoves.fetch_add(1, std::memory_order_relaxed);
                BVR_LOG("picsplit: capture points moved to writes %d and %d of %d (held %d frames)", candL, candR, n, g_candRun);
                g_candRun = 0;
            }
        } else {
            g_candRun = 0;
        }
    }
    g_lastTaggedWrite = 0;
    g_writesThisFrame = 0;
    g_frameNo.fetch_add(1, std::memory_order_relaxed);
    if (g_immSpy.load() > 0) {
        BVR_LOG("immspy: ---- Present (frame had %u immediate maps, %u immediate / %u deferred UpdateSubresource of instance blocks) ----",
                g_immCount, g_immCountUpd, g_immCountDefUpd);
        g_immCount = g_immCountUpd = g_immCountDefUpd = 0;
        if (g_immSpy.fetch_sub(1) == 1) upd_stacks_log();
    }
    g_execN = 0;
    if (g_instSpy.load() > 0) {
        std::lock_guard<std::mutex> lk(g_instMutex);
        BVR_LOG("instspy frame: envData per view (0..6, >6): GAME %u %u %u %u %u %u %u %u | OURS %u %u %u %u %u %u %u %u | view unknown %u",
                g_instHist[0][0], g_instHist[0][1], g_instHist[0][2], g_instHist[0][3], g_instHist[0][4], g_instHist[0][5], g_instHist[0][6], g_instHist[0][7],
                g_instHist[1][0], g_instHist[1][1], g_instHist[1][2], g_instHist[1][3], g_instHist[1][4], g_instHist[1][5], g_instHist[1][6], g_instHist[1][7],
                g_instUnknownView);
        for (auto& h : g_instHist) for (auto& x : h) x = 0;
        g_instUnknownView = 0;
        if (g_instSpy.fetch_sub(1) == 1) {
            HMODULE exe = GetModuleHandleW(nullptr);
            for (int i = 0; i < g_instCallerN; ++i)
                BVR_LOG("instspy caller ds3+0x%llx: GAME %u (envData>1: %u) | OURS %u (envData>1: %u)",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_instCallers[i].ret) - reinterpret_cast<uintptr_t>(exe)),
                        g_instCallers[i].n[0], g_instCallers[i].bad[0], g_instCallers[i].n[1], g_instCallers[i].bad[1]);
            g_instCallerN = 0;
        }
    }
    if (g_bindSpy.load() > 0 && g_bindSpy.fetch_sub(1) == 1 && g_inSlot.load() < 0) g_inHash.store(0);
    if (g_writeFrames.load() > 0 && g_writeQuiet.load()) {
        g_stageLog = false;
        if (g_writeFrames.fetch_sub(1) == 1) { g_writeWatch.store(nullptr); g_writeWatchB.store(nullptr); g_stageK.store(0); g_writeQuiet.store(false); }
    } else if (g_writeFrames.load() > 0) {
        std::lock_guard<std::mutex> lk(g_writeMutex);
        char line[600]; int k = sprintf_s(line, "writers of %p this frame:", g_writeWatch.load());
        for (int i = 0; i < g_writerN && k < 560; ++i)
            k += sprintf_s(line + k, sizeof(line) - k, " %s %016llx (%s) x%u;", g_writers[i].cs ? "CS" : "PS",
                           static_cast<unsigned long long>(g_writers[i].hash),
                           bvr::ds3::shaderid::name_of(g_writers[i].hash) ? bvr::ds3::shaderid::name_of(g_writers[i].hash) : "-", g_writers[i].n);
        BVR_LOG("%s", line);
        g_writerN = 0;
        if (g_writeFrames.fetch_sub(1) == 1) g_writeWatch.store(nullptr);
    }
    if (g_histSpy.load() > 0 && g_histSpy.fetch_sub(1) == 1) BVR_LOG("histspy: done");
    else if (g_histSpy.load() > 0) BVR_LOG("histspy: ---- frame boundary (Present) ----");
    g_owner = 0;
    if (g_sunTrace.load() > 0) {
        std::lock_guard<std::mutex> lk(g_cbMutex);
        char line[300]; int k = sprintf_s(line, "suntrace frame %u:", g_frames);
        for (int i = 0; i < g_sunN && k < 260; ++i) {
            const SunWrite& w = g_sunWrites[i];
            const float len = w.dir[0] * w.dir[0] + w.dir[1] * w.dir[1] + w.dir[2] * w.dir[2];
            k += sprintf_s(line + k, sizeof(line) - k, "  w%d %s(%.3f %.3f %.3f) cascade %.3g split %.3g", i + 1,
                           len < 0.01f ? "NO SUN " : "", w.dir[0], w.dir[1], w.dir[2], w.c0, w.sel);
        }
        BVR_LOG("%s", line);
        g_sunN = 0;
        if (g_sunTrace.fetch_sub(1) == 1) BVR_LOG("suntrace: done");
    }
    if (g_cbSpy.load()) {
        std::lock_guard<std::mutex> lk(g_cbMutex);
        static uint32_t last = 0;
        if (g_frames - last >= 300) { last = g_frames; BVR_LOG("cbspy: %d writes of the per-view constant buffer in this frame", g_cbWrites); }
        g_cbWrites = 0;
        g_cbLogged = 0;
    }
    if (!g_on.load()) return;
    ++g_frames;
    ++g_hist[n < 64 ? n : 64];
    if (g_pairMode.load() == 3 && g_learnedN.load() && n && n != g_learnedN.load()) {
        BVR_LOG("picsplit: the frame's shape changed (%d writes, learned %d) - back to following the view labels until it is learned again",
                n, g_learnedN.load());
        g_pairMode.store(1);
        g_learnedN.store(0);
    }
    g_pairThisFrame = g_gotL && g_gotR;
    if (g_gotL && g_gotR) {                      // a complete pair: each view's own write was seen this frame
        std::lock_guard<std::mutex> lk(g_pairMutex);
        g_left = g_copy[0];
        g_right = g_copy[1];
        g_pairL = g_idxL; g_pairR = g_idxR; g_pairWrites = n;
        ++g_pairs;
    } else if (g_pair.load() && n) ++g_odd;
    g_gotL = g_gotR = false;
    g_expect = n;
    if (g_dumpState.load() == 1 && g_dumpN) g_dumpState.store(2);          // the armed frame is complete
    else if (g_dumpState.load() == 3) { g_dumpN = 0; g_dumpState.store(1); } // armed: the NEXT frame is recorded whole
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 5000) {
        last = now;
        if (g_histSwap.load() || g_volLend.load()) {
            char line[400]; int k = sprintf_s(line, "histswap %d / vollend %d in 5 s: restored %u, saved %u, first %u, twice %u, view unknown %u, "
                                              "shape mismatch %u, vollend gave %u | passes (bound/ran):", g_histSwap.load(), g_volLend.load(),
                                              g_hsRestore.exchange(0), g_hsSave.exchange(0), g_hsFirst.exchange(0), g_hsTwice.exchange(0),
                                              g_hsUnknown.exchange(0), g_hsShape.exchange(0), g_volGave.exchange(0));
            for (int p = 0; p < kPassN && k < 380; ++p)
                k += sprintf_s(line + k, sizeof(line) - k, " %s %u/%u;", kPasses[p].name, g_passBound[p].exchange(0), g_passRan[p].exchange(0));
            BVR_LOG("%s", line);
        }
        if (shadowres::active())
            BVR_LOG("shadowres: in 5 s %u binds of an enlarged map (%u on the immediate context), %u viewports and %u scissors "
                    "scaled, %u copies touching one | %s", g_srBinds.exchange(0), g_srImm.exchange(0), g_srVp.exchange(0),
                    g_srSc.exchange(0), g_srCopies.load(), shadowres::status());
        if (contact::on()) BVR_LOG("contact: %s", contact::status());
        if (g_shadowLend.load())
            BVR_LOG("shadowlend: the main view's maps were bound %u times, ours %u times (%u of them had no partner yet) in 5 s; "
                    "lookups past a full table %u", g_lendTook.exchange(0), g_lendGave.exchange(0) + g_lendSkipped.load(),
                    g_lendSkipped.exchange(0), g_shFull.exchange(0));
        if (lend3())
            BVR_LOG("shadowx: our reads of X swapped to the game's %u times (no partner %u) | volumetric buffer: game %u, ours re-based "
                    "%u, stale %u, eyes together %u | cbSceneParam: game %u, ours re-based %u, stale %u, eyes together %u",
                    g_xGave.exchange(0), g_xMiss.exchange(0), g_rbGame[0].exchange(0), g_rbFixed[0].exchange(0),
                    g_rbStale[0].exchange(0), g_rbUnsure[0].exchange(0), g_rbGame[1].exchange(0), g_rbFixed[1].exchange(0),
                    g_rbStale[1].exchange(0), g_rbUnsure[1].exchange(0));
        if (lend3()) {
            float e0[3]; { std::lock_guard<std::mutex> lk(g_eyeMutex); memcpy(e0, g_eyePos[0], sizeof(e0)); }
            BVR_LOG("shadowx: value substitution - scans %u, hits cbSceneParam %u, 528 B %u, 256 B %u, other %u", g_subScans.exchange(0),
                    g_subHits[0].exchange(0), g_subHits[1].exchange(0), g_subHits[2].exchange(0), g_subHits[3].exchange(0));
            BVR_LOG("shadowx: 528-byte screen-shadow constants seen %u, ours re-based %u", g_headCalls.exchange(0),
                    g_headFixed.exchange(0) + 0 * g_headNear.exchange(0) + 0 * static_cast<uint32_t>(e0[0]));
        }
        char h[160]; int k = 0;
        for (int i = 0; i <= 64 && k < 140; ++i) if (g_hist[i]) k += sprintf_s(h + k, sizeof(h) - k, " %d:%d", i, g_hist[i]);
        snprintf(g_status, sizeof(g_status), "picsplit: frames %u, pairs %u, odd %u | writers per frame (count:frames)%s | tags none %u main %u "
                 "ours %u both %u | binds %u, finishes %u, marked %u", g_frames, g_pairs, g_odd, k ? h : " none",
                 g_tagCount[0].exchange(0), g_tagCount[1].exchange(0), g_tagCount[2].exchange(0), g_tagCount[3].exchange(0),
                 g_tBinds.exchange(0), g_finishes.exchange(0), g_marks.exchange(0));
        g_rtCalls.store(0);
        BVR_LOG("%s", g_status);
        {
            std::lock_guard<std::mutex> lk(g_inMutex);
            for (int k = 0; k < g_inputN; ++k) {
                static const char* kWho[4] = {"no view", "main", "ours", "both"};
                BVR_LOG("picsplit:   the pass writing the picture reads t%u = %p %ux%u fmt %u (%u times, %s)", g_inputs[k].slot, g_inputs[k].r,
                        g_inputs[k].w, g_inputs[k].h, g_inputs[k].fmt, g_inputs[k].hits, kWho[g_inputs[k].tag & 3]);
            }
            g_inputN = 0;
        }
        g_frames = g_pairs = g_odd = 0;
        for (int& x : g_hist) x = 0;
    }
}

void dump_arm() { g_dumpState.store(3); }
int dump_ready() { return g_dumpState.load() == 2 ? g_dumpN : 0; }
ID3D11Texture2D* dump_tex(int i) { return i >= 0 && i < kDump ? g_dump[i] : nullptr; }
void dump_done() { g_dumpState.store(0); }
ID3D11Texture2D* left() { std::lock_guard<std::mutex> lk(g_pairMutex); return g_left; }
ID3D11Texture2D* right() { std::lock_guard<std::mutex> lk(g_pairMutex); return g_right; }
const char* status() { return g_status; }
void pair_index(int& l, int& r, int& writes) { std::lock_guard<std::mutex> lk(g_pairMutex); l = g_pairL; r = g_pairR; writes = g_pairWrites; }

// ---- L7 controls ----
void hist_spy(int frames) {
    bvr::ds3::shaderid::report();
    g_histSpy.store(frames);
    BVR_LOG("histspy: logging every watched pass (geo11 names) in GPU order for %d frames", frames);
}
void hist_swap(int mask) {
    for (auto& v : g_save) for (auto& p : v) for (auto& k : p) k.valid = false;   // never restore an old save
    g_histSwap.store(mask);
    BVR_LOG("histswap %d: each view keeps its own history for%s%s%s%s%s", mask, mask & 1 ? " the sun-shadow volume," : "",
            mask & 2 ? " the fog volume," : "", mask & 4 ? " AO," : "", mask & 8 ? " TAA," : "", mask ? "" : " nothing (off)");
}
void vol_lend(int on) {
    g_volLend.store(on ? 1 : 0);
    BVR_LOG("vollend: our view's volumetric light %s", on ? "is built from the MAIN view's sun shadow map (compute t3)"
                                                         : "uses its own (empty) sun shadow map");
}
void set_hist_own(int view, ID3D11Resource* const* res, int n) {
    if (view < 0 || view > 1) return;
    for (int i = 0; i < kHistOwn; ++i) g_histOwn[view][i].store(i < n ? res[i] : nullptr, std::memory_order_relaxed);
}

// ---- EYESTAB: per-frame picture change of each eye's finished picture (the L6 number, measured in the mod) ----
namespace {
std::atomic<int> g_stabWant{0};
ID3D11Texture2D* g_stage[2][3] = {};
int g_stageN = 0;
uint32_t g_stageFrame[3] = {};
bool g_stabLog = false;              // LIGHTSRC: log each frame's eye brightness with its frame number
std::vector<uint8_t> g_stabPrev[2];
double g_stabSum[2] = {}, g_stabMax[2] = {};
int g_stabCount[2] = {};
double g_stabMean[2] = {}, g_stabPeak[2] = {};
bool g_stabDone = false;
// HISTAB: {vollend, histswap} per step; the control at both ends shows any drift during the run
constexpr int kAb[][2] = {{0, 0}, {1, 0}, {1, 1}, {1, 3}, {1, 7}, {1, 15}, {0, 15}, {0, 0}};
constexpr int kAbN = sizeof(kAb) / sizeof(kAb[0]);
int g_abStep = -1, g_abPhase = 0, g_abSettle = 0, g_abKeepSwap = 0, g_abKeepVol = 0;

bool sample(ID3D11DeviceContext* ctx, ID3D11Texture2D* st, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC d{}; st->GetDesc(&d);
    const bool ten = d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return false;
    constexpr UINT step = 8;
    const UINT w = d.Width / step, h = d.Height / step;
    out.resize(static_cast<size_t>(w) * h * 3);
    for (UINT y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * step * m.RowPitch);
        for (UINT x = 0; x < w; ++x) {
            const uint32_t q = row[x * step];
            uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 3];
            if (ten) { o[0] = static_cast<uint8_t>((q & 0x3FF) >> 2); o[1] = static_cast<uint8_t>(((q >> 10) & 0x3FF) >> 2); o[2] = static_cast<uint8_t>(((q >> 20) & 0x3FF) >> 2); }
            else { o[0] = static_cast<uint8_t>(q); o[1] = static_cast<uint8_t>(q >> 8); o[2] = static_cast<uint8_t>(q >> 16); }
        }
    }
    ctx->Unmap(st, 0);
    return true;
}
} // namespace

void eye_stab(int frames) {
    for (int e = 0; e < 2; ++e) { g_stabPrev[e].clear(); g_stabSum[e] = g_stabMax[e] = 0; g_stabCount[e] = 0; }
    g_stageN = 0;
    g_stabDone = false;
    g_stabWant.store(frames);
    if (g_abStep < 0) BVR_LOG("eyestab: measuring each eye's per-frame picture change for %d frames", frames);
}

void hist_ab() {
    g_abKeepSwap = g_histSwap.load(); g_abKeepVol = g_volLend.load();
    g_abStep = 0; g_abPhase = 0;
    BVR_LOG("histab: %d steps of {vollend, histswap}, 1 s settle + 2 s measure each (stand still; ~25 s)", kAbN);
}

namespace {
ID3D11Texture2D* g_pStage[2][kProbeIn][3] = {};
std::vector<uint8_t> g_pPrev[2][kProbeIn];
double g_pChanged[2][kProbeIn] = {}, g_pDiff[2][kProbeIn] = {};
int g_pCount[2][kProbeIn] = {};
int g_pStageN = 0;
double g_pCross[kProbeIn] = {}, g_pCrossTop[kProbeIn] = {}; int g_pCrossN[kProbeIn] = {};   // game vs ours, same frame
std::vector<uint8_t> g_pCur[2];
FILE* g_probeDumpArr[2] = {};
FILE** g_probeDump = nullptr;           // LITPROBE dump: the sampled frames of the probed input, per view
int g_probeDumpN[2] = {}; UINT g_probeDumpW = 0, g_probeDumpH = 0;

UINT bytes_per_texel(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM: return 2;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_A8_UNORM: return 1;
    default: return 4;
    }
}

// Every 8th texel: the fraction whose bits changed since the previous frame, and the mean change of its top byte.
bool probe_sample(ID3D11DeviceContext* ctx, ID3D11Texture2D* st, std::vector<uint8_t>& out) {
    D3D11_TEXTURE2D_DESC d{}; st->GetDesc(&d);
    const UINT bpt = bytes_per_texel(d.Format), take = bpt < 4 ? bpt : 4;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return false;
    constexpr UINT step = 8;
    const UINT w = d.Width / step, h = d.Height / step;
    out.resize(static_cast<size_t>(w) * h * take);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * step * m.RowPitch;
        for (UINT x = 0; x < w; ++x) memcpy(&out[(static_cast<size_t>(y) * w + x) * take], row + static_cast<size_t>(x) * step * bpt, take);
    }
    ctx->Unmap(st, 0);
    return true;
}

void probe_measure(ID3D11DeviceContext* ctx) {
    if (g_probeWant.load() <= 0 || !ctx) return;
    const int w = g_pStageN % 3;
    for (int v = 0; v < 2; ++v)
        for (int k = 0; k < kProbeIn; ++k) {
            if (g_probeOnly.load() >= 0 && k != g_probeOnly.load()) continue;
            ID3D11Texture2D* src = nullptr;
            ID3D11Resource* from = g_probeOwn.load() ? (k < kHistOwn ? g_histOwn[v][k].load() : nullptr) : g_probe[v][k];
            if (g_probeOwn.load()) g_probeSrc[v][k] = from;
            if (!from || FAILED(from->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src))) || !src) continue;
            D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
            for (auto& st : g_pStage[v][k]) {
                if (st) {
                    D3D11_TEXTURE2D_DESC c{}; st->GetDesc(&c);
                    if (c.Width == d.Width && c.Height == d.Height && c.Format == d.Format && c.MipLevels == d.MipLevels && c.ArraySize == d.ArraySize) continue;
                    st->Release(); st = nullptr;
                }
                D3D11_TEXTURE2D_DESC sd = d;
                sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
                if (dev) { dev->CreateTexture2D(&sd, nullptr, &st); dev->Release(); }
            }
            if (g_pStage[v][k][w]) ctx->CopyResource(g_pStage[v][k][w], src);
            src->Release();
        }
    ++g_pStageN;
    if (g_pStageN >= 3) {
        const int r = (g_pStageN - 3) % 3;
        g_pCur[0].clear(); g_pCur[1].clear();
        for (int v = 0; v < 2; ++v)
            for (int k = 0; k < kProbeIn; ++k) {
                if (g_probeOnly.load() >= 0 && k != g_probeOnly.load()) continue;
                std::vector<uint8_t> cur;
                if (!g_pStage[v][k][r] || !probe_sample(ctx, g_pStage[v][k][r], cur)) { g_pPrev[v][k].clear(); continue; }
                if (g_probeDump && g_probeDump[v]) {   // every sampled frame, raw (every 8th texel, first 4 bytes)
                    D3D11_TEXTURE2D_DESC d{}; g_pStage[v][k][r]->GetDesc(&d);
                    fwrite(cur.data(), 1, cur.size(), g_probeDump[v]);
                    ++g_probeDumpN[v];
                    g_probeDumpW = d.Width / 8; g_probeDumpH = d.Height / 8;
                }
                if (g_probeOnly.load() == k) g_pCur[v] = cur;   // CROSS: compare the two views on this frame
                std::vector<uint8_t>& prev = g_pPrev[v][k];
                if (prev.size() == cur.size() && !cur.empty()) {
                    D3D11_TEXTURE2D_DESC d{}; g_pStage[v][k][r]->GetDesc(&d);
                    const UINT bpt = bytes_per_texel(d.Format), take = bpt < 4 ? bpt : 4;
                    size_t changed = 0; double top = 0;
                    const size_t n = cur.size() / take;
                    for (size_t i = 0; i < n; ++i) {
                        if (memcmp(&cur[i * take], &prev[i * take], take)) ++changed;
                        top += std::abs(int(cur[i * take + take - 1]) - int(prev[i * take + take - 1]));
                    }
                    g_pChanged[v][k] += n ? 100.0 * changed / n : 0;
                    g_pDiff[v][k] += n ? top / n : 0;
                    ++g_pCount[v][k];
                }
                prev.swap(cur);
            }
        if (const int k = g_probeOnly.load(); k >= 0 && k < kProbeIn && !g_pCur[0].empty() && g_pCur[0].size() == g_pCur[1].size()) {
            D3D11_TEXTURE2D_DESC d{}; if (g_pStage[0][k][r]) g_pStage[0][k][r]->GetDesc(&d);
            const UINT bpt = bytes_per_texel(d.Format), take = bpt < 4 ? bpt : 4;
            const size_t n = g_pCur[0].size() / take;
            double all = 0, top = 0;
            for (size_t i = 0; i < n * take; ++i) all += std::abs(int(g_pCur[0][i]) - int(g_pCur[1][i]));
            for (size_t i = 0; i < n; ++i) top += std::abs(int(g_pCur[0][i * take + take - 1]) - int(g_pCur[1][i * take + take - 1]));
            g_pCross[k] += all / (n * take); g_pCrossTop[k] += top / n; ++g_pCrossN[k];
        }
    }
    if (g_probeWant.load() == 1 && g_probeDump) {
        for (int v = 0; v < 2; ++v) if (g_probeDumpArr[v]) { fclose(g_probeDumpArr[v]); g_probeDumpArr[v] = nullptr; }
        BVR_LOG("litprobe: dumped %d / %d frames of %ux%u texels (game / ours) into captures\litprobe_*.raw", g_probeDumpN[0], g_probeDumpN[1],
                g_probeDumpW, g_probeDumpH);
        g_probeDump = nullptr;
    }
    if (g_probeWant.fetch_sub(1) == 1) {
        if (g_inSlot.load() >= 0) { BVR_LOG("inprobe: done (shader %016llx input t%d)", static_cast<unsigned long long>(g_inHash.load()), g_inSlot.load()); g_inSlot.store(-1); g_inHash.store(0); }
        if (g_ppHash.load()) { BVR_LOG("passprobe: done (%u draws of it whose view could not be told)", g_ppUnknown.load()); g_ppHash.store(0); }
        if (g_probeOwn.load()) BVR_LOG("ownprobe: each view's OWN texture at Present, frame to frame (index = the per-view path list in stereo.cpp)");
        else BVR_LOG("litprobe: each input of the lighting pass, frame to frame (texels changed %% / mean top-byte change; same texture in both views = SHARED)");
        for (int k = 0; k < kProbeIn; ++k) {
            if (g_probeOnly.load() >= 0 && k != g_probeOnly.load()) continue;
            const int a = g_pCount[0][k], b = g_pCount[1][k];
            ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
            ID3D11Resource* shown = g_probeOwn.load() ? g_probeSrc[0][k] : g_probe[0][k];
            if (shown && SUCCEEDED(shown->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) { t->GetDesc(&d); t->Release(); }
            if (g_pCrossN[k]) BVR_LOG("litprobe   %-18s BETWEEN the views (same frame): mean byte difference %.2f, top byte %.2f (%d frames)",
                                      g_probeOwn.load() ? "own texture" : kProbeName[k], g_pCross[k] / g_pCrossN[k], g_pCrossTop[k] / g_pCrossN[k], g_pCrossN[k]);
            BVR_LOG("litprobe   %-18s game view %5.1f%% / %5.2f  | ours %5.1f%% / %5.2f  | %ux%u fmt %u, %s (%d/%d frames)",
                    g_probeOwn.load() ? "own texture" : kProbeName[k], a ? g_pChanged[0][k] / a : 0, a ? g_pDiff[0][k] / a : 0, b ? g_pChanged[1][k] / b : 0,
                    b ? g_pDiff[1][k] / b : 0, d.Width, d.Height, d.Format,
                    g_probeSrc[0][k] && g_probeSrc[0][k] == g_probeSrc[1][k] ? "SHARED" : "own per view", a, b);
        }
    }
}
} // namespace

void lit_probe(int frames, int only, bool dump, bool own) {
    for (int k = 0; k < kProbeIn; ++k) { g_pCross[k] = g_pCrossTop[k] = 0; g_pCrossN[k] = 0; }
    g_probeOwn.store(own);
    g_probeOnly.store(only);
    if (dump && only >= 0) {
        for (int v = 0; v < 2; ++v) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\captures\\litprobe_in%d_view%d.raw", bvr::log::data_dir(), only, v);
            _wfopen_s(&g_probeDumpArr[v], path, L"wb");
            g_probeDumpN[v] = 0;
        }
        g_probeDump = g_probeDumpArr;
    }
    for (auto& v : g_pPrev) for (auto& k : v) k.clear();
    for (int v = 0; v < 2; ++v) for (int k = 0; k < kProbeIn; ++k) { g_pChanged[v][k] = g_pDiff[v][k] = 0; g_pCount[v][k] = 0; }
    g_pStageN = 0;
    g_probeWant.store(frames);
    BVR_LOG("litprobe: copying every input of the lighting pass for each view, %d frames", frames);
}

void lightsrc_measure(ID3D11DeviceContext* ctx) {
    if (g_lsWant.load() <= 0 || !ctx) return;
    const uint32_t now = g_frameNo.load();
    if (now < 3) return;
    const uint32_t F = now - 3;                   // recorded two frames ago: the copies are done
    std::lock_guard<std::mutex> lk(g_lsMutex);
    LsFrame& f = g_ls[F % kLsFrames];
    if (f.frame == F) {
        static const char* kV[2] = {"GAME", "OURS"};
        for (int i = 0; i < f.n; ++i) {
            const LsRec& r = f.r[i];
            float b3[40 * 4] = {}, b8[72 * 4] = {};
            float* dst[2] = {b3, b8};
            const UINT cap[2] = {sizeof(b3), sizeof(b8)};
            for (int k = 0; k < 2; ++k) {
                if (!r.cb[k]) continue;
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(r.cb[k], 0, D3D11_MAP_READ, 0, &m))) {
                    D3D11_BUFFER_DESC d{}; r.cb[k]->GetDesc(&d);
                    memcpy(dst[k], m.pData, (std::min)(cap[k], d.ByteWidth));
                    ctx->Unmap(r.cb[k], 0);
                }
            }
            // b3 rows: 3 g_envMapWorld translation, 8 extents, 9 colour/fade, 20 fade rate / diffuse scale, 25 volume sampling
            // b8 rows: 3 SC_CameraPos, 62 pre-exposure
            if (r.pass == kSunPass) {             // cbLight: FC_DirLight[2] {Vec_0-3, Col_0-3, Spc_0-3, AmbientSH[3], AmbientReflection, ...}
                uint32_t lp = 2166136261u;
                for (int q = 0; q < 36 * 16 && q < static_cast<int>(sizeof(b3)); ++q) { lp ^= reinterpret_cast<const uint8_t*>(b3)[q]; lp *= 16777619u; }
                const float* L = b3;
                BVR_LOG("lightsrc frame %u %s sun light block %08x | dir0 %.3f %.3f %.3f %.3f dir1 %.3f %.3f %.3f %.3f | col0 %.3f %.3f %.3f col1 %.3f %.3f %.3f | "
                        "spc0 %.3f %.3f %.3f | SH0 %.3f %.3f %.3f %.3f SH1 %.3f %.3f %.3f %.3f SH2 %.3f %.3f %.3f %.3f | refl %.3f %.3f %.3f | local %u ssao %.3f %.3f",
                        F, kV[r.view & 1], lp, L[0], L[1], L[2], L[3], L[4], L[5], L[6], L[7], L[16], L[17], L[18], L[20], L[21], L[22],
                        L[32], L[33], L[34], L[48], L[49], L[50], L[51], L[52], L[53], L[54], L[55], L[56], L[57], L[58], L[59], L[60], L[61], L[62],
                        reinterpret_cast<const uint32_t*>(L)[64], L[68], L[69]);
                BVR_LOG("lightsrc frame %u %s sun light scene: shadow colour %.3f %.3f %.3f %.3f preexp %.4f camera %.2f %.2f %.2f", F, kV[r.view & 1],
                        b8[29 * 4], b8[29 * 4 + 1], b8[29 * 4 + 2], b8[29 * 4 + 3], b8[62 * 4 + 2], b8[12], b8[13], b8[14]);
                if (L[72 + 0] != 0 || L[72 + 16] != 0)
                    BVR_LOG("lightsrc frame %u %s sun light [1] dir0 %.3f %.3f %.3f %.3f col0 %.3f %.3f %.3f | SH0 %.3f %.3f %.3f %.3f", F, kV[r.view & 1],
                            L[72], L[73], L[74], L[75], L[88], L[89], L[90], L[120], L[121], L[122], L[123]);
                continue;
            }
            if (r.pass == kLightingPass) {        // cbLightAccParam: g_InvProjMatrix rows 0-3, g_ViewMatrix (4x3) rows 4-6
                uint32_t lp = 2166136261u;
                for (int q = 0; q < 23 * 16; ++q) { lp ^= reinterpret_cast<const uint8_t*>(b3)[q]; lp *= 16777619u; }
                BVR_LOG("lightsrc frame %u %s lighting  block %08x | InvProj r0 %.5f %.5f %.5f %.5f r3 %.5f %.5f %.5f %.5f | View r0 %.4f %.4f %.4f %.4f "
                        "r1 %.4f %.4f %.4f %.4f r2 %.4f %.4f %.4f %.4f", F, kV[r.view & 1], lp, b3[0], b3[1], b3[2], b3[3], b3[12], b3[13], b3[14],
                        b3[15], b3[16], b3[17], b3[18], b3[19], b3[20], b3[21], b3[22], b3[23], b3[24], b3[25], b3[26], b3[27]);
                continue;
            }
            uint32_t fp = 2166136261u;            // fingerprint of the whole region block (31 rows)
            for (int q = 0; q < 31 * 16; ++q) { fp ^= reinterpret_cast<const uint8_t*>(b3)[q]; fp *= 16777619u; }
            BVR_LOG("lightsrc frame %u %s %-16s ibl %p vol %p | region %08x at %.1f %.1f %.1f (rows) %.1f %.1f %.1f (cols) size %.1f %.1f %.1f | "
                    "colour %.3f diffuse %.3f fade %.3f %.3f | vol sampling %.3f %.3f %.3f %.3f | camera %.2f %.2f %.2f preexp %.4f",
                    F, kV[r.view & 1], kPasses[r.pass].name, r.ibl, r.vol, fp, b3[12], b3[13], b3[14], b3[3], b3[7], b3[11], b3[32], b3[33], b3[34],
                    b3[36], b3[80 + 1], b3[38], b3[80], b3[100], b3[101], b3[102], b3[103], b8[12], b8[13], b8[14], b8[62 * 4 + 2]);
        }
    }
    if (g_lsWant.fetch_sub(1) == 1) BVR_LOG("lightsrc: done");
}

void inst_readback(ID3D11DeviceContext* ctx) {
    if (g_instState.load() == 1 && g_frameNo.load() > g_instFrame.load() + 1) g_instState.store(2);
    if (g_instState.load() != 2 || !ctx || !g_instBuf) return;
    g_instState.store(0);
    const int n = (std::min)(g_instN.load(), kInstCap);
    D3D11_BUFFER_DESC sd{}; g_instBuf->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    ID3D11Buffer* st = nullptr;
    if (dev) { dev->CreateBuffer(&sd, nullptr, &st); dev->Release(); }
    if (!st) return;
    ctx->CopyResource(st, g_instBuf);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return; }
    const uint8_t* base = static_cast<const uint8_t*>(m.pData);
    HMODULE exe = GetModuleHandleW(nullptr);
    uint32_t hist[2][8] = {};
    int logged = 0;
    for (int i = 0; i < n; ++i) {
        const InstRec& r = g_instRec[i];
        if (r.view < 0) continue;
        const uint32_t e0 = *reinterpret_cast<const uint32_t*>(base + i * kInstSize + 80);
        const uint32_t e1 = *reinterpret_cast<const uint32_t*>(base + i * kInstSize + 272 + 80);
        ++hist[r.view][e0 < 7 ? e0 : 7];
        const float* w = reinterpret_cast<const float*>(base + i * kInstSize);   // mWorld (4x3): translation in .w of rows 0-2
        if (logged < 400)
            BVR_LOG("instcap #%d %s ps %016llx from ds3+0x%llx | envData %u / %u | world pos %.1f %.1f %.1f | window %u of %u bytes | buf %p", i,
                    r.view ? "OURS" : "GAME", static_cast<unsigned long long>(r.hash),
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(r.ret) - reinterpret_cast<uintptr_t>(exe)), e0, e1, w[3], w[7], w[11],
                    r.first * 16, r.width, r.buf), ++logged;
    }
    BVR_LOG("instcap: %d G-buffer draws | envData GAME 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u >6:%u | OURS 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u >6:%u", n,
            hist[0][0], hist[0][1], hist[0][2], hist[0][3], hist[0][4], hist[0][5], hist[0][6], hist[0][7],
            hist[1][0], hist[1][1], hist[1][2], hist[1][3], hist[1][4], hist[1][5], hist[1][6], hist[1][7]);
    ctx->Unmap(st, 0);
    st->Release();
}

void measure(ID3D11DeviceContext* ctx) {
    probe_measure(ctx);
    inst_readback(ctx);
    lightsrc_measure(ctx);
    if (g_abStep >= 0) {                        // HISTAB: apply -> settle -> measure -> log, then the next step
        if (g_abPhase == 0) { vol_lend(kAb[g_abStep][0]); hist_swap(kAb[g_abStep][1]); g_abSettle = 90; g_abPhase = 1; }
        else if (g_abPhase == 1 && --g_abSettle <= 0) { eye_stab(180); g_abPhase = 2; }
        else if (g_abPhase == 2 && g_stabDone) {
            BVR_LOG("histab step %d/%d: vollend %d histswap %2d -> left %.2f (max %.1f) | right %.2f (max %.1f) | left/right %.2f",
                    g_abStep + 1, kAbN, kAb[g_abStep][0], kAb[g_abStep][1], g_stabMean[0], g_stabPeak[0], g_stabMean[1], g_stabPeak[1],
                    g_stabMean[1] > 0 ? g_stabMean[0] / g_stabMean[1] : 0.0);
            g_stabDone = false;
            g_abPhase = 0;
            if (++g_abStep >= kAbN) {
                g_abStep = -1;
                vol_lend(g_abKeepVol); hist_swap(g_abKeepSwap);
                BVR_LOG("histab: done - switches back to vollend %d histswap %d", g_abKeepVol, g_abKeepSwap);
            }
        }
    }
    if (g_stabWant.load() <= 0 || !ctx || !g_pairThisFrame || !g_copy[0] || !g_copy[1]) return;
    for (int e = 0; e < 2; ++e) {
        D3D11_TEXTURE2D_DESC d{}; g_copy[e]->GetDesc(&d);
        for (auto& st : g_stage[e]) {
            if (st) {
                D3D11_TEXTURE2D_DESC c{}; st->GetDesc(&c);
                if (c.Width == d.Width && c.Height == d.Height && c.Format == d.Format) continue;
                st->Release(); st = nullptr;
            }
            D3D11_TEXTURE2D_DESC sd = d;
            sd.BindFlags = 0; sd.MiscFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
            if (dev) { dev->CreateTexture2D(&sd, nullptr, &st); dev->Release(); }
            if (!st) return;
        }
    }
    const int w = g_stageN % 3;
    for (int e = 0; e < 2; ++e) ctx->CopyResource(g_stage[e][w], g_copy[e]);
    g_stageFrame[w] = g_frameNo.load() - 1;
    ++g_stageN;
    if (g_stageN >= 3) {                          // read the copy from two frames ago (no stall)
        const int r = (g_stageN - 3) % 3;
        double frameMean[2] = {-1, -1};
        for (int e = 0; e < 2; ++e) {
            std::vector<uint8_t> cur;
            if (!sample(ctx, g_stage[e][r], cur)) { g_stabPrev[e].clear(); continue; }
            if (g_stabPrev[e].size() == cur.size() && !cur.empty()) {
                double sum = 0;
                for (size_t j = 0; j < cur.size(); ++j) sum += std::abs(int(cur[j]) - int(g_stabPrev[e][j]));
                const double m = sum / cur.size();
                g_stabSum[e] += m; if (m > g_stabMax[e]) g_stabMax[e] = m; ++g_stabCount[e];
            }
            if (!cur.empty()) { double t = 0; for (uint8_t q : cur) t += q; frameMean[e] = t / cur.size(); }
            g_stabPrev[e].swap(cur);
        }
        if (g_stabLog) BVR_LOG("eyestab frame %u: left brightness %.2f, right %.2f", g_stageFrame[r], frameMean[0], frameMean[1]);
    }
    if (g_stabWant.fetch_sub(1) == 1) {
        g_stabLog = false;
        for (int e = 0; e < 2; ++e) { g_stabMean[e] = g_stabCount[e] ? g_stabSum[e] / g_stabCount[e] : 0; g_stabPeak[e] = g_stabMax[e]; }
        g_stabDone = true;
        if (g_abStep < 0)
            BVR_LOG("eyestab: %d / %d frames - left %.2f (max %.1f) | right %.2f (max %.1f) | left/right %.2f (baseline ~1.0, L6 broken 2-4.5)",
                    g_stabCount[0], g_stabCount[1], g_stabMean[0], g_stabPeak[0], g_stabMean[1], g_stabPeak[1],
                    g_stabMean[1] > 0 ? g_stabMean[0] / g_stabMean[1] : 0.0);
    }
}

void writers(int input, int view, int frames) {
    ID3D11Resource* r = input >= 0 && input < kProbeIn && view >= 0 && view < 2 ? g_probeSrc[view][input] : nullptr;
    if (!r) { BVR_LOG("writers: run `ds litprobe` first (input %d of view %d not seen yet)", input, view); return; }
    { std::lock_guard<std::mutex> lk(g_writeMutex); g_writerN = 0; }
    g_writeWatch.store(r);
    g_writeFrames.store(frames);
    BVR_LOG("writers: every shader writing the %s view's %s (%p), %d frames", view ? "our" : "game", kProbeName[input], r, frames);
}

void light_src(int frames) {
    g_lsWant.store(frames + 3);
    eye_stab(frames + 3);
    g_stabLog = true;
    BVR_LOG("lightsrc: every ambient-region pass of both views, with the left eye's brightness per frame, %d frames", frames);
}

void pass_probe(uint64_t hash, int frames, bool dump, int rt) {
    g_ppRt.store(rt);
    g_ppUnknown.store(0);
    g_ppHash.store(hash);
    lit_probe(frames, 0, dump, false);
    BVR_LOG("passprobe: the output of shader %016llx (%s), per view, frame to frame", static_cast<unsigned long long>(hash),
            bvr::ds3::shaderid::name_of(hash) ? bvr::ds3::shaderid::name_of(hash) : "-");
}

void stage_probe(int own, int k, int frames) {
    ID3D11Resource* a = own >= 0 && own < kHistOwn ? g_histOwn[0][own].load() : nullptr;
    ID3D11Resource* b = own >= 0 && own < kHistOwn ? g_histOwn[1][own].load() : nullptr;
    if (!a || !b) { BVR_LOG("stageprobe: own texture %d not known", own); return; }
    g_ppHash.store(0);
    lit_probe(frames, 0, false, false);
    g_probeWant.store(frames);
    g_stageLog = true;
    g_writeWatch.store(a); g_writeWatchB.store(b);
    g_stageK.store(k);
    g_writeQuiet.store(true);
    g_writeFrames.store(frames + 3);
    BVR_LOG("stageprobe: each view's own texture %d right after that view's list number %d that writes it, frame to frame", own, k);
}

void bind_spy(uint64_t hash) { g_inSlot.store(-1); g_inHash.store(hash); g_bindSpy.store(2); BVR_LOG("bindspy: what shader %016llx reads, per view", static_cast<unsigned long long>(hash)); }
void in_probe(uint64_t hash, int slot, int frames) {
    g_ppHash.store(0); g_stageK.store(0);
    lit_probe(frames, 0, false, false);
    g_inSlot.store(slot); g_inHash.store(hash);
    BVR_LOG("inprobe: shader %016llx input t%d, per view, frame to frame", static_cast<unsigned long long>(hash), slot);
}

void inst_spy(int frames) { g_instSpy.store(frames); BVR_LOG("instspy: envData of every instance block (544 bytes) per view, %d frames", frames); }

void inst_cap(ID3D11Device* dev) {
    if (!g_instBuf && dev) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = kInstCap * kInstSize; d.Usage = D3D11_USAGE_DEFAULT;
        dev->CreateBuffer(&d, nullptr, &g_instBuf);
    }
    g_instN.store(0);
    g_instFrame.store(g_frameNo.load() + 2);
    g_instState.store(1);
    BVR_LOG("instcap: capturing every G-buffer draw's instance block in frame %u", g_instFrame.load());
}

void imm_spy(int frames) { g_immLogged = 0; g_immSpy.store(frames); BVR_LOG("immspy: instance-block writes on the immediate context, %d frames", frames); }

} // namespace bvr::ds3::picsplit

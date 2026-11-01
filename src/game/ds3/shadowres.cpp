// DS3VR2 SHADOWRES - see shadowres.h.
#include "shadowres.h"

#include "core/util/log.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <intrin.h>

namespace bvr::ds3::shadowres {
namespace {

float g_k = 1.0f;          // this launch's factor
bool g_ours = false;       // shadow_res_ours: our second view's maps too
std::atomic<int> g_ownBuild{0};
uintptr_t g_exeLo = 0, g_exeHi = 0;

// What the creation hook did (the log may not exist yet at the gate: report() writes it out later).
struct Made { ID3D11Resource* tex; UINT w0, h0, w, h; DXGI_FORMAT fmt; bool own; bool big; };
constexpr int kMade = 64;
Made g_made[kMade];
std::atomic<int> g_madeN{0};
std::atomic<ID3D11Resource*> g_big[kMade];   // the enlarged maps (no reference held: pointer identity only)
std::atomic<uint64_t> g_extraBytes{0};
std::atomic<uint32_t> g_bigN{0}, g_keptN{0}, g_failN{0};
bool g_reported = false;
std::atomic<UINT> g_w0max{0};          // the largest map size the GAME asked for (4096 at MAX)
std::atomic<bool> g_engineFollows{false};   // self-check: the engine sizes its viewports from the real (bigger) map

// Depth view -> 1 on an enlarged map, 2 not. Every creation writes its entry, so an address the runtime reuses for a
// new view is overwritten before it is bound.
constexpr int kDsv = 4096;
std::atomic<void*> g_dsvKey[kDsv];
std::atomic<uint8_t> g_dsvVal[kDsv];

using CreateTex2D = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateDsv = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);
CreateTex2D oCreateTex2D = nullptr;
CreateDsv oCreateDsv = nullptr;

bool pow2(UINT v) { return v && !(v & (v - 1)); }

// A sun shadow map as Dark Souls III makes them: a power-of-two 16/32-bit depth texture that is also read by shaders, one
// level, no array, no initial data. The eye-sized depth buffers (2448x2648 ...) are never powers of two.
bool shadow_like(const D3D11_TEXTURE2D_DESC& d) {
    const bool fmt = d.Format == DXGI_FORMAT_R16_TYPELESS || d.Format == DXGI_FORMAT_R32_TYPELESS ||
                     d.Format == DXGI_FORMAT_D16_UNORM || d.Format == DXGI_FORMAT_D32_FLOAT;
    const UINT need = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    return fmt && (d.BindFlags & need) == need && pow2(d.Width) && pow2(d.Height) && d.Width >= 1024 && d.Height >= 1024 &&
           d.ArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1 && d.Usage == D3D11_USAGE_DEFAULT;
}

bool big(ID3D11Resource* r) {
    if (!r) return false;
    for (auto& b : g_big) if (b.load(std::memory_order_relaxed) == r) return true;
    return false;
}

void forget(ID3D11Resource* r) {   // a new texture at the address of a released enlarged one
    for (auto& b : g_big) { ID3D11Resource* e = r; b.compare_exchange_strong(e, nullptr); }
}

void note(ID3D11Resource* tex, UINT w0, UINT h0, UINT w, UINT h, DXGI_FORMAT f, bool own, bool isBig) {
    const int i = g_madeN.fetch_add(1);
    if (i < kMade) g_made[i] = {tex, w0, h0, w, h, f, own, isBig};
}

HRESULT STDMETHODCALLTYPE hkCreateTex2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init,
                                        ID3D11Texture2D** out) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool game = ret >= g_exeLo && ret < g_exeHi;   // our own copies of a map must keep its (already bigger) size
    if (g_k > 1.0f && d && out && !init && game && shadow_like(*d)) {
        const bool own = g_ownBuild.load() > 0;
        if (own && !g_ours) {
            const HRESULT hr = oCreateTex2D(dev, d, init, out);
            if (SUCCEEDED(hr) && *out) { forget(*out); note(*out, d->Width, d->Height, d->Width, d->Height, d->Format, true, false); g_keptN.fetch_add(1); }
            return hr;
        }
        D3D11_TEXTURE2D_DESC b = *d;
        b.Width = static_cast<UINT>(std::lround(d->Width * g_k));
        b.Height = static_cast<UINT>(std::lround(d->Height * g_k));
        if (b.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) b.Width = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        if (b.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) b.Height = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        // The scale must be the same both ways and exact, or the viewports would not land on the same texels.
        if (static_cast<float>(b.Width) / d->Width == g_k && static_cast<float>(b.Height) / d->Height == g_k) {
            const HRESULT hr = oCreateTex2D(dev, &b, init, out);
            if (SUCCEEDED(hr) && *out) {
                forget(*out);
                for (auto& s : g_big) { ID3D11Resource* e = nullptr; if (s.compare_exchange_strong(e, *out)) break; }
                const UINT bpp = d->Format == DXGI_FORMAT_R16_TYPELESS || d->Format == DXGI_FORMAT_D16_UNORM ? 2 : 4;
                g_extraBytes.fetch_add((static_cast<uint64_t>(b.Width) * b.Height - static_cast<uint64_t>(d->Width) * d->Height) * bpp);
                g_bigN.fetch_add(1);
                if (d->Width > g_w0max.load()) g_w0max.store(d->Width);
                note(*out, d->Width, d->Height, b.Width, b.Height, d->Format, own, true);
                return hr;
            }
            g_failN.fetch_add(1);   // out of memory at the bigger size: fall back to the game's own size
        }
    }
    const HRESULT hr = oCreateTex2D(dev, d, init, out);
    if (SUCCEEDED(hr) && out && *out) forget(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDsv(ID3D11Device* dev, ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* d,
                                      ID3D11DepthStencilView** out) {
    const HRESULT hr = oCreateDsv(dev, r, d, out);
    if (SUCCEEDED(hr) && out && *out) {
        void* v = *out;
        const uint8_t val = big(r) ? 1 : 2;
        const uintptr_t h = (reinterpret_cast<uintptr_t>(v) >> 4) * 2654435761u;
        for (int probe = 0; probe < 64; ++probe) {
            const int i = static_cast<int>((h + probe) & (kDsv - 1));
            void* k = g_dsvKey[i].load(std::memory_order_acquire);
            if (k == v) { g_dsvVal[i].store(val, std::memory_order_release); break; }
            if (!k) {
                void* e = nullptr;
                if (g_dsvKey[i].compare_exchange_strong(e, v) || e == v) { g_dsvVal[i].store(val, std::memory_order_release); break; }
            }
        }
    }
    return hr;
}

bool patch(void** slot, void* det, void** orig) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    *slot = det;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

} // namespace

void install(ID3D11Device* dev, const wchar_t* ini) {
    static std::atomic<bool> done{false};
    if (!dev || done.exchange(true)) return;
    wchar_t b[32] = {};
    GetPrivateProfileStringW(L"ds3vr2", L"shadow_res", L"", b, 32, ini);
    const float k = b[0] ? static_cast<float>(_wtof(b)) : kDefaultFactor;
    // Only factors that give whole texel counts on 1024..4096 maps (and stay within D3D11's 16384).
    g_k = k >= 1.99f ? 2.0f : k >= 1.49f ? 1.5f : 1.0f;
    g_ours = GetPrivateProfileIntW(L"ds3vr2", L"shadow_res_ours", 0, ini) != 0;
    // SR1 (LIVE sim, 2026-10-30): Dark Souls III sizes its shadow viewports AND the filter offset from the real texture itself
    // (it set an 8192-wide viewport and FC_ShadowMapParam.zw = 1/8192 on its own; the 8192 atlas came out filled in all
    // four cascades). So our viewport scaling is only a fallback for an engine that does not: shadow_res_vp = 1.
    if (!GetPrivateProfileIntW(L"ds3vr2", L"shadow_res_vp", 0, ini)) g_engineFollows.store(true);
    if (g_k <= 1.0f) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    g_exeLo = reinterpret_cast<uintptr_t>(exe);
    g_exeHi = g_exeLo + nt->OptionalHeader.SizeOfImage;
    // A vtable slot patch, not MinHook (MinHook is not initialised yet - SHADERID does the same here). The slot is shared
    // by every device of that class; our own creations pass through untouched (not called from the game's code).
    void** vt = *reinterpret_cast<void***>(dev);
    patch(&vt[5], reinterpret_cast<void*>(&hkCreateTex2D), reinterpret_cast<void**>(&oCreateTex2D));
    patch(&vt[10], reinterpret_cast<void*>(&hkCreateDsv), reinterpret_cast<void**>(&oCreateDsv));
}

float factor() { return g_k; }
bool active() { return g_bigN.load(std::memory_order_relaxed) > 0; }
void own_view_build(bool begin) { g_ownBuild.fetch_add(begin ? 1 : -1); }
bool is_big(ID3D11Resource* r) { return g_k > 1.0f && big(r); }

float dsv_scale(ID3D11DepthStencilView* dsv) {
    if (!dsv || g_k <= 1.0f || g_engineFollows.load(std::memory_order_relaxed)) return 1.0f;
    void* v = dsv;
    const uintptr_t h = (reinterpret_cast<uintptr_t>(v) >> 4) * 2654435761u;
    for (int probe = 0; probe < 64; ++probe) {
        const int i = static_cast<int>((h + probe) & (kDsv - 1));
        void* k = g_dsvKey[i].load(std::memory_order_acquire);
        if (k == v) return g_dsvVal[i].load(std::memory_order_acquire) == 1 ? g_k : 1.0f;
        if (!k) break;
    }
    ID3D11Resource* r = nullptr;   // not seen at creation (cannot happen once installed): ask the view itself
    dsv->GetResource(&r);
    const bool b = big(r);
    if (r) r->Release();
    return b ? g_k : 1.0f;
}

void viewport_seen(float right, float bottom) {
    const UINT w0 = g_w0max.load(std::memory_order_relaxed);
    if (!w0 || (right <= w0 + 1.0f && bottom <= w0 + 1.0f) || g_engineFollows.exchange(true)) return;
    BVR_LOG("shadowres: SELF-CHECK - the game set a viewport reaching %.0f x %.0f on a map it asked %u for: it follows the "
            "real size itself, so our viewport scaling is switched OFF (the bigger maps stay)", right, bottom, w0);
}
bool engine_follows() { return g_engineFollows.load(std::memory_order_relaxed); }

float filter_scale(float zw) {
    const UINT w0 = g_w0max.load(std::memory_order_relaxed);
    if (g_k <= 1.0f || !w0 || zw <= 0.0f) return 1.0f;
    if (std::fabs(zw * w0 * g_k - 1.0f) < 0.05f) return 1.0f;   // already one texel of the BIGGER map: the engine follows
    return 1.0f / g_k;                                            // one texel of the game's map (or a kernel in its texels)
}

const char* status() {
    static char s[240];
    if (g_k <= 1.0f) { snprintf(s, sizeof(s), "shadow detail: the game's own size"); return s; }
    UINT w0 = 0, w = 0;
    for (int i = 0; i < g_madeN.load() && i < kMade; ++i) if (g_made[i].big && g_made[i].w0 >= w0) { w0 = g_made[i].w0; w = g_made[i].w; }
    snprintf(s, sizeof(s), "shadow detail x%.1f: %u shadow maps drawn bigger (largest %u -> %u), %u of our second view's kept, "
                           "%.0f MB more video memory%s", g_k, g_bigN.load(), w0, w, g_keptN.load(),
             g_extraBytes.load() / 1048576.0, g_failN.load() ? " - SOME DID NOT FIT, kept at the game's size"
                                              : g_engineFollows.load() ? " (the game follows the size itself)" : "");
    return s;
}

void report(bool force) {
    if (g_reported && !force) return;
    g_reported = true;
    BVR_LOG("shadowres: %s", status());
    for (int i = 0; i < g_madeN.load() && i < kMade; ++i) {
        const Made& m = g_made[i];
        BVR_LOG("shadowres:   %p %ux%u -> %ux%u fmt %d%s", m.tex, m.w0, m.h0, m.w, m.h, static_cast<int>(m.fmt),
                m.own ? (m.big ? " (our second view's)" : " (our second view's - kept at the game's size)") : "");
    }
}

} // namespace bvr::ds3::shadowres

// DS3VR2 CONTACT - see contact.h.
#include "contact.h"

#include "settings.h"
#include "core/util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace bvr::ds3::contact {
namespace {

std::atomic<int> g_on{0};
std::atomic<float> g_len{0.5f}, g_strength{0.85f};
std::atomic<uint32_t> g_draws{0}, g_drawsTotal{0};
std::mutex g_buildMutex;
int g_state = 0;                       // 0 not built, 1 ready, -1 failed (logged once)
ID3D11PixelShader* g_ps = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11BlendState* g_blendShow = nullptr;   // test mode 2: our rays REPLACE the mask (shows them even in shade)
ID3D11Buffer* g_cb = nullptr;
constexpr UINT kCbSlot = 13;           // free in the mask pass (it binds b0 and b8)

// One pixel = one ray towards the sun, marched through this eye's own depth (point samples, 16 steps, jittered start).
const char kPs[] = R"(
cbuffer cbPerScene : register(b0) { float4 g_InvProj[4]; float4 g_Shadow0[4]; };
cbuffer cbSceneParam : register(b8) { float4 g_sp[8]; };
cbuffer cbContact : register(b13) { float4 g_p; float4 g_q; };   // p: length (m), strength, max distance (m), steps
Texture2D<float4> g_Depth : register(t1);
SamplerState SS_ClampPoint : register(s7);

float4x4 inverse4(float4x4 m) {
    float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
    float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
    float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
    float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];
    float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
    float b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    float b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
    float b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    float4x4 r;
    r[0] = float4(a11 * b11 - a12 * b10 + a13 * b09, a02 * b10 - a01 * b11 - a03 * b09, a31 * b05 - a32 * b04 + a33 * b03, a22 * b04 - a21 * b05 - a23 * b03);
    r[1] = float4(a12 * b08 - a10 * b11 - a13 * b07, a00 * b11 - a02 * b08 + a03 * b07, a32 * b02 - a30 * b05 - a33 * b01, a20 * b05 - a22 * b02 + a23 * b01);
    r[2] = float4(a10 * b10 - a11 * b08 + a13 * b06, a01 * b08 - a00 * b10 - a03 * b06, a30 * b04 - a31 * b02 + a33 * b00, a21 * b02 - a20 * b04 - a23 * b00);
    r[3] = float4(a11 * b07 - a10 * b09 - a12 * b06, a00 * b09 - a01 * b07 + a02 * b06, a31 * b01 - a30 * b03 - a32 * b00, a20 * b03 - a21 * b01 + a22 * b00);
    return r / det;
}

float3 viewpos(float4x4 R, float2 clip, float d) {   // the game's own reconstruction (10ada77c): v = InvProj . (clip, d, 1)
    float4 v = mul(R, float4(clip, d, 1.0));
    return v.xyz / v.w;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0, float4 clip4 : TEXCOORD1) : SV_TARGET {
    float4x4 R = float4x4(g_InvProj[0], g_InvProj[1], g_InvProj[2], g_InvProj[3]);
    // screen uv <-> clip: affine, read off this pixel's own derivatives of the game's two interpolants
    float2 a = float2(ddx(uv.x) / ddx(clip4.x), ddy(uv.y) / ddy(clip4.y));
    float2 b = uv - a * clip4.xy;
    float d0 = g_Depth.SampleLevel(SS_ClampPoint, uv, 0).x;
    float3 P = viewpos(R, clip4.xy, d0);
    float dist = length(P);
    if (!(dist < g_p.z) || !(dist > 0.05)) return float4(0, 0, 0, 1);     // sky / beyond range / invalid: leave the mask
    // The sun in view space: the direction along which cascade 0's shadow-map x and y do not change; pointing up.
    float3 L = normalize(cross(g_Shadow0[0].xyz, g_Shadow0[1].xyz));
    float3 upV = float3(g_sp[5].y, g_sp[6].y, g_sp[7].y);
    if (dot(L, upV) < 0) L = -L;
    float4x4 Pm = inverse4(R);
    int steps = (int)g_p.w;
    float len = g_p.x * (1.0 + dist * 0.02);             // a little longer far away, where a pixel covers more
    float stepLen = len / steps;
    float jit = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));
    float thick = max(0.15, len * 0.6);
    float occl = 0;
    [loop] for (int i = 0; i < steps; ++i) {
        float t = (i + jit) * stepLen + 0.02 + dist * 0.0015;   // start just off the surface
        float3 Q = P + L * t;
        float4 c = mul(Pm, float4(Q, 1.0));
        if (c.w <= 0) break;
        float2 cq = c.xy / c.w;
        float2 suv = a * cq + b;
        if (any(suv < 0) || any(suv > 1)) break;
        float ds = g_Depth.SampleLevel(SS_ClampPoint, suv, 0).x;
        float3 S = viewpos(R, cq, ds);
        float dz = abs(Q.z) - abs(S.z);                  // > 0: the picture's surface there is nearer than the ray point
        float bias = 0.01 + abs(Q.z) * 0.002;
        if (dz > bias && dz < thick) { occl = 1.0 - (float)i / steps * 0.5; break; }
    }
    float fade = saturate((g_p.z - dist) / (g_p.z * 0.25));
    return float4(0, 0, 0, 1.0 - occl * g_p.y * fade);
}
)";

bool build(ID3D11Device* dev) {
    std::lock_guard<std::mutex> lk(g_buildMutex);
    if (g_state) return g_state == 1;
    g_state = -1;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) { BVR_LOG("contact: no shader compiler"); return false; }
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(compile(kPs, sizeof(kPs) - 1, "contact_ps", nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)) || !code) {
        BVR_LOG("contact: shader did not compile: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    const bool okPs = SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps));
    code->Release();
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_ZERO; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_MIN;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;   // the packed depth in RGB stays the game's
    bool okB = SUCCEEDED(dev->CreateBlendState(&bd, &g_blend));
    rt.BlendEnable = FALSE;
    okB = okB && SUCCEEDED(dev->CreateBlendState(&bd, &g_blendShow));
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 32; cd.Usage = D3D11_USAGE_DYNAMIC; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const bool okC = SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_cb));
    if (!okPs || !okB || !okC) { BVR_LOG("contact: could not create the pass (ps %d blend %d cb %d)", okPs, okB, okC); return false; }
    g_state = 1;
    BVR_LOG("contact: ray-traced contact shadow pass built");
    return true;
}

} // namespace

void after_mask(ID3D11DeviceContext* c, bool indexed, unsigned count, unsigned start, int base, DrawFn draw, DrawIdxFn drawIdx) {
    if (!g_on.load(std::memory_order_relaxed) || !c) return;
    if (g_state != 1) {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        const bool ok = dev && build(dev);
        if (dev) dev->Release();
        if (!ok) return;
    }
    // The constants: written with Map(WRITE_DISCARD) on this deferred context, so each list carries its own copy.
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(c->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    float* p = static_cast<float*>(ms.pData);
    p[0] = g_len.load(std::memory_order_relaxed); p[1] = g_strength.load(std::memory_order_relaxed); p[2] = 40.0f; p[3] = 16.0f;
    p[4] = p[5] = p[6] = p[7] = 0.0f;
    c->Unmap(g_cb, 0);
    // Keep the game's state exactly: its pixel shader, blend state and whatever sits at b13.
    ID3D11PixelShader* ps = nullptr; ID3D11ClassInstance* ci[16] = {}; UINT nci = 16;
    c->PSGetShader(&ps, ci, &nci);
    ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT mask = 0;
    c->OMGetBlendState(&bs, bf, &mask);
    ID3D11Buffer* oldCb = nullptr;
    c->PSGetConstantBuffers(kCbSlot, 1, &oldCb);
    c->PSSetShader(g_ps, nullptr, 0);
    c->PSSetConstantBuffers(kCbSlot, 1, &g_cb);
    c->OMSetBlendState(g_on.load(std::memory_order_relaxed) == 2 ? g_blendShow : g_blend, nullptr, 0xFFFFFFFFu);
    if (indexed) drawIdx(c, count, start, base); else draw(c, count, start);
    c->OMSetBlendState(bs, bf, mask);
    c->PSSetConstantBuffers(kCbSlot, 1, &oldCb);
    c->PSSetShader(ps, nci ? ci : nullptr, nci);
    if (ps) ps->Release();
    for (UINT i = 0; i < nci; ++i) if (ci[i]) ci[i]->Release();
    if (bs) bs->Release();
    if (oldCb) oldCb->Release();
    g_draws.fetch_add(1, std::memory_order_relaxed);
    g_drawsTotal.fetch_add(1, std::memory_order_relaxed);
}

void load() {
    // DS3: off by default - it hooks Sekiro's screen-shadow pass, which DS3 never ran ("0 passes drawn").
    set(settings::i("contact_shadows", 0), settings::f("contact_len", 0.5f), settings::f("contact_strength", 0.85f));
}
void set(int on, float len, float strength) {
    g_on.store(on == 2 ? 2 : on ? 1 : 0);   // 2 = test: show only the traced shadows
    g_len.store(len < 0.05f ? 0.05f : len > 3.0f ? 3.0f : len);
    g_strength.store(strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength);
    BVR_LOG("contact: ray-traced contact shadows %s (ray %.2f m, strength %.2f)", g_on.load() == 2 ? "SHOWN ALONE (test)" : g_on.load() ? "ON" : "off", g_len.load(), g_strength.load());
}
int on() { return g_on.load(); }
float length() { return g_len.load(); }
float strength() { return g_strength.load(); }
const char* status() {
    static char s[160];
    snprintf(s, sizeof(s), "contact shadows %s: %u passes drawn since the last look (%u in all)%s", g_on.load() ? "on" : "off",
             g_draws.exchange(0), g_drawsTotal.load(), g_state < 0 ? " - PASS FAILED TO BUILD (see log)" : "");
    return s;
}

} // namespace bvr::ds3::contact

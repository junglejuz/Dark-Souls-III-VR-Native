// DS3VR2 DLSS - see dlss.h. Port of akvr_native/src/dlss.cpp (AKN D1, JJ-confirmed); method after Luma's Arkham Knight
// mod (Luma Framework, Filippo Tarpini / Garamond, "Custom MIT": reuse with credit). Changes from AKN: settings from
// DS3VR2\settings.ini, output size set by the adapter (runtime eye size x resolution slider), NGX DLL from <game>\DS3VR2,
// no rain mask, camera-only motion (mode 1) needs no game velocity buffer.
#include "dlss.h"
#include "dlss_modes.h"
#include "eyesize.h"

#include "settings.h"

#include "core/util/log.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

namespace bvr::ds3::dlss {
namespace {

int   g_mode = -1;
UINT  g_outW = 0, g_outH = 0;
float g_scale = 1.0f;
wchar_t g_dir[MAX_PATH] = L"";
bool  g_ngxTried = false, g_ngxOk = false;
// NGX's own start took 2579 ms on the present thread in run M6-1, with a session already live - exactly the stall
// VR_ESSENTIALS section 4 forbids (Asylum PIMAXSTART). It now runs on a background thread at the first Present and
// the eyes simply stay un-upscaled until it is ready. 0 not started, 1 starting, 2 finished.
std::atomic<int> g_ngxState{0};
NVSDK_NGX_Parameter* g_caps = nullptr;
ID3D11Device* g_dev = nullptr;
int g_rebirth = 0;   // V13s REBIRTH switch (`ds dlss rebirth 1`)
struct Eye {
    NVSDK_NGX_Handle* h = nullptr; NVSDK_NGX_Parameter* p = nullptr; bool failed = false;
    UINT rw = 0, rh = 0;
    ID3D11Texture2D* in = nullptr; ID3D11Texture2D* out = nullptr;
    ID3D11Resource* mv = nullptr; ID3D11Resource* depth = nullptr;   // weak: noted this eye's render
    bool reset = true; long evals = 0, skips = 0;
    // REBIRTH (ported from SKVR2 JITTER5, 2026-10-31; DS3 JJ 23:37 "3D was completely missing from the left eye", log: the
    // left feature created 5 ms BEFORE the camera motion pass, the right one after). A first feature can come out blank
    // while NGX reports success; rebuilding it cures it. Each eye rebuilds once, ~60 evaluations after it starts.
    bool reborn = false; long sinceBorn = -1;
    UINT newW = 0, newH = 0; int newN = 0;
    double prevVP[16] = {}; bool prevOk = false;
    ID3D11Texture2D* mvOut = nullptr; ID3D11UnorderedAccessView* mvUav = nullptr;
} g_eye[2];
volatile LONG g_preset = 0, g_depthInv = 1, g_mvMode = 1;
// A/B for the declared jitter (playbook 19 fault 1: "misreported jitter is worse than none"). +1 is the sign the
// viewport shift means; `ds dlss jitsign -1` declares the opposite, so the right one can be PROVEN, not argued.
volatile LONG g_jitSign = 1;
volatile LONG g_mvGuard = 1;   // MVGUARD: `ds dlss mvguard 0|1` (settings dlss_mvguard, default 1 - removed the Inf motion live)
volatile LONG g_check = 1;     // DLSSCHECK: `ds dlss check 0|1` (on by default - it costs a few tiny dispatches a second)
const char* g_why[2] = {"not tried yet", "not tried yet"};   // why an eye was not upscaled (diag / log)
ID3D11ComputeShader* g_mvCS = nullptr; ID3D11Buffer* g_mvCB = nullptr; int g_mvState = 0;
std::map<ID3D11Resource*, ID3D11ShaderResourceView*> g_srvCache;
ID3DDeviceContextState* g_state = nullptr;

struct Tq { ID3D11Query* dj = nullptr; ID3D11Query* a = nullptr; ID3D11Query* b = nullptr; bool pending = false; };
Tq g_tq[2][6]; int g_tqI[2] = {}; double g_tqSum = 0; long g_tqN = 0; double g_lastMs = 0;
void tq_collect(ID3D11DeviceContext* c) {
    for (auto& row : g_tq) for (auto& q : row) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
        if (c->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (c->GetData(q.a, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            c->GetData(q.b, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b > a) { g_tqSum += static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency); ++g_tqN; }
    }
}
Tq* tq_begin(ID3D11DeviceContext* c, int eye) {
    tq_collect(c);
    Tq& q = g_tq[eye][g_tqI[eye]++ % 6];
    if (q.pending) return nullptr;
    if (!q.dj) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}; g_dev->CreateQuery(&d, &q.dj);
        d.Query = D3D11_QUERY_TIMESTAMP; g_dev->CreateQuery(&d, &q.a); g_dev->CreateQuery(&d, &q.b);
    }
    if (!q.dj || !q.a || !q.b) return nullptr;
    c->Begin(q.dj); c->End(q.a);
    return &q;
}
void tq_end(ID3D11DeviceContext* c, Tq* q) { if (q) { c->End(q->b); c->End(q->dj); q->pending = true; } }

float halton(int i, int b) { float f = 1, r = 0; while (i > 0) { f /= static_cast<float>(b); r += f * static_cast<float>(i % b); i /= b; } return r; }

void load_settings() {
    if (g_mode >= 0) return;
    // The conventional modes and their scales come from dlss_modes.h, the same table eyesize sized the render with
    // (eyesize also converts an old `dlss` key into `dlss_mode` in DllMain, before this ever runs).
    g_mode = settings::i("dlss_mode", 0);
    if (g_mode < 0 || g_mode >= dlss_modes::kModeCount) g_mode = 0;
    g_scale = dlss_modes::scale_of(g_mode);
    g_preset = settings::i("dlss_preset", 0);
    g_mvGuard = settings::i("dlss_mvguard", 1);
    // DEPTHDIR (2026-11-01 overnight): DS3's depth is STANDARD (near 0, far 1), not reversed like Sekiro's - `ds vpcheck`: a
    // point 10 units ahead lands at depth 0.992 (= 1 - near/10), and the depth means fall when walking up to a wall. DLSS was
    // told "inverted" (flag inherited from the Sekiro fork). `ds dlss depthinv 1` brings the old flag back (features rebuilt).
    g_depthInv = settings::i("dlss_depthinv", 0);
    swprintf_s(g_dir, L"%s", bvr::log::data_dir());
    BVR_LOG("DLSS: %s (scale %.3f), preset %s; NGX DLL folder %ls", dlss_modes::kModes[g_mode].name, g_scale,
            dlss_modes::kPresets[dlss_modes::preset_index(static_cast<int>(g_preset))].name, g_dir);
}

bool ngx_init(ID3D11Device* dev) {
    if (g_ngxTried) return g_ngxOk;
    g_ngxTried = true; g_dev = dev;
    const wchar_t* paths[1] = {g_dir};
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path = paths; info.PathListInfo.Length = 1;
    // our own project GUID (unregistered with NVIDIA, like Luma's and AKN's)
    const ULONGLONG t0 = GetTickCount64();
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("8d1f3a62-5c47-4b9e-a0d3-6e2b9c71f4a5", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                             "2.0", g_dir, dev, &info);
    if (NVSDK_NGX_FAILED(r)) { BVR_LOG("DLSS: NGX init FAILED 0x%08x (nvngx_dlss.dll in %ls?)", (unsigned)r, g_dir); return false; }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_caps);
    int avail = 0; if (g_caps) g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    g_ngxOk = NVSDK_NGX_SUCCEED(r) && avail;
    BVR_LOG("DLSS: NGX %s (super resolution available %d) - start took %llu ms", g_ngxOk ? "ready" : "NOT usable", avail,
            GetTickCount64() - t0);
    ID3D11Device1* d1 = nullptr;
    if (g_ngxOk && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1))) && d1) {
        // a private context state: DLSS runs inside it, the game's state is swapped back untouched
        const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state);
        d1->Release();
    }
    return g_ngxOk;
}

void mul4(const double a[16], const double b[16], double o[16]) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { double v = 0; for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = v; }
}
bool inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return false;
        if (p != c) for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        const double d = a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) o[i * 4 + j] = a[i][j + 4];
    return true;
}
ID3D11ShaderResourceView* srv_for(ID3D11Resource* r) {
    if (!r) return nullptr;
    auto it = g_srvCache.find(r);
    if (it != g_srvCache.end()) return it->second;
    ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) || !t) return nullptr;
    t->GetDesc(&d); t->Release();
    DXGI_FORMAT f = d.Format;
    if (f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    else if (f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D32_FLOAT) f = DXGI_FORMAT_R32_FLOAT;
    else if (f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT) f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    else if (f == DXGI_FORMAT_R16G16_TYPELESS) f = DXGI_FORMAT_R16G16_FLOAT;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = f; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* v = nullptr;
    if (FAILED(g_dev->CreateShaderResourceView(r, &sd, &v))) { BVR_LOG("DLSS: camera motion - no view on a texture of format %d", (int)d.Format); v = nullptr; }
    g_srvCache[r] = v;
    return v;
}
bool mv_build() {
    if (g_mvState) return g_mvState == 1;
    g_mvState = -1;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) { BVR_LOG("DLSS: camera motion - no shader compiler"); return false; }
    // CAMMV (AKN 2026-10-08): every pixel's point from this eye's depth + camera, projected with the same eye's camera of
    // the previous frame = the camera motion, in render pixels, without jitter. Mode 2 keeps the game's own value where it
    // has a moving object (needs the game's velocity buffer, t1).
    static const char cs[] =
        "Texture2D<float> depthT : register(t0); Texture2D<float2> gameMv : register(t1); RWTexture2D<float2> outMv : register(u0);"
        "cbuffer C : register(b0) { float4 r0, r1, r2, r3; float4 sz; float4 jit; float4 opt; };"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
        " if (id.x >= (uint)sz.x || id.y >= (uint)sz.y) return;"
        " float d = depthT.Load(int3(id.xy, 0));"
        " float2 ndc = float2((id.x + 0.5) * sz.z * 2.0 - 1.0, 1.0 - (id.y + 0.5) * sz.w * 2.0);"
        " float4 p = ndc.x * r0 + ndc.y * r1 + d * r2 + r3;"
        " float2 prev = p.xy / p.w;"
        " float2 mvN = prev - (ndc - jit.xy);"
        " float2 mv = float2(mvN.x * sz.x * 0.5, -mvN.y * sz.y * 0.5);"
        " if (jit.z > 1.5) { float2 g = gameMv.Load(int3(id.xy, 0)); if (abs(g.x) + abs(g.y) > 1e-6) mv = float2(g.x * sz.x * 0.5, g.y * sz.y * 0.5 * jit.w); }"
        // MVGUARD (2026-11-01): a motion that is not a finite number, or longer than the picture, becomes "no motion".
        // Bit tests, so the compiler cannot fold the NaN check away. 16-bit floats overflow to Inf above 65504.
        " if (opt.x > 0.5) { uint2 b = asuint(mv) & 0x7f800000; float lim = max(sz.x, sz.y);"
        "   if (b.x == 0x7f800000 || b.y == 0x7f800000 || abs(mv.x) > lim || abs(mv.y) > lim) mv = float2(0, 0); }"
        " outMv[id.xy] = mv; }";
    ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(compile(cs, sizeof(cs) - 1, "cammv", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) || !b) {
        BVR_LOG("DLSS: camera motion shader failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    const bool ok = SUCCEEDED(g_dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_mvCS));
    b->Release();
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 112; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!ok || FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_mvCB))) return false;
    g_mvState = 1;
    BVR_LOG("DLSS: camera motion pass ready");
    return true;
}
DXGI_FORMAT concrete(DXGI_FORMAT f) {
    switch (f) {   // the game's final picture is a TYPELESS target; NGX needs a view format on its own textures
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        default: return f;
    }
}
bool make_tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** out) {
    f = concrete(f);
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    return SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, out)) && *out;
}
bool make_feature(ID3D11DeviceContext* c, Eye& e, UINT rw, UINT rh) {
    if (e.h) { NVSDK_NGX_D3D11_ReleaseFeature(e.h); e.h = nullptr; }
    if (!e.p && NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&e.p))) return false;
    const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                      (g_depthInv ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
    if (g_preset > 0) {
        const char* const keys[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance};
        for (const char* k : keys) NVSDK_NGX_Parameter_SetUI(e.p, k, static_cast<unsigned>(g_preset));
    }
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = rw; cp.Feature.InHeight = rh; cp.Feature.InTargetWidth = g_outW; cp.Feature.InTargetHeight = g_outH;
    // dlss_modes.h order: 1 DLAA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance.
    static const NVSDK_NGX_PerfQuality_Value kNgx[dlss_modes::kModeCount] = {
        NVSDK_NGX_PerfQuality_Value_MaxQuality,          // (off - never created)
        NVSDK_NGX_PerfQuality_Value_DLAA,
        NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    cp.Feature.InPerfQualityValue = kNgx[g_mode >= 0 && g_mode < dlss_modes::kModeCount ? g_mode : 0];
    cp.InFeatureCreateFlags = flags;
    const ULONGLONG t0 = GetTickCount64();
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(c, &e.h, e.p, &cp);
    if (NVSDK_NGX_FAILED(r)) { cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced; r = NGX_D3D11_CREATE_DLSS_EXT(c, &e.h, e.p, &cp); }
    BVR_LOG("DLSS: eye feature %ux%u -> %ux%u, flags 0x%x, preset %ld: %s (0x%08x), took %llu ms", rw, rh, g_outW, g_outH,
            flags, g_preset, NVSDK_NGX_SUCCEED(r) ? "created" : "FAILED", (unsigned)r, GetTickCount64() - t0);
    e.rw = rw; e.rh = rh; e.reset = true;
    return NVSDK_NGX_SUCCEED(r) && e.h;
}

// ---- DLSSCHECK (2026-11-01 overnight): the mod says itself which eye's DLSS is broken, and why ----
// JJ (DS3 left eye, Sekiro right eye): one eye black or coloured garbage, intermittent; blackwatch saw the finished left
// image at brightness 0 for 2-22 frames at a time from 0.3 s after DLSS started. Every 10th evaluation of an eye, a tiny
// pass samples every 8th pixel of each of that eye's DLSS inputs and its output - colour in, our motion vectors, depth,
// colour out - into a 4 x 8 uint buffer (non-finite count, Inf count, four channel sums, largest |value|, samples),
// copied to a staging ring and read back a few frames later without waiting. An eye is BROKEN when its output's colour
// differs from its input's by more than a quarter (the upscale must keep the picture), or any input is not finite.
// Logged every 2 s per eye; the first broken sample (then at most every 20 s, 3 times) asks for a capture of both eyes.
namespace chk {
enum { kIn = 0, kMv = 1, kDepth = 2, kOut = 3, kKinds = 4, kStage = 4 };
ID3D11ComputeShader* g_cs = nullptr; ID3D11Buffer* g_cb = nullptr; int g_state = 0;
struct Ring { ID3D11Buffer* st = nullptr; bool pending = false; long eval = 0; float lx = 0, ly = 0; };
struct PerEye {
    ID3D11Buffer* buf = nullptr; ID3D11UnorderedAccessView* uav = nullptr; Ring ring[kStage]; int next = 0;
    // the 2 s window
    long samples = 0, broken = 0, nonfinite[kKinds] = {}; double sumIn = 0, sumOut = 0, sumAlphaOut = 0, mvMax = 0, mvMean = 0;
    double dMin = 1e9, dMax = -1e9; float lxMin = 1e9f, lxMax = 0, lyMin = 1e9f, lyMax = 0; long lxJumps = 0; float lxLast = 0;
    char worst[220] = ""; double worstScore = -1;
    long totalBroken = 0;
} g_e[2];
ULONGLONG g_lastLog = 0, g_lastCap = 0; int g_caps = 0; volatile LONG g_capReq = 0;   // bit 0/1 = eye that broke, bit 2 = asked
bool build() {
    if (g_state) return g_state == 1;
    g_state = -1;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) return false;
    static const char cs[] =
        "Texture2D<float4> T : register(t0); RWByteAddressBuffer O : register(u0);"
        "cbuffer C : register(b0) { uint slot, w, h, chans; float scale, clampv, p0, p1; };"
        "uint q(float v) { return (uint)(min(abs(v), clampv) * scale + 0.5); }"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
        " uint2 p = id.xy * 8 + 4; if (p.x >= w || p.y >= h) return;"
        " float4 v = T.Load(int3(p, 0)); uint4 b = asuint(v); uint base = slot * 32;"
        " uint bad = 0, inf = 0; float mx = 0; float vv[4] = {v.x, v.y, v.z, v.w}; uint bb[4] = {b.x, b.y, b.z, b.w};"
        " [unroll] for (uint c = 0; c < 4; ++c) { if (c < chans) {"
        "  if ((bb[c] & 0x7f800000) == 0x7f800000) { bad++; if ((bb[c] & 0x007fffff) == 0) inf++; vv[c] = 0; }"
        "  mx = max(mx, abs(vv[c])); } else vv[c] = 0; }"
        " if (bad) O.InterlockedAdd(base + 0, 1); if (inf) O.InterlockedAdd(base + 4, 1);"
        " O.InterlockedAdd(base + 8, q(vv[0])); O.InterlockedAdd(base + 12, q(vv[1]));"
        " O.InterlockedAdd(base + 16, q(vv[2])); O.InterlockedAdd(base + 20, q(vv[3]));"
        " O.InterlockedMax(base + 24, asuint(mx)); O.InterlockedAdd(base + 28, 1); }";
    ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(compile(cs, sizeof(cs) - 1, "dlsscheck", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) || !b) {
        BVR_LOG("dlsscheck: shader failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    const bool ok = SUCCEEDED(g_dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_cs));
    b->Release();
    D3D11_BUFFER_DESC cd{}; cd.ByteWidth = 32; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!ok || FAILED(g_dev->CreateBuffer(&cd, nullptr, &g_cb))) return false;
    for (auto& pe : g_e) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = kKinds * 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &pe.buf))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kKinds * 8; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(g_dev->CreateUnorderedAccessView(pe.buf, &ud, &pe.uav))) return false;
        for (auto& r : pe.ring) {
            D3D11_BUFFER_DESC sd{}; sd.ByteWidth = kKinds * 32; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(g_dev->CreateBuffer(&sd, nullptr, &r.st))) return false;
        }
    }
    g_state = 1;
    BVR_LOG("dlsscheck: per-eye DLSS input / output check ready (every 10th evaluation, logged every 2 s)");
    return true;
}
// one texture -> slot `kind`: channels counted, scale for the sums (colour 255 = 0..255, motion 16 = 1/16 pixel, depth 1e5)
void measure(ID3D11DeviceContext* c, PerEye& pe, int kind, ID3D11Resource* r, UINT chans, float scale, float clampv) {
    ID3D11ShaderResourceView* v = srv_for(r);
    if (!v) return;
    ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) || !t) return;
    t->GetDesc(&d); t->Release();
    struct { UINT slot, w, h, chans; float scale, clampv, p0, p1; } cb{static_cast<UINT>(kind), d.Width, d.Height, chans, scale, clampv, 0, 0};
    c->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    c->CSSetShader(g_cs, nullptr, 0); c->CSSetShaderResources(0, 1, &v); c->CSSetUnorderedAccessViews(0, 1, &pe.uav, nullptr);
    c->CSSetConstantBuffers(0, 1, &g_cb);
    c->Dispatch((d.Width / 8 + 7) / 8, (d.Height / 8 + 7) / 8, 1);
    ID3D11ShaderResourceView* nv = nullptr; c->CSSetShaderResources(0, 1, &nv);
}
const float kScale[kKinds] = {255.0f, 16.0f, 100000.0f, 255.0f}, kClamp[kKinds] = {1.0f, 20000.0f, 1.0f, 1.0f};
const UINT kChans[kKinds] = {4, 2, 1, 4};
void read_back(ID3D11DeviceContext* c, int eye) {
    PerEye& pe = g_e[eye];
    for (auto& r : pe.ring) {
        if (!r.pending) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (c->Map(r.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        uint32_t u[kKinds][8]; memcpy(u, m.pData, sizeof(u)); c->Unmap(r.st, 0);
        r.pending = false;
        double mean[kKinds][4] = {}; float mx[kKinds] = {};
        for (int k = 0; k < kKinds; ++k) {
            const double n = u[k][7] ? static_cast<double>(u[k][7]) : 1.0;
            for (int ch = 0; ch < 4; ++ch) mean[k][ch] = u[k][2 + ch] / n / kScale[k];
            memcpy(&mx[k], &u[k][6], 4);
        }
        // colour means on 0..255
        const double inS = (mean[kIn][0] + mean[kIn][1] + mean[kIn][2]) * 255.0 / 3.0;
        const double outS = (mean[kOut][0] + mean[kOut][1] + mean[kOut][2]) * 255.0 / 3.0;
        double diff = 0; for (int ch = 0; ch < 3; ++ch) diff += fabs(mean[kOut][ch] - mean[kIn][ch]) * 255.0 / 3.0;
        const bool nonfin = u[kMv][0] || u[kDepth][0] || u[kIn][0] || u[kOut][0];
        const bool hasOut = u[kOut][7] != 0, hasIn = u[kIn][7] != 0;
        const bool broken = hasOut && hasIn && (nonfin || diff > 0.25 * inS + 6.0);
        ++pe.samples;
        for (int k = 0; k < kKinds; ++k) pe.nonfinite[k] += u[k][0];
        pe.sumIn += inS; pe.sumOut += outS; pe.sumAlphaOut += mean[kOut][3] * 255.0;
        const double mvMean = mean[kMv][0] + mean[kMv][1];
        pe.mvMean += mvMean; if (mx[kMv] > pe.mvMax) pe.mvMax = mx[kMv];
        if (u[kDepth][7]) { const double dm = mean[kDepth][0]; if (dm < pe.dMin) pe.dMin = dm; if (dm > pe.dMax) pe.dMax = dm; }
        if (r.lx > 0) {
            if (r.lx < pe.lxMin) pe.lxMin = r.lx; if (r.lx > pe.lxMax) pe.lxMax = r.lx;
            if (r.ly < pe.lyMin) pe.lyMin = r.ly; if (r.ly > pe.lyMax) pe.lyMax = r.ly;
        }
        const double score = diff / (inS + 1.0) + (nonfin ? 10.0 : 0.0);
        if (score > pe.worstScore) {
            pe.worstScore = score;
            snprintf(pe.worst, sizeof(pe.worst), "eval %ld: in %.1f out %.1f (alpha %.0f) | motion mean %.2f max %.1f px, "
                     "not finite %u (Inf %u) | depth mean %.5f max %.4f, not finite %u | lens x %.4f y %.4f", r.eval, inS, outS,
                     mean[kOut][3] * 255.0, mvMean, mx[kMv], u[kMv][0], u[kMv][1], mean[kDepth][0], mx[kDepth], u[kDepth][0], r.lx, r.ly);
        }
        if (broken) {
            ++pe.broken; ++pe.totalBroken;
            const ULONGLONG now = GetTickCount64();
            if (g_caps < 3 && (!g_lastCap || now - g_lastCap > 20000)) {
                g_lastCap = now; ++g_caps;
                InterlockedOr(&g_capReq, (1 << eye) | 4);
                BVR_LOG("dlsscheck: eye %s BROKEN (%s) - capturing both eyes' DLSS inputs and outputs", eye ? "R" : "L", pe.worst);
            }
        }
    }
}
void log_window() {
    const ULONGLONG now = GetTickCount64();
    if (g_lastLog && now - g_lastLog < 2000) return;
    const bool any = g_e[0].samples || g_e[1].samples;
    g_lastLog = now;
    if (!any) return;
    char s[2][420];
    for (int i = 0; i < 2; ++i) {
        PerEye& pe = g_e[i];
        const double n = pe.samples ? static_cast<double>(pe.samples) : 1.0;
        snprintf(s[i], sizeof(s[i]), "%s %s %ld/%ld broken | in %.1f out %.1f alpha %.0f | motion mean %.2f max %.1f | depth %.5f..%.5f | "
                 "not finite in %ld mv %ld depth %ld out %ld | lens x %.4f..%.4f y %.4f..%.4f jumps %ld | worst: %s",
                 i ? "R" : "L", pe.broken ? "BROKEN" : "ok", pe.broken, pe.samples, pe.sumIn / n, pe.sumOut / n, pe.sumAlphaOut / n,
                 pe.mvMean / n, pe.mvMax, pe.dMin < 1e8 ? pe.dMin : 0.0, pe.dMax > -1e8 ? pe.dMax : 0.0, pe.nonfinite[kIn],
                 pe.nonfinite[kMv], pe.nonfinite[kDepth], pe.nonfinite[kOut], pe.lxMin < 1e8f ? pe.lxMin : 0.0f, pe.lxMax,
                 pe.lyMin < 1e8f ? pe.lyMin : 0.0f, pe.lyMax, pe.lxJumps, pe.worstScore >= 0 ? pe.worst : "-");
        const long tb = pe.totalBroken; const float lj = pe.lxLast;
        // keep the GPU objects and the ring, reset the window
        pe.samples = pe.broken = 0; for (auto& v : pe.nonfinite) v = 0;
        pe.sumIn = pe.sumOut = pe.sumAlphaOut = pe.mvMax = pe.mvMean = 0; pe.dMin = 1e9; pe.dMax = -1e9;
        pe.lxMin = pe.lyMin = 1e9f; pe.lxMax = pe.lyMax = 0; pe.lxJumps = 0; pe.worst[0] = 0; pe.worstScore = -1;
        pe.totalBroken = tb; pe.lxLast = lj;
    }
    BVR_LOG("dlsscheck: %s", s[0]);
    BVR_LOG("dlsscheck: %s", s[1]);
}
// called inside the private context state, after the eye's evaluation
void sample(ID3D11DeviceContext* c, int eye, Eye& e, ID3D11Resource* mv, const float* VP) {
    if (!g_check || eye < 0 || eye > 1) return;
    PerEye& pe = g_e[eye];
    // the lens size of the matrix the motion came from (row-vector form: column 0 / 1 = the clip x / y rows); a camera
    // flip shows as a jump between evaluations, on every evaluation, not only the sampled ones
    float lx = 0, ly = 0;
    if (VP) {
        lx = sqrtf(VP[0] * VP[0] + VP[4] * VP[4] + VP[8] * VP[8]);
        ly = sqrtf(VP[1] * VP[1] + VP[5] * VP[5] + VP[9] * VP[9]);
        if (pe.lxLast > 0 && fabsf(lx - pe.lxLast) > 0.01f * pe.lxLast) ++pe.lxJumps;
        pe.lxLast = lx;
    }
    if ((e.evals % 10) != 0 || !build()) return;
    Ring* slot = nullptr;
    for (int k = 0; k < kStage && !slot; ++k) { Ring& r = pe.ring[(pe.next + k) % kStage]; if (!r.pending) { slot = &r; pe.next = (pe.next + k + 1) % kStage; } }
    if (!slot) return;
    const UINT zero[4] = {};
    c->ClearUnorderedAccessViewUint(pe.uav, zero);
    measure(c, pe, kIn, e.in, kChans[kIn], kScale[kIn], kClamp[kIn]);
    if (mv) measure(c, pe, kMv, mv, kChans[kMv], kScale[kMv], kClamp[kMv]);
    if (e.depth) measure(c, pe, kDepth, e.depth, kChans[kDepth], kScale[kDepth], kClamp[kDepth]);
    measure(c, pe, kOut, e.out, kChans[kOut], kScale[kOut], kClamp[kOut]);
    ID3D11UnorderedAccessView* nu = nullptr; c->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    c->CopyResource(slot->st, pe.buf);
    slot->pending = true; slot->eval = e.evals; slot->lx = lx; slot->ly = ly;
}
} // namespace chk

} // namespace

// Starts NGX on a background thread (the eyes stay un-upscaled until it is ready). Safe off-thread: a D3D11 device is
// free-threaded by default and only the FEATURE creation needs a context, which stays on the present thread.
void start(ID3D11Device* dev) {
    load_settings();
    if (g_mode <= 0 || !dev) return;
    int expect = 0;
    if (!g_ngxState.compare_exchange_strong(expect, 1)) return;
    dev->AddRef();
    std::thread([dev] {
        const ULONGLONG t0 = GetTickCount64();
        ngx_init(dev);
        BVR_LOG("DLSS: background start finished after %llu ms (usable %d)", GetTickCount64() - t0, (int)g_ngxOk);
        g_ngxState.store(2, std::memory_order_release);
        dev->Release();
    }).detach();
    BVR_LOG("DLSS: starting NGX in the background (the eyes stay as they are until it is ready)");
}

int   mode() { load_settings(); return g_mode; }
float scale() { load_settings(); return g_scale; }
void  out_size(UINT& w, UINT& h) { w = g_outW; h = g_outH; }
void  set_out_size(UINT w, UINT h) {
    w &= ~7u; h &= ~7u; // compare the rounded size (2284 -> 2280 used to count as a change every frame)
    if (w == g_outW && h == g_outH) return;
    g_outW = w; g_outH = h;
    for (auto& e : g_eye) { e.rw = 0; if (e.out) { e.out->Release(); e.out = nullptr; } }
    BVR_LOG("DLSS: headset eye %ux%u", g_outW, g_outH);
}
void jitter(unsigned long long frame, float& jpx, float& jpy) {
    // Halton(2,3) sub-pixel offset in render pixels, -0.5..0.5; phase count 8 x (out/in)^2 (NVIDIA's guide). The same
    // index for both eyes (playbook 19 #dlss-in-vr).
    load_settings();
    if (g_mode <= 0) { jpx = jpy = 0; return; }
    const float ratio = g_scale > 0.01f ? 1.0f / g_scale : 1.0f;
    const int phases = static_cast<int>(lrintf(8.0f * ratio * ratio));
    const int i = static_cast<int>(frame % static_cast<unsigned long long>(phases > 0 ? phases : 8)) + 1;
    jpx = halton(i, 2) - 0.5f; jpy = halton(i, 3) - 0.5f;
}
void note_mv(int eye, ID3D11Resource* r) { if (eye == 0 || eye == 1) g_eye[eye].mv = r; }
void note_depth(int eye, ID3D11Resource* r) { if (eye == 0 || eye == 1) g_eye[eye].depth = r; }
void command(const char* what, int v) {
    if (!strcmp(what, "mode")) {
        load_settings();
        // Only 0 (stop evaluating) and the mode this launch was SIZED for are allowed. Switching it on live in a
        // launch that renders full size hung the game (run M6 02:41:25: the first full-size feature was built on the
        // present thread with a session live). Mode 0 <-> the launch mode is the A/B every sharpness test needs.
        // The mode this launch was SIZED for, from eyesize - not settings.ini, which the panel may have changed since
        // (a panel change is for the next launch).
        const int launched = eyesize::dlss_mode();
        if (v != 0 && v != launched) {
            BVR_LOG("DLSS: mode %d refused - this launch renders for mode %d. Only 0 (off) and %d can be switched live; "
                    "pick it in the panel and restart for anything else.", v, launched, launched);
            return;
        }
        g_mode = v;   // the render size stays as this launch set it: mode 0 = the same picture, only stretched
        for (auto& e : g_eye) { e.failed = false; e.reset = true; e.prevOk = false; }
        BVR_LOG("DLSS: evaluation mode now %d (render size unchanged - relaunch to change it)", g_mode);
        return;
    }
    if (!strcmp(what, "rebirth")) { g_rebirth = v; BVR_LOG("DLSS: rebirth %d", g_rebirth); return; }
    if (!strcmp(what, "mvguard")) { g_mvGuard = v; settings::set_i("dlss_mvguard", v); BVR_LOG("DLSS: motion guard %ld", g_mvGuard); return; }
    if (!strcmp(what, "check")) { g_check = v; BVR_LOG("dlsscheck: %s", v ? "on" : "off"); return; }
    if (!strcmp(what, "capture")) { InterlockedOr(&chk::g_capReq, 3 | 4); BVR_LOG("dlsscheck: capture asked"); return; }
    if (!strcmp(what, "reset")) { for (auto& e : g_eye) { e.reset = true; e.prevOk = false; } BVR_LOG("DLSS: both histories reset"); return; }
    if (!strcmp(what, "jitsign")) { g_jitSign = v < 0 ? -1 : 1; for (auto& e : g_eye) { e.reset = true; e.prevOk = false; } BVR_LOG("DLSS: declared jitter sign %ld", g_jitSign); return; }
    if (!strcmp(what, "preset")) { g_preset = v; settings::set_i("dlss_preset", v); }
    else if (!strcmp(what, "depthinv")) { g_depthInv = v; settings::set_i("dlss_depthinv", v); }
    else if (!strcmp(what, "mvmode")) g_mvMode = v;
    for (auto& e : g_eye) e.rw = 0; // recreate the features with the new flags
}

ID3D11Texture2D* eval(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* src, float jpx, float jpy, const float* VP,
                      bool vpOk) {
    load_settings();
    if (g_mode <= 0 || eye < 0 || eye > 1 || !src || !g_outW || !g_outH) return nullptr;
    Eye& e = g_eye[eye];
    if (e.failed) return nullptr;
    const int st = g_ngxState.load(std::memory_order_acquire);
    if (st != 2) {   // still starting in the background (or never asked to): no upscale yet, no stall either
        if (st == 0) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { start(dev); dev->Release(); } }
        ++e.skips;
        return nullptr;
    }
    if (!g_ngxOk) { e.failed = true; return nullptr; }
    const bool needGameMv = g_mvMode == 0 || g_mvMode == 2;
    if (!e.depth || (needGameMv && !e.mv)) {
        if ((e.skips++ % 300) == 0) BVR_LOG("DLSS: eye %d - no %s yet, not upscaled", eye, !e.depth ? "depth" : "motion vectors");
        return nullptr;
    }
    // MVREADY (SKVR2): never build an eye's feature before everything it will be evaluated with exists.
    if (g_mvMode > 0 && !e.h && !(vpOk && VP && mv_build())) {
        g_why[eye] = "the camera motion pass is not ready yet";
        if ((e.skips++ % 300) == 0) BVR_LOG("DLSS: eye %d - its feature is not built yet: %s", eye, g_why[eye]);
        e.reset = true;
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd);
    if (e.sinceBorn >= 0) ++e.sinceBorn;   // REBIRTH
    // V13s: OFF by default - in DS3 the rebuild broke both eyes (JJ 23:43: "one shows blue, the other not the correct
    // scene"); MVREADY alone ordered the build right. `ds dlss preset 0` still rebuilds both features live if an eye is blank.
    if (g_rebirth && e.h && !e.reborn && e.sinceBorn >= 60) {
        e.reborn = true;
        e.rw = 0;   // falls into the rebuild below
        BVR_LOG("DLSS: eye %d - rebuilding its feature once (REBIRTH: a first feature can come out blank)", eye);
    }
    if (e.h && e.rw && (e.rw != sd.Width || e.rh != sd.Height)) {
        // a different size: rebuild only if it lasts (30 frames) - a one-off render is skipped, the history kept
        if (sd.Width != e.newW || sd.Height != e.newH) { e.newW = sd.Width; e.newH = sd.Height; e.newN = 0; }
        if (++e.newN < 30) { ++e.skips; g_why[eye] = "the picture changed size"; return nullptr; }
    }
    if (e.rw != sd.Width || e.rh != sd.Height || !e.h) {
        e.newN = 0;
        if (e.in) { e.in->Release(); e.in = nullptr; }
        if (!e.out && !make_tex(g_outW, g_outH, sd.Format, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &e.out)) {
            BVR_LOG("DLSS: output texture %ux%u f%d FAILED", g_outW, g_outH, (int)sd.Format); e.failed = true; return nullptr;
        }
        if (!make_tex(sd.Width, sd.Height, sd.Format, D3D11_BIND_SHADER_RESOURCE, &e.in)) { e.failed = true; return nullptr; }
        if (!make_feature(c, e, sd.Width, sd.Height)) { e.failed = true; return nullptr; }
        e.sinceBorn = 0;
        e.reset = true;
    }
    c->CopyResource(e.in, src);
    ID3D11Resource* mvRes = e.mv; float mvScaleX = static_cast<float>(e.rw) * 0.5f, mvScaleY = static_cast<float>(e.rh) * 0.5f;
    if (g_mvMode > 0 && vpOk && VP && mv_build()) {
        D3D11_TEXTURE2D_DESC md{};
        if (e.mvOut) e.mvOut->GetDesc(&md);
        if (!e.mvOut || md.Width != e.rw || md.Height != e.rh) {
            if (e.mvUav) { e.mvUav->Release(); e.mvUav = nullptr; }
            if (e.mvOut) { e.mvOut->Release(); e.mvOut = nullptr; }
            if (make_tex(e.rw, e.rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &e.mvOut))
                g_dev->CreateUnorderedAccessView(e.mvOut, nullptr, &e.mvUav);
        }
        double VPj[16], VPu[16], inv[16], M[16];
        const double jxN = 2.0 * jpx / static_cast<double>(e.rw), jyN = -2.0 * jpy / static_cast<double>(e.rh);
        // The engine's own view-projection carries no jitter (ours goes in through the viewport, DS3VR2 VPJITTER), so
        // the matrix we were handed IS the unjittered one; the jittered one adds the shift in clip space:
        // clip.x += jxN * clip.w, clip.y += jyN * clip.w (row-vector form, the convention mul4 uses).
        for (int i = 0; i < 16; ++i) { VPu[i] = VP[i]; VPj[i] = VP[i]; }
        for (int i = 0; i < 4; ++i) { VPj[i * 4 + 0] += jxN * VPu[i * 4 + 3]; VPj[i * 4 + 1] += jyN * VPu[i * 4 + 3]; }
        ID3D11ShaderResourceView* dv = srv_for(e.depth);
        ID3D11ShaderResourceView* gv = srv_for(e.mv); // may be null in mode 1
        g_why[eye] = !e.mvUav ? "our own motion-vector texture could not be made"
                     : !dv ? "no readable view on this eye's depth"
                     : !inv4(VPj, inv) ? "the view-projection cannot be inverted (all zero?)" : "";
        if (g_why[eye][0] && (e.skips % 300) == 0)
            BVR_LOG("DLSS: eye %d matrix | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | jitter %.3f %.3f render %ux%u",
                    eye, VPj[0], VPj[1], VPj[2], VPj[3], VPj[4], VPj[5], VPj[6], VPj[7], VPj[8], VPj[9], VPj[10], VPj[11],
                    VPj[12], VPj[13], VPj[14], VPj[15], jpx, jpy, e.rw, e.rh);
        if (e.mvUav && dv && inv4(VPj, inv)) {
            if (e.prevOk) mul4(inv, e.prevVP, M); else mul4(inv, VPu, M); // first frame: no motion
            float cb[28] = {};
            for (int i = 0; i < 16; ++i) cb[i] = static_cast<float>(M[i]);
            cb[16] = static_cast<float>(e.rw); cb[17] = static_cast<float>(e.rh); cb[18] = 1.0f / static_cast<float>(e.rw); cb[19] = 1.0f / static_cast<float>(e.rh);
            cb[20] = static_cast<float>(jxN); cb[21] = static_cast<float>(jyN); cb[22] = static_cast<float>(gv ? g_mvMode : 1); cb[23] = 1.0f;
            cb[24] = g_mvGuard ? 1.0f : 0.0f;
            c->UpdateSubresource(g_mvCB, 0, nullptr, cb, 0, 0);
            ID3D11DeviceContext1* c1 = nullptr; ID3DDeviceContextState* prev = nullptr;
            if (g_state && SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1)
                c1->SwapDeviceContextState(g_state, &prev);
            ID3D11ShaderResourceView* sv[2] = {dv, gv};
            c->CSSetShader(g_mvCS, nullptr, 0); c->CSSetShaderResources(0, 2, sv); c->CSSetUnorderedAccessViews(0, 1, &e.mvUav, nullptr);
            c->CSSetConstantBuffers(0, 1, &g_mvCB);
            c->Dispatch((e.rw + 7) / 8, (e.rh + 7) / 8, 1);
            ID3D11ShaderResourceView* nsv[2] = {}; ID3D11UnorderedAccessView* nuv = nullptr;
            c->CSSetShaderResources(0, 2, nsv); c->CSSetUnorderedAccessViews(0, 1, &nuv, nullptr);
            if (c1) { if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); } c1->Release(); }
            mvRes = e.mvOut; mvScaleX = 1.0f; mvScaleY = 1.0f; // already in render pixels
        }
        for (int i = 0; i < 16; ++i) e.prevVP[i] = VPu[i];
        e.prevOk = true;
    }
    if (!mvRes) {
        if ((e.skips++ % 300) == 0)
            BVR_LOG("DLSS: eye %d - no motion vectors: %s (mode %ld, view-projection %s, depth %p)", eye, g_why[eye],
                    g_mvMode, vpOk ? "known" : "NOT known", (void*)e.depth);
        return nullptr;
    }
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = e.in; ep.Feature.pInOutput = e.out;
    ep.pInDepth = e.depth; ep.pInMotionVectors = mvRes;
    ep.InJitterOffsetX = jpx * static_cast<float>(g_jitSign); ep.InJitterOffsetY = jpy * static_cast<float>(g_jitSign);
    ep.InRenderSubrectDimensions.Width = e.rw; ep.InRenderSubrectDimensions.Height = e.rh;
    ep.InReset = e.reset ? 1 : 0; e.reset = false;
    ep.InMVScaleX = mvScaleX; ep.InMVScaleY = mvScaleY;
    ID3D11DeviceContext1* c1 = nullptr; ID3DDeviceContextState* prev = nullptr;
    if (g_state && SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1)
        c1->SwapDeviceContextState(g_state, &prev);
    Tq* tq = tq_begin(c, eye);
    const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(c, e.h, e.p, &ep);
    tq_end(c, tq);
    if (NVSDK_NGX_SUCCEED(r)) { chk::read_back(c, eye); chk::sample(c, eye, e, mvRes, VP); chk::log_window(); }
    if (c1) { if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); } c1->Release(); }
    if (NVSDK_NGX_FAILED(r)) { if ((e.skips++ % 300) == 0) BVR_LOG("DLSS: eye %d evaluate FAILED 0x%08x", eye, (unsigned)r); return nullptr; }
    ++e.evals;
    return e.out;
}

const char* diag() {
    static char s[300];
    if (g_tqN) { g_lastMs = g_tqSum / g_tqN; g_tqSum = 0; g_tqN = 0; }
    snprintf(s, sizeof(s), "DLSS mode %d scale %.2f preset %ld | evals L %ld R %ld | skipped L %ld R %ld | ngx %d (start %d) "
                           "| GPU ms per evaluate %.2f | last reason L: %s R: %s",
             g_mode, g_scale, g_preset, g_eye[0].evals, g_eye[1].evals, g_eye[0].skips, g_eye[1].skips, (int)g_ngxOk,
             g_ngxState.load(), g_lastMs, g_why[0], g_why[1]);
    return s;
}
void reset() { for (auto& e : g_eye) { e.reset = true; e.prevOk = false; } }
int take_capture_request() { return static_cast<int>(InterlockedExchange(&chk::g_capReq, 0)); }
ID3D11Resource* tex(int eye, int kind) {
    if (eye < 0 || eye > 1) return nullptr;
    const Eye& e = g_eye[eye];
    return kind == 0 ? static_cast<ID3D11Resource*>(e.in) : kind == 1 ? static_cast<ID3D11Resource*>(e.mvOut)
         : kind == 2 ? e.depth : static_cast<ID3D11Resource*>(e.out);
}
int evals(int eye) { return eye == 0 || eye == 1 ? static_cast<int>(g_eye[eye].evals) : 0; }

} // namespace bvr::ds3::dlss

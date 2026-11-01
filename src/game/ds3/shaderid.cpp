// DS3VR2 SHADERID - see shaderid.h.
#include "shaderid.h"
#include "tilefix.h"

#include "core/util/log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <intrin.h>
#include <nmmintrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace bvr::ds3::shaderid {
namespace {

// The passes L7 watches, named as in the geo11 fix's shader dumps.
struct Named { uint64_t hash; const char* name; };
constexpr Named kNames[] = {
    {0xc5e347bab1de5afbull, "volumetric light CS 1 (sun-shadow volume)"},
    {0x24be6e842c4467d8ull, "volumetric light accumulate (reads the sun-shadow volume)"},
    {0xeba9127de2c68e7full, "volumetric integrate"},
    {0xeff1b4f2d6f72973ull, "volumetric fog volume"},
    {0xb29e37013df1ed73ull, "TAA"},
    {0x3dc636dc006aa479ull, "TAA (second)"},
    {0x0f22056782a29001ull, "AO temporal"},
    {0x57ba11be28134946ull, "AO temporal (second)"},
    {0x0db837620f2133c1ull, "reflections (SSR)"},
    {0x94371e143481d51aull, "lighting (sun)"},
};

// shader object -> hash (open addressing; a released shader's address can be reused, so a create overwrites)
constexpr int kN = 32768;
std::atomic<const void*> g_key[kN];
std::atomic<uint64_t> g_val[kN];
std::atomic<uint32_t> g_created{0};
std::atomic<uint32_t> g_seen[sizeof(kNames) / sizeof(kNames[0])];   // creations per watched pass (the log may not run yet)

uint64_t fnv1_64_seed0(const void* p, size_t n) {
    uint64_t h = 0;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h *= 0x100000001b3ull; h ^= b[i]; }
    return h;
}

void remember(const void* shader, uint64_t hash) {
    const uintptr_t h = (reinterpret_cast<uintptr_t>(shader) >> 4) * 2654435761u;
    for (int probe = 0; probe < 64; ++probe) {
        const int i = static_cast<int>((h + probe) & (kN - 1));
        const void* k = g_key[i].load(std::memory_order_acquire);
        if (k == shader) { g_val[i].store(hash, std::memory_order_release); return; }
        if (!k) {
            const void* e = nullptr;
            if (g_key[i].compare_exchange_strong(e, shader) || e == shader) { g_val[i].store(hash, std::memory_order_release); return; }
        }
    }
}

// SHADERDUMP: every shader's bytecode, once, as <game>\DS3VR2\shaders\<hash>-ps.bin / -cs.bin (3Dmigoto's naming), so
// any pass can be disassembled offline (d3dcompiler D3DDisassemble). Setting shader_dump = 0 turns it off.
wchar_t g_dumpDir[MAX_PATH] = {};
bool g_dumpOn = false;
void dump_code(uint64_t h, const void* code, SIZE_T len, const char* stage) {
    if (!g_dumpOn) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s%016llx-%s.bin", g_dumpDir, static_cast<unsigned long long>(h), stage[0] == 'p' ? L"ps" : stage[0] == 'v' ? L"vs" : L"cs");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, code, static_cast<DWORD>(len), &w, nullptr);
    CloseHandle(f);
}

// SHAFTS (2026-10-30, JJ's flat screenshot: the sun beams are missing in VR): the pixel shaders that really USE the
// light-shaft values of the effects' common constant block (PSCommonBuffer g_LightShaftScreenCenter / g_ps_LightShaft*),
// found by reflection as the game creates them, so the probe can count and read exactly those draws.
std::mutex g_shaftMx;
std::vector<const void*> g_shaftPs;
void note_shaft(const void* shader, uint64_t h, const void* code, SIZE_T len) {
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(D3DReflect(code, len, IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&r))) || !r) return;
    char used[200] = {}; int k = 0;
    if (ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName("PSCommonBuffer")) {
        for (const char* n : {"g_LightShaftScreenCenter", "g_ps_LightShaftDir", "g_ps_LightShaftDustScaleOffset", "g_ps_LightShaftDustThreshold"}) {
            ID3D11ShaderReflectionVariable* v = cb->GetVariableByName(n);
            D3D11_SHADER_VARIABLE_DESC vd{};
            if (v && SUCCEEDED(v->GetDesc(&vd)) && (vd.uFlags & D3D_SVF_USED)) k += sprintf_s(used + k, sizeof(used) - k, " %s", n);
        }
    }
    r->Release();
    if (!k) return;
    { std::lock_guard<std::mutex> lk(g_shaftMx); g_shaftPs.push_back(shader); }
    BVR_LOG("shaftps: pixel shader %016llx uses%s", static_cast<unsigned long long>(h), used);
}

void note_created(const void* shader, const void* code, SIZE_T len, const char* stage) {
    const uint64_t h = fnv1_64_seed0(code, len);
    if (stage[0] == 'p') note_shaft(shader, h, code, len);
    dump_code(h, code, len, stage);
    remember(shader, h);
    g_created.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) if (kNames[i].hash == h) g_seen[i].fetch_add(1);
    if (const char* n = name_of(h)) BVR_LOG("shaderid: the game created %s %016llx = %s (%p)", stage, static_cast<unsigned long long>(h), n, shader);
}

// MARKERS (M8): the HUD's vertex shaders are kept WITH their bytecode, because the card geometry shader is generated
// from each one's output signature (there is no API that gives a created shader's code back). Unbounded on purpose -
// AKN's first try used a fixed table, the HUD's shaders came last, and the fix silently never ran (guide §5a).
std::mutex g_vsMutex;
std::map<const void*, std::vector<uint8_t>> g_vsCode;

using CreateVS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
CreateVS oCreateVS = nullptr;
HRESULT STDMETHODCALLTYPE hkCreateVS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11VertexShader** out) {
    const HRESULT hr = oCreateVS(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && code && len && len < (1u << 20)) {
        std::lock_guard<std::mutex> lk(g_vsMutex);
        auto& v = g_vsCode[*out];
        v.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + len);
    }
    // HUDSHEET: vertex shaders get their 3Dmigoto hash too (the HUD's four are named by it in the geo-11 fix)
    if (SUCCEEDED(hr) && out && *out && code && len) {
        const uint64_t h = fnv1_64_seed0(code, len);
        remember(*out, h);
        dump_code(h, code, len, "vertex shader");   // SHADERDUMP: vertex shaders too (the god-ray quad's TEXCOORD1)
    }
    return hr;
}

// TEXHASH (HUDSHEET, 2026-10-30): the game's textures named by 3Dmigoto's texture hash, as the geo-11 fix's
// [TextureOverride*] sections name them (lock-on marker, enemy bars, crosshair). CRC32C of the top mip, then of the
// 44-byte description, seeded with the first (memory `migoto-hashes-reusable`; our NieR code niervr/src/core/pure.h).
// Kept as private data on the texture, so a freed and reused pointer never carries an old texture's name.
constexpr GUID kTexHashTag = {0x6a1f4c2e, 0x2b7d, 0x4e8a, {0x9c, 0x1d, 0x5f, 0x3e, 0x7b, 0x9a, 0x0d, 0x42}};
std::atomic<uint32_t> g_texHashed{0};
uint32_t crc32c_sw(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u))); }
    return ~crc;
}
uint32_t crc32c_hw(uint32_t crc, const uint8_t* p, size_t n) {
    uint64_t c = ~crc;
    for (; n >= 8; p += 8, n -= 8) { uint64_t w; memcpy(&w, p, 8); c = _mm_crc32_u64(c, w); }
    for (; n; ++p, --n) c = _mm_crc32_u8(static_cast<uint32_t>(c), *p);
    return ~static_cast<uint32_t>(c);
}
uint32_t crc32c(uint32_t crc, const uint8_t* p, size_t n) {
    static const bool hw = [] { int r[4] = {}; __cpuid(r, 1); return (r[2] & (1 << 20)) != 0; }();
    return hw ? crc32c_hw(crc, p, n) : crc32c_sw(crc, p, n);
}
uint32_t bc_block_bytes(uint32_t f) {
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return 8;
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) return 16;
    return 0;
}
using CreateTex2D = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
CreateTex2D oCreateTex2D = nullptr;
HRESULT STDMETHODCALLTYPE hkCreateTex2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
                                        ID3D11Texture2D** out) {
    const HRESULT hr = oCreateTex2D(d, desc, init, out);
    if (SUCCEEDED(hr) && out && *out && desc && init && init->pSysMem) {
        static_assert(sizeof(D3D11_TEXTURE2D_DESC) == 44, "3Dmigoto hashes the 44-byte description");
        const uint32_t block = bc_block_bytes(desc->Format);
        const size_t v12 = static_cast<size_t>(desc->Width) * desc->Height * desc->ArraySize;
        const size_t rowBytes = block ? static_cast<size_t>((desc->Width + 3) / 4) * block : 0;
        const size_t rows = block ? (desc->Height + 3) / 4 : desc->Height;
        const size_t length = block ? rowBytes * rows : static_cast<size_t>(init->SysMemPitch) * desc->Height;
        const uint8_t* data = static_cast<const uint8_t*>(init->pSysMem);
        bool ok = true;
        uint32_t h = 0;
        if (v12 <= length) h = crc32c(0, data, v12);
        else if (!block) ok = false;
        else for (size_t r = 0; r < rows; ++r) h = crc32c(h, data + r * init->SysMemPitch, rowBytes < init->SysMemPitch ? rowBytes : init->SysMemPitch);
        if (ok) {
            h = crc32c(h, reinterpret_cast<const uint8_t*>(desc), sizeof(*desc));
            (*out)->SetPrivateData(kTexHashTag, sizeof(h), &h);
            g_texHashed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return hr;
}

using CreatePS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateCS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
CreatePS oCreatePS = nullptr;
CreateCS oCreateCS = nullptr;

HRESULT STDMETHODCALLTYPE hkCreatePS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11PixelShader** out) {
    const HRESULT hr = oCreatePS(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && code) note_created(*out, code, len, "pixel shader");
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateCS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11ComputeShader** out) {
    const HRESULT hr = oCreateCS(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && code) {
        note_created(*out, code, len, "compute shader");
        tilefix::on_created(d, fnv1_64_seed0(code, len), code, len, *out, oCreateCS);   // TILEFIX (V6)
    }
    return hr;
}

// GSPROBE (2026-11-01, the bonfire smoke billboard): its vertex shader only passes POSITION through, so a geometry shader builds
// the camera-facing card. Geometry shaders get their hash too, and their bytecode is kept so the smoke's one can be saved.
using CreateGS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**);
CreateGS oCreateGS = nullptr;
std::mutex g_gsMutex;
std::map<const void*, std::vector<uint8_t>> g_gsCode;
HRESULT STDMETHODCALLTYPE hkCreateGS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11GeometryShader** out) {
    const HRESULT hr = oCreateGS(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && code && len) {
        remember(*out, fnv1_64_seed0(code, len));
        if (len < (1u << 20)) {
            std::lock_guard<std::mutex> lk(g_gsMutex);
            g_gsCode[*out].assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + len);
        }
    }
    return hr;
}

// GSPROBE 2 (01:36:07): the smoke has NO geometry shader - hull + domain shaders, topology 33 (1-control-point patches): the
// domain shader builds the camera-facing card. Hull / domain shaders are hashed and their bytecode kept too.
using CreateHS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**);
using CreateDS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**);
CreateHS oCreateHS = nullptr;
CreateDS oCreateDS = nullptr;
void keep_code(const void* obj, const void* code, SIZE_T len) {
    remember(obj, fnv1_64_seed0(code, len));
    if (len < (1u << 20)) {
        std::lock_guard<std::mutex> lk(g_gsMutex);
        g_gsCode[obj].assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + len);
    }
}
HRESULT STDMETHODCALLTYPE hkCreateHS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11HullShader** out) {
    const HRESULT hr = oCreateHS(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && code && len) keep_code(*out, code, len);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkCreateDS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11DomainShader** out) {
    const HRESULT hr = oCreateDS(d, code, len, cl, out);
    if (SUCCEEDED(hr) && out && *out && code && len) keep_code(*out, code, len);
    return hr;
}

// A vtable slot patch, not MinHook: this runs inside the game's D3D11CreateDevice, before the framework thread has
// initialised MinHook. The slot is shared by every device of that class, which is what we want.
bool patch(void** slot, void* det, void** orig) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    *slot = det;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

} // namespace

void set_dump_dir(const wchar_t* dir) {
    swprintf_s(g_dumpDir, L"%s", dir);
    CreateDirectoryW(g_dumpDir, nullptr);
    g_dumpOn = true;
}

void install(ID3D11Device* dev) {
    static std::atomic<bool> done{false};
    if (!dev || done.exchange(true)) return;
    void** vt = *reinterpret_cast<void***>(dev);
    const bool a = patch(&vt[15], reinterpret_cast<void*>(&hkCreatePS), reinterpret_cast<void**>(&oCreatePS));
    const bool b = patch(&vt[18], reinterpret_cast<void*>(&hkCreateCS), reinterpret_cast<void**>(&oCreateCS));
    const bool c = patch(&vt[12], reinterpret_cast<void*>(&hkCreateVS), reinterpret_cast<void**>(&oCreateVS));
    const bool t = patch(&vt[5], reinterpret_cast<void*>(&hkCreateTex2D), reinterpret_cast<void**>(&oCreateTex2D));   // TEXHASH
    const bool g = patch(&vt[13], reinterpret_cast<void*>(&hkCreateGS), reinterpret_cast<void**>(&oCreateGS));   // GSPROBE
    const bool hh = patch(&vt[16], reinterpret_cast<void*>(&hkCreateHS), reinterpret_cast<void**>(&oCreateHS));   // GSPROBE 2
    const bool dd = patch(&vt[17], reinterpret_cast<void*>(&hkCreateDS), reinterpret_cast<void**>(&oCreateDS));
    BVR_LOG("shaderid: geometry / hull / domain shader creation %s / %s / %s", g ? "hooked" : "NOT hooked", hh ? "hooked" : "NOT hooked",
            dd ? "hooked" : "NOT hooked");
    BVR_LOG("shaderid: texture creation %s (3Dmigoto texture hashes, for the geo-11 fix's HUD texture lists)", t ? "hooked" : "NOT hooked");
    BVR_LOG("shaderid: pixel / compute / vertex shader creation %s / %s / %s (3Dmigoto hashes to name passes from the "
            "geo11 fix; the vertex bytecode is kept for the marker card shaders)",
            a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked", c ? "hooked" : "NOT hooked");
}

bool gs_code(const void* gs, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(g_gsMutex);
    auto it = g_gsCode.find(gs);
    if (it == g_gsCode.end()) return false;
    out = it->second;
    return true;
}

uint64_t hash_of(const void* shader) {
    if (!shader) return 0;
    const uintptr_t h = (reinterpret_cast<uintptr_t>(shader) >> 4) * 2654435761u;
    for (int probe = 0; probe < 64; ++probe) {
        const int i = static_cast<int>((h + probe) & (kN - 1));
        const void* k = g_key[i].load(std::memory_order_acquire);
        if (k == shader) return g_val[i].load(std::memory_order_acquire);
        if (!k) return 0;
    }
    return 0;
}

bool is_shaft_ps(const void* shader) {
    if (!shader) return false;
    std::lock_guard<std::mutex> lk(g_shaftMx);
    for (const void* s : g_shaftPs) if (s == shader) return true;
    return false;
}

bool tex_hash(ID3D11Resource* r, uint32_t* hash) {
    if (!r || !hash) return false;
    UINT size = sizeof(*hash);
    return SUCCEEDED(r->GetPrivateData(kTexHashTag, &size, hash)) && size == sizeof(*hash);
}
uint32_t textures_hashed() { return g_texHashed.load(); }

const char* name_of(uint64_t hash) {
    for (const Named& n : kNames) if (n.hash == hash) return n.name;
    return nullptr;
}

bool vs_code(const void* shader, const void** code, size_t* len) {
    if (!shader || !code || !len) return false;
    std::lock_guard<std::mutex> lk(g_vsMutex);
    auto it = g_vsCode.find(shader);
    if (it == g_vsCode.end() || it->second.empty()) return false;
    *code = it->second.data();
    *len = it->second.size();
    return true;
}

uint32_t vs_kept() { std::lock_guard<std::mutex> lk(g_vsMutex); return static_cast<uint32_t>(g_vsCode.size()); }

uint32_t created() { return g_created.load(std::memory_order_relaxed); }

void report() {
    char line[600]; int k = sprintf_s(line, "shaderid: %u shaders hashed; watched passes created:", created());
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]) && k < 560; ++i)
        k += sprintf_s(line + k, sizeof(line) - k, " %016llx x%u", static_cast<unsigned long long>(kNames[i].hash), g_seen[i].load());
    BVR_LOG("%s", line);
}

} // namespace bvr::ds3::shaderid

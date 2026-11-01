// DS3VR2 EYESIZE (M4) - see eyesize.h. Ported from v1 (skvr/src/hooks.cpp TALLRES / RESLIST / INTERNALRES, all LIVE /
// HEADSET on 1.6.0.0); the size now follows the headset's eye and the lens mode instead of one shared tall picture.
#include "eyesize.h"

#include "dlss_modes.h"
#include "lens.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace bvr::ds3::eyesize {
namespace {

wchar_t g_ini[MAX_PATH] = {};
bool g_on = false;
unsigned g_w = 0, g_h = 0;            // what the GAME renders at (= the eye picture x the DLSS render scale)
unsigned g_fullW = 0, g_fullH = 0;    // the eye picture itself (what DLSS reconstructs to; = g_w when DLSS is off)
int g_dlss = 0;
int g_lens = 2;
char g_status[512] = "eye size: not started";
char g_start[1024] = "";                     // what DllMain / the gate did (logged by install, when the log exists)
volatile int* g_internal = nullptr;          // the game's internal render size (0x1446601E8 / +4 in 1.15.2.0)
std::atomic<uint32_t> g_forcedCreate{0}, g_forcedResize{0};

void note(const char* fmt, ...) {
    char line[256];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    strncat_s(g_start, line, _TRUNCATE);
    strncat_s(g_start, " | ", _TRUNCATE);
}

float ini_f(const wchar_t* key, float def) {
    wchar_t b[64];
    if (!GetPrivateProfileStringW(L"ds3vr2", key, L"", b, 64, g_ini) || !b[0]) return def;
    return static_cast<float>(_wtof(b));
}

// ---- find / patch in the exe's own sections (v1 find_in_section / find_internal_res) ----
uint8_t* find_in_section(const char* secName, const uint8_t* pat, size_t n) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp(reinterpret_cast<const char*>(sec->Name), secName, 8) != 0) continue;
        uint8_t* b = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
        const size_t sz = sec->Misc.VirtualSize;
        for (size_t k = 0; k + n <= sz; ++k)
            if (memcmp(b + k, pat, n) == 0) return b + k;
    }
    return nullptr;
}
bool write_code(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// The windowed resolution list's last slot -> our size (v1 RESLIST). DS3's list has 12 entries and NO
// 10000x10001 terminator (Sekiro had one), so it is matched on its last two entries 3200x1800 + 3840x2160
// and the 3840x2160 pair is overwritten.
void patch_list() {
    static const uint8_t pat[] = {0x80, 0x0C, 0, 0,  0x08, 0x07, 0, 0,   // 3200 x 1800
                                  0x00, 0x0F, 0, 0,  0x70, 0x08, 0, 0};  // 3840 x 2160
    uint8_t* at = find_in_section(".data", pat, sizeof(pat));
    if (!at) { note("resolution list NOT found"); return; }
    uint8_t* slot = at + 8;                           // the 3840x2160 entry
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { note("resolution list not writable"); return; }
    reinterpret_cast<uint32_t*>(slot)[0] = g_w;
    reinterpret_cast<uint32_t*>(slot)[1] = g_h;
    VirtualProtect(slot, 8, old, &old);
    note("resolution list 3840x2160 -> %ux%u", g_w, g_h);
}

// The windowed-size list builder keeps an entry only if it is no larger than the desktop (two `jg`). Both become
// 2-byte NOPs so our tall/square size survives (v1 DS3 RESLIST, LIVE: the game then also SAVES that size on exit).
// Patched in DllMain, before the game builds the list.
void patch_res_filter() {
    static const uint8_t pat[] = {0x39, 0x77, 0xFC, 0x7F, 0x00, 0x44, 0x39, 0x3F, 0x7F, 0x00};
    static const bool wild[] =   {false, false, false, false, true, false, false, false, false, true};
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* hit = nullptr;
    int hits = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp(reinterpret_cast<const char*>(sec->Name), ".text", 8) != 0) continue;
        uint8_t* b = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
        for (size_t k = 0; k + sizeof(pat) <= sec->Misc.VirtualSize; ++k) {
            size_t j = 0;
            for (; j < sizeof(pat); ++j) if (!wild[j] && b[k + j] != pat[j]) break;
            if (j == sizeof(pat)) { if (!hits) hit = b + k; ++hits; }
        }
    }
    if (hits != 1) { note("desktop-size filter: %d hits (want 1) - NOT patched", hits); return; }
    static const uint8_t nop2[2] = {0x66, 0x90};
    const bool a = write_code(hit + 3, nop2, 2);      // jg over entry (width)
    const bool b2 = write_code(hit + 8, nop2, 2);     // jg over entry (height)
    note(a && b2 ? "desktop-size filter off" : "desktop-size filter not writable");
}

// GraphicsConfig.xml (UTF-16 LE, no BOM - v1 run 15): Windowed at our size. vrmode.cpp has already put VR's own copy in
// place (2D keeps its own), so this only ever changes the VR settings.
void sync_config() {
    wchar_t app[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) { note("no APPDATA"); return; }
    const std::wstring path = std::wstring(app) + L"\\DarkSoulsIII\\GraphicsConfig.xml";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { note("GraphicsConfig.xml missing - the game makes one; size applies next launch"); return; }
    std::string raw;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    raw.assign(n > 0 ? n : 0, '\0');
    fread(&raw[0], 1, raw.size(), f); fclose(f);
    bool bom = false;
    if (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFF && static_cast<uint8_t>(raw[1]) == 0xFE) { bom = true; raw.erase(0, 2); }
    if (raw.size() < 4 || raw[1] != 0) { note("GraphicsConfig not UTF-16 - unchanged"); return; }
    std::wstring x(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
    auto span = [&](const wchar_t* tag, size_t& b, size_t& e) {
        const std::wstring o = std::wstring(L"<") + tag + L">";
        b = x.find(o); if (b == std::wstring::npos) return false;
        b += o.size(); e = x.find(L"<", b); return e != std::wstring::npos;
    };
    size_t mb, me, wb, we, hb, he;
    if (!span(L"ScreenMode", mb, me) || !span(L"Resolution-WindowScreenWidth", wb, we) ||
        !span(L"Resolution-WindowScreenHeight", hb, he)) { note("GraphicsConfig fields missing - unchanged"); return; }
    const std::wstring mode = x.substr(mb, me - mb);
    const unsigned cw = static_cast<unsigned>(_wtoi(x.substr(wb, we - wb).c_str())), ch = static_cast<unsigned>(_wtoi(x.substr(hb, he - hb).c_str()));
    if (mode == L"WINDOW" && cw == g_w && ch == g_h) { note("GraphicsConfig already Windowed %ux%u", cw, ch); return; }
    // back to front, so the earlier offsets stay valid (ScreenMode comes first in the file)
    x.replace(hb, he - hb, std::to_wstring(g_h));
    x.replace(wb, we - wb, std::to_wstring(g_w));
    if (mode != L"WINDOW") x.replace(mb, me - mb, L"WINDOW");
    FILE* o = _wfopen(path.c_str(), L"wb");
    if (!o) { note("GraphicsConfig could not be written"); return; }
    if (bom) { const uint8_t b2[2] = {0xFF, 0xFE}; fwrite(b2, 1, 2, o); }
    fwrite(x.data(), sizeof(wchar_t), x.size(), o);
    fclose(o);
    note("GraphicsConfig %ls %ux%u -> WINDOW %ux%u", mode.c_str(), cw, ch, g_w, g_h);
}

// ---- swapchain: created and resized at our size while the game renders at it (v1 INTERNALRES) ----
bool rendering_ours(unsigned& w, unsigned& h) {
    if (!g_on || !g_internal) return false;
    if (static_cast<unsigned>(g_internal[0]) != g_w || static_cast<unsigned>(g_internal[1]) != g_h) return false;
    w = g_w; h = g_h;
    return true;
}
void resize_filter(unsigned* w, unsigned* h) {
    unsigned iw = 0, ih = 0;
    if (*w > 64 && rendering_ours(iw, ih) && (*w != iw || *h != ih)) { *w = iw; *h = ih; g_forcedResize.fetch_add(1); }
}
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
CreateSwapChainFn oCreateSwapChain = nullptr;
HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    unsigned iw = 0, ih = 0;
    if (d && d->BufferDesc.Width > 64 && rendering_ours(iw, ih) && (d->BufferDesc.Width != iw || d->BufferDesc.Height != ih)) {
        d->BufferDesc.Width = iw;
        d->BufferDesc.Height = ih;
        g_forcedCreate.fetch_add(1);
    }
    return oCreateSwapChain(f, dev, d, out);
}

} // namespace

void dllmain(const wchar_t* dataDir) {
    swprintf_s(g_ini, L"%ssettings.ini", dataDir);
    g_on = GetPrivateProfileIntW(L"ds3vr2", L"eye_res", 1, g_ini) != 0;
    // lens 1 = the true off-centre lens (OFFAXIS, default from 2026-10-30); 2 = the older turned eyes; 0 = one wide lens
    g_lens = static_cast<int>(GetPrivateProfileIntW(L"ds3vr2", L"lens", 1, g_ini));
    if (g_lens != 0 && g_lens != 1 && g_lens != 2) g_lens = 1;
    if (!g_on) { snprintf(g_status, sizeof(g_status), "eye size: off (eye_res = 0) - the window's size"); return; }
    // The headset as the last session saw it; JJ's Quest 3 through Virtual Desktop until then (run 3, HEADSET).
    const unsigned rw = GetPrivateProfileIntW(L"ds3vr2", L"xr_rec_w", 2284, g_ini);
    const unsigned rh = GetPrivateProfileIntW(L"ds3vr2", L"xr_rec_h", 2448, g_ini);
    float deg[8] = {-49.5f, 35.5f, 39.4f, -50.5f, -35.5f, 49.5f, 39.4f, -50.5f};
    {
        wchar_t b[256];
        if (GetPrivateProfileStringW(L"ds3vr2", L"xr_ang", L"", b, 256, g_ini) && b[0]) {
            float t[8];
            if (swscanf_s(b, L"%f,%f,%f,%f,%f,%f,%f,%f", &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 8)
                memcpy(deg, t, sizeof(deg));
        }
    }
    float ang[2][4];
    for (int i = 0; i < 8; ++i) ang[i / 4][i % 4] = deg[i] * 3.14159265f / 180.0f;
    float scale = ini_f(L"resolution", 1.0f);
    scale = scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale);
    const lens::Shape s = lens::shape(g_lens, ang);
    lens::picture_size(s, ang[0], rw, rh, scale, g_fullW, g_fullH);
    g_w = g_fullW; g_h = g_fullH;
    note("eye %ux%u (headset %ux%u, lens %s, slider x%.2f)", g_fullW, g_fullH, rw, rh, g_lens == 2 ? "turned" : g_lens == 1 ? "off-centre" : "symmetric", scale);
    // DLSS (M6): the game renders smaller and DLSS reconstructs the eye picture. Same scale table as dlss.cpp (it
    // cannot be called here - DllMain), multiples of 8 so the upscaler and our copies agree.
    // `dlss_mode` = the conventional NVIDIA modes (dlss_modes.h). A settings file from before 2026-10-30 has the
    // old `dlss` key, whose numbers meant something else: it is converted once, by scale, and removed.
    g_dlss = static_cast<int>(GetPrivateProfileIntW(L"ds3vr2", L"dlss_mode", -1, g_ini));
    if (g_dlss < 0) {
        const int old = static_cast<int>(GetPrivateProfileIntW(L"ds3vr2", L"dlss", 0, g_ini));
        g_dlss = dlss_modes::from_old_key(old);
        wchar_t b[16]; swprintf_s(b, L"%d", g_dlss);
        WritePrivateProfileStringW(L"ds3vr2", L"dlss_mode", b, g_ini);
        WritePrivateProfileStringW(L"ds3vr2", L"dlss", nullptr, g_ini);
        if (old) note("DLSS setting converted: old %d -> %s", old, dlss_modes::kModes[g_dlss].name);
    }
    if (g_dlss < 0 || g_dlss >= dlss_modes::kModeCount) g_dlss = 0;
    if (g_dlss > 0) {
        const float ds = dlss_modes::scale_of(g_dlss);
        auto r8 = [](float v) { unsigned u = (static_cast<unsigned>(v) + 4u) & ~7u; return u < 640u ? 640u : u; };
        g_w = r8(g_fullW * ds); g_h = r8(g_fullH * ds);
        note("DLSS %s: the game renders %ux%u, DLSS rebuilds %ux%u", dlss_modes::kModes[g_dlss].name, g_w, g_h, g_fullW, g_fullH);
    }
    patch_res_filter();   // before the game builds the windowed-size list
    patch_list();
    sync_config();
}

void gate() {
    if (!g_on) return;
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    // No 16:9 carve-out on DS3: the three 1.7777778 constants are the camera default, the render-camera builder's
    // Hor+ threshold (which we WANT) and one entry in a smooth float table (v1 DS3_PLAN section 5, STATIC - re-checked
    // on 1.15.2). Nothing to patch here.
    // Internal render size globals: the render-camera builder opens with
    //   mov esi,[rip+W] / mov ebp,[rip+H] / xorps xmm1,xmm1 / xorps xmm0,xmm0   (0x140CED687).
    static const uint8_t a[] = {0x8B, 0x35}, b[] = {0x8B, 0x2D}, c[] = {0x0F, 0x57, 0xC9, 0x0F, 0x57, 0xC0};
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    int sizeHits = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp(reinterpret_cast<const char*>(sec->Name), ".text", 8) != 0) continue;
        uint8_t* t = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
        for (size_t k = 0; k + 20 < sec->Misc.VirtualSize; ++k)
            if (memcmp(t + k, a, 2) == 0 && memcmp(t + k + 6, b, 2) == 0 && memcmp(t + k + 12, c, 6) == 0) {
                if (!sizeHits)
                    g_internal = reinterpret_cast<volatile int*>(t + k + 6 + *reinterpret_cast<const int32_t*>(t + k + 2));
                ++sizeHits;
            }
    }
    if (sizeHits != 1) { g_internal = nullptr; note("internal render size: %d hits (want 1)", sizeHits); }
    else note("internal render size found");
    d3d11_hook::set_resize_filter(&resize_filter); // before the game's first resize
}

void hook_factory(void* factory) {
    if (!g_on || !factory || oCreateSwapChain) return;
    void** vt = *reinterpret_cast<void***>(factory);
    DWORD old = 0;
    if (!VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &old)) return;
    oCreateSwapChain = reinterpret_cast<CreateSwapChainFn>(vt[10]);
    vt[10] = reinterpret_cast<void*>(&hkCreateSwapChain);
    VirtualProtect(&vt[10], sizeof(void*), old, &old);
}

void install() {
    BVR_LOG("eyesize: %s", g_on ? g_start : "off (eye_res = 0)");
    if (!g_on) return;
    unsigned iw = 0, ih = 0;
    internal(iw, ih);
    snprintf(g_status, sizeof(g_status), "eye size %ux%u wanted, the game renders %ux%u; swapchain sized %u / %u times", g_w, g_h,
             iw, ih, g_forcedCreate.load(), g_forcedResize.load());
    BVR_LOG("eyesize: %s", g_status);
}

// The game can re-derive its render size when a level loads (v1 DS3 run 2: it fell back to the window's size).
// Every change is logged once, so "the game renders X" is never assumed from the start-up line.
void watch() {
    if (!g_on || !g_internal) return;
    static unsigned lastW = 0, lastH = 0;
    const unsigned w = static_cast<unsigned>(g_internal[0]), h = static_cast<unsigned>(g_internal[1]);
    if (w == lastW && h == lastH) return;
    lastW = w; lastH = h;
    BVR_LOG("eyesize: the game's render size is now %ux%u (we asked for %ux%u)%s", w, h, g_w, g_h,
            (w == g_w && h == g_h) ? "" : "  <-- NOT the eye size");
}

bool wanted(unsigned& w, unsigned& h) { w = g_w; h = g_h; return g_on; }

bool full_size(unsigned& w, unsigned& h) { w = g_fullW; h = g_fullH; return g_on && g_fullW && g_fullH; }

int dlss_mode() { return g_dlss; }

bool internal(unsigned& w, unsigned& h) {
    if (!g_internal) { w = h = 0; return false; }
    w = static_cast<unsigned>(g_internal[0]); h = static_cast<unsigned>(g_internal[1]);
    return true;
}

const char* status() {
    unsigned iw = 0, ih = 0;
    internal(iw, ih);
    if (g_on)
        snprintf(g_status, sizeof(g_status), "eye size %ux%u wanted, the game renders %ux%u%s; swapchain sized %u / %u times", g_w,
                 g_h, iw, ih, (iw == g_w && ih == g_h) ? " (OK)" : " (NOT the eye size)", g_forcedCreate.load(),
                 g_forcedResize.load());
    return g_status;
}

void note_runtime(unsigned recW, unsigned recH, const float ang[2][4]) {
    if (!g_ini[0] || !recW || !recH) return;
    // Never learn the eye size or the lens angles from the FAKE headset: they size the NEXT launch, so a test run
    // used to leave the sim's 2064x2208 where JJ's Quest 3 numbers belong (memory akn-test-runs-touch-jj-settings,
    // and it bit again on 2026-10-30). The sim always arrives through a runtime_json, which sets this.
    if (GetEnvironmentVariableW(L"BVR_XRSIM_DIR", nullptr, 0) != 0) return;
    wchar_t b[256];
    swprintf_s(b, L"%u", recW); WritePrivateProfileStringW(L"ds3vr2", L"xr_rec_w", b, g_ini);
    swprintf_s(b, L"%u", recH); WritePrivateProfileStringW(L"ds3vr2", L"xr_rec_h", b, g_ini);
    const float k = 180.0f / 3.14159265f;
    swprintf_s(b, L"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", ang[0][0] * k, ang[0][1] * k, ang[0][2] * k, ang[0][3] * k,
               ang[1][0] * k, ang[1][1] * k, ang[1][2] * k, ang[1][3] * k);
    WritePrivateProfileStringW(L"ds3vr2", L"xr_ang", b, g_ini);
}

int lens_mode() { return g_lens; }

} // namespace bvr::ds3::eyesize

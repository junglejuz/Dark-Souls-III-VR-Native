// DS3VR2 - entry point (dinput8.dll).
//
// DllMain stays minimal (loader lock). DarkSoulsIII.exe imports d3d11 and dxgi statically. Its main .text is plain on
// disk, but a SECOND, Arxan-protected .text (0x144DBD000) is unpacked at run time and several function ENTRIES are
// `jmp` stubs into it, so nothing may scan or hook the game's code from DllMain or from a thread started there.
// Instead DllMain only patches the exe's import slots for its first two graphics calls (v1 bootstrap, DS3_PLAN §3);
// the slots are found by NAME, never by address, because DS3 1.15.2 has ASLR:
//   dxgi!CreateDXGIFactory  and  d3d11!D3D11CreateDevice.
// The first of them to run (in WinMain, code decrypted, no loader lock) starts the framework on its own thread
// (crash filter, log, MinHook, adapter hooks, swapchain hooks, OpenXR instance).
//
// SVRAUTO (VR_ESSENTIALS §11, AKN; BioShock VR): a game on the DXGI 1.0 factory makes every SteamVR submit fail
// (SharedTexturesNotSupported). Dark Souls III calls CreateDXGIFactory, so the gate answers with CreateDXGIFactory1 - always,
// because no geo-11 is involved in 2.0 (setting dxgi11 = 0 turns it off for a test).
#include "eyesize.h"
#include "shaderid.h"
#include "shadowres.h"

#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::framework { void init(); }
bool ds3vr2_vr_mode_decide(HINSTANCE self); // vrmode.cpp

namespace {

std::atomic<bool> g_started{false};
HINSTANCE g_self = nullptr;
wchar_t g_dataDir[MAX_PATH] = {};

using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(void*, int, HMODULE, UINT, const void*, UINT, UINT, void**, void*, void**);
CreateDXGIFactoryFn oFactory = nullptr;
D3D11CreateDeviceFn oDevice = nullptr;
volatile LONG g_gateCalls[2] = {0, 0};

DWORD WINAPI InitThread(LPVOID) {
    bvr::framework::init();
    return 0;
}

void start_framework_once() {
    if (g_started.exchange(true)) return;
    HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

bool dxgi11_enabled() {
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%ssettings.ini", g_dataDir);
    return GetPrivateProfileIntW(L"ds3vr2", L"dxgi11", 1, ini) != 0;
}

bool shader_id_enabled() {
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%ssettings.ini", g_dataDir);
    return GetPrivateProfileIntW(L"ds3vr2", L"shader_id", 1, ini) != 0;
}

HRESULT WINAPI gate_CreateDXGIFactory(REFIID riid, void** out) {
    InterlockedIncrement(&g_gateCalls[0]);
    bvr::ds3::eyesize::gate(); // M4: game thread, code decrypted - before the framework thread scans anything
    start_framework_once();
    HRESULT hr = E_FAIL;
    static auto cf1 = reinterpret_cast<CreateDXGIFactoryFn>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "CreateDXGIFactory1"));
    if (dxgi11_enabled() && cf1) hr = cf1(riid, out);
    else hr = oFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) bvr::ds3::eyesize::hook_factory(*out); // M4: the swapchain at the eye size
    return hr;
}

HRESULT WINAPI gate_D3D11CreateDevice(void* a, int dt, HMODULE sw, UINT fl, const void* lv, UINT nlv, UINT sdk, void** dev,
                                      void* ofl, void** ctx) {
    InterlockedIncrement(&g_gateCalls[1]);
    bvr::ds3::eyesize::gate();
    start_framework_once();
    const HRESULT hr = oDevice(a, dt, sw, fl, lv, nlv, sdk, dev, ofl, ctx);
    // SHADOWRES: bigger sun shadow maps (settings shadow_res), patched before the game creates any texture
    if (SUCCEEDED(hr) && dev && *dev) {
        wchar_t ini[MAX_PATH];
        swprintf_s(ini, L"%ssettings.ini", g_dataDir);
        bvr::ds3::shadowres::install(static_cast<ID3D11Device*>(*dev), ini);
    }
    // SHADERID (L7): hash the game's shaders as it creates them, to name passes by the geo11 fix's hashes
    // (a plain vtable slot patch, no MinHook trampoline: v1 run 46's crash here was MinHook under geo-11; setting
    // shader_id = 0 turns it off)
    if (SUCCEEDED(hr) && dev && *dev && shader_id_enabled()) {
        wchar_t ini[MAX_PATH], dir[MAX_PATH];
        swprintf_s(ini, L"%ssettings.ini", g_dataDir);
        if (GetPrivateProfileIntW(L"ds3vr2", L"shader_dump", 0, ini)) {
            swprintf_s(dir, L"%sshaders\\", g_dataDir);
            bvr::ds3::shaderid::set_dump_dir(dir);
        }
        bvr::ds3::shaderid::install(static_cast<ID3D11Device*>(*dev));
    }
    return hr;
}

void** find_iat_slot(HMODULE exe, const char* dll, const char* fn) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<BYTE*>(exe) + dir.VirtualAddress); imp->Name;
         ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(exe) + imp->Name, dll) != 0) continue;
        auto ft = reinterpret_cast<IMAGE_THUNK_DATA64*>(reinterpret_cast<BYTE*>(exe) + imp->FirstThunk);
        auto oft = imp->OriginalFirstThunk
                       ? reinterpret_cast<IMAGE_THUNK_DATA64*>(reinterpret_cast<BYTE*>(exe) + imp->OriginalFirstThunk)
                       : nullptr;
        if (!oft) return nullptr;
        for (int i = 0; ft[i].u1.Function; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(oft[i].u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(reinterpret_cast<BYTE*>(exe) + oft[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), fn) == 0) return reinterpret_cast<void**>(&ft[i].u1.Function);
        }
    }
    return nullptr;
}

bool patch_slot(void** slot, void* gate, void** orig) {
    if (!slot) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    *slot = gate;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

void early_init(HINSTANCE self) {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(self, p, MAX_PATH);
    if (wchar_t* s = wcsrchr(p, L'\\')) s[1] = 0;
    swprintf_s(g_dataDir, L"%sDS3VR2\\", p);
    // ReShade is installed on JJ's PC as an always-on implicit OpenXR layer and treats the ReShade.ini in Dark Souls III's folder
    // as "enabled here"; it broke xrCreateSession (v1 run 3). Its own per-process switch keeps it out of this process.
    SetEnvironmentVariableW(L"DISABLE_XR_APILAYER_reshade_1", L"1");
    // Per-launch runtime choice (VR_ESSENTIALS section 1, ACOVR aco.ini runtime_json): settings runtime_json = a manifest
    // (the fake headset ac6vr_xrsim64.json, or SteamVR's steamxr_win64.json) for THIS process only, set before the OpenXR
    // loader's first call. Runs logged as LIVE (sim), never HEADSET. Launchers can also leave it via the environment.
    {
        wchar_t ini[MAX_PATH], rj[MAX_PATH] = {}, simDir[MAX_PATH] = {};
        swprintf_s(ini, L"%ssettings.ini", g_dataDir);
        GetPrivateProfileStringW(L"ds3vr2", L"runtime_json", L"", rj, MAX_PATH, ini);
        if (rj[0]) {
            SetEnvironmentVariableW(L"XR_RUNTIME_JSON", rj);
            wcscpy_s(simDir, rj);
            if (wchar_t* s = wcsrchr(simDir, L'\\')) { *s = 0; SetEnvironmentVariableW(L"BVR_XRSIM_DIR", simDir); }
            // ONE SHOT. A test run interrupted before its cleanup used to leave this line behind, and the next
            // launch then went to the fake headset instead of JJ's own ("it's not launching into the headset",
            // 2026-10-30). The key is deleted the moment it is read; the test driver writes it fresh each launch.
            WritePrivateProfileStringW(L"ds3vr2", L"runtime_json", nullptr, ini);
        }
    }
    // M4 EYESIZE: GraphicsConfig.xml + the resolution list, before the game reads either (v1 order)
    bvr::ds3::eyesize::dllmain(g_dataDir);
    HMODULE exe = GetModuleHandleW(nullptr);
    patch_slot(find_iat_slot(exe, "dxgi.dll", "CreateDXGIFactory"), reinterpret_cast<void*>(&gate_CreateDXGIFactory),
               reinterpret_cast<void**>(&oFactory));
    patch_slot(find_iat_slot(exe, "d3d11.dll", "D3D11CreateDevice"), reinterpret_cast<void*>(&gate_D3D11CreateDevice),
               reinterpret_cast<void**>(&oDevice));
    // If neither import slot exists (another build, another bootstrap order), start on the first Present instead:
    // the framework's swapchain hooks are process-wide, so a late start still works (logged by the framework).
    if (!oFactory && !oDevice) start_framework_once();
}

} // namespace

extern "C" __declspec(dllexport) LONG DS3VR2_GateCalls(int which) { return which ? g_gateCalls[1] : g_gateCalls[0]; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        g_self = instance;
        if (ds3vr2_vr_mode_decide(instance)) early_init(instance);
    }
    // Hooks stay in place for the life of the process: removing live code at exit was AKVR's exit crash.
    return TRUE;
}

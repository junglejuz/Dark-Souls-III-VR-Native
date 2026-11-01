// DS3VR2 - DarkSoulsIII.exe imports DINPUT8 (x360ce owns xinput1_3), so the mod is dinput8.dll. The genuine
// DirectInput exports pass through to System32's copy, loaded lazily (never inside DllMain).
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

namespace {
HMODULE real_input()
{
    // Called by forwarded exports, not DllMain. Static initialization serializes
    // concurrent first calls. An absolute system path prevents proxy recursion.
    static HMODULE module = [] {
        wchar_t path[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length >= MAX_PATH) return HMODULE{};
        if (wcscat_s(path, L"\\dinput8.dll") != 0) return HMODULE{};
        return LoadLibraryW(path);
    }();
    return module;
}

template <typename T> T real_export(const char* name)
{
    HMODULE module = real_input();
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}
}

extern "C" HRESULT WINAPI pDirectInput8Create(HINSTANCE instance, DWORD version,
                                              REFIID iid, LPVOID* output,
                                              LPUNKNOWN outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    static auto fn = real_export<Fn>("DirectInput8Create");
    if (!fn) { if (output) *output = nullptr; return E_FAIL; }
    return fn(instance, version, iid, output, outer);
}

extern "C" HRESULT WINAPI pDllCanUnloadNow()
{
    // DS3VR2 owns hooks and a worker thread throughout the process lifetime.
    return S_FALSE;
}

extern "C" HRESULT WINAPI pDllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    static auto fn = real_export<Fn>("DllGetClassObject");
    if (!fn) { if (output) *output = nullptr; return E_FAIL; }
    return fn(clsid, iid, output);
}

extern "C" HRESULT WINAPI pDllRegisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    static auto fn = real_export<Fn>("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI pDllUnregisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    static auto fn = real_export<Fn>("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" LPCDIDATAFORMAT WINAPI pGetdfDIJoystick()
{
    using Fn = LPCDIDATAFORMAT(WINAPI*)();
    static auto fn = real_export<Fn>("GetdfDIJoystick");
    return fn ? fn() : nullptr;
}

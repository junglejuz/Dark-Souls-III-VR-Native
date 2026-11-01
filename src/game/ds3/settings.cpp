#include "settings.h"

#include "core/util/log.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace bvr::ds3::settings {
namespace {
wchar_t g_path[MAX_PATH] = {};

void widen(const char* s, wchar_t* out, int n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n); }
} // namespace

void load() {
    swprintf_s(g_path, L"%s\\settings.ini", bvr::log::data_dir());
    const bool have = GetFileAttributesW(g_path) != INVALID_FILE_ATTRIBUTES;
    BVR_LOG("settings: %s", have ? "loaded DS3VR2\\settings.ini" : "no settings.ini yet - defaults (written on first change)");
}

const wchar_t* path() { return g_path; }

float f(const char* key, float def) {
    wchar_t k[64], buf[64];
    widen(key, k, 64);
    if (!GetPrivateProfileStringW(L"ds3vr2", k, L"", buf, 64, g_path) || !buf[0]) return def;
    return static_cast<float>(_wtof(buf));
}

int i(const char* key, int def) {
    wchar_t k[64];
    widen(key, k, 64);
    return static_cast<int>(GetPrivateProfileIntW(L"ds3vr2", k, def, g_path));
}

void set_f(const char* key, float v) {
    wchar_t k[64], buf[64];
    widen(key, k, 64);
    swprintf_s(buf, L"%.4f", v);
    WritePrivateProfileStringW(L"ds3vr2", k, buf, g_path);
}

void set_i(const char* key, int v) {
    wchar_t k[64], buf[32];
    widen(key, k, 64);
    swprintf_s(buf, L"%d", v);
    WritePrivateProfileStringW(L"ds3vr2", k, buf, g_path);
}

} // namespace bvr::ds3::settings

#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace bvr::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
wchar_t g_dataDir[MAX_PATH] = {};

} // namespace

void init(const wchar_t* subdir) {
    std::lock_guard lock(g_mutex);
    if (g_file) return;

    // DS3VR2: the mod's files live next to the game, in <game>\DS3VR2\ (v1's TIDYFOLDER layout; the
    // game folder is writable, unlike Documents on some PCs - memory launcher-documents-write-denied).
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = 0;
    swprintf_s(g_dataDir, L"%s\\DS3VR2", exe);
    CreateDirectoryW(g_dataDir, nullptr);
    if (subdir && subdir[0]) {
        wchar_t withSub[MAX_PATH];
        swprintf_s(withSub, L"%s\\%s", g_dataDir, subdir);
        wcscpy_s(g_dataDir, withSub);
        CreateDirectoryW(g_dataDir, nullptr);
    }

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\ds3vr2_log.txt", g_dataDir);

    // Keep ONE generation of history: a crashing run survives as ds3vr2_log_previous.txt until the
    // launch after next (players send the wrong log otherwise - Batman session 23, SKVR TRAVELSAFE).
    wchar_t prev[MAX_PATH];
    swprintf_s(prev, L"%s\\ds3vr2_log_previous.txt", g_dataDir);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING); // no-op on first run

    // _SH_DENYNO so a tail tool can follow the file while the game runs.
    g_file = _wfsopen(path, L"w", _SH_DENYNO);
}

const wchar_t* data_dir() {
    return g_dataDir;
}

void write(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(message, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard lock(g_mutex);
    if (g_file) {
        fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
        fflush(g_file); // the process may die at any moment - never lose lines
    }
    char dbg[2100];
    sprintf_s(dbg, "[ds3vr2] %s\n", message);
    OutputDebugStringA(dbg);
}

} // namespace bvr::log

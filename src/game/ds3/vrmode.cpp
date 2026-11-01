// DS3VR2 - VR or flat (2D), decided at process start (v1 VRLAUNCH, run 135; skvr/src/sk_vrmode.cpp).
//
// JJ (2026-10-02): launching from Steam must give the normal 2D game; only the VR launcher starts VR. The mod's
// dinput8.dll is always loaded by the game. It runs in VR only when
//   - the VR launcher (DS3VR2-Launch-VR.bat) left DS3VR2\ds3vr2_launch_vr.flag within the last 3 minutes (used up at once), or
//   - the game was started with -ds3vr2 (Steam launch options: always VR).
// Otherwise nothing of DS3VR2 starts: no hooks, no OpenXR - the dinput8 exports still pass through.
//
// The game keeps different graphics settings in VR (windowed at the VR size) in %APPDATA%\DarkSoulsIII\GraphicsConfig.xml
// (UTF-16 LE, no BOM - v1 run 15). Each mode keeps its own copy in DS3VR2\profiles\{2d,vr}\; when the mode changes, the file
// as the last session left it goes to that mode's copy and the other mode's copy comes back, so a crashed VR session still
// gives a normal 2D start.
//
// Runs inside DllMain (loader lock): only kernel32 file calls and environment reads (no LoadLibrary, no COM).
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>

namespace {
    wchar_t g_dir[MAX_PATH] = L"";       // the game folder, with a trailing backslash
    wchar_t g_data[MAX_PATH] = L"";      // <game>\DS3VR2\ - the mod's own files
    bool g_vr = false;

    void join(wchar_t* out, const wchar_t* a, const wchar_t* b) { wcscpy_s(out, MAX_PATH, a); wcscat_s(out, MAX_PATH, b); }
    bool exists(const wchar_t* p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

    void log_line(const wchar_t* fmt, ...)
    {
        wchar_t p[MAX_PATH]; join(p, g_data, L"ds3vr2_mode_log.txt");
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 32768) DeleteFileW(p);   // stays small
        wchar_t msg[600];
        SYSTEMTIME st{}; GetLocalTime(&st);
        int n = _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond);
        va_list ap; va_start(ap, fmt);
        if (n > 0) _vsnwprintf_s(msg + n, _countof(msg) - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        wcscat_s(msg, L"\r\n");
        HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        char a[1200]; const int len = WideCharToMultiByte(CP_UTF8, 0, msg, -1, a, sizeof(a), nullptr, nullptr);
        DWORD w = 0; if (len > 1) WriteFile(h, a, (DWORD)(len - 1), &w, nullptr);
        CloseHandle(h);
    }
    bool read_small(const wchar_t* p, char* buf, DWORD cap)
    {
        buf[0] = 0;
        HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD r = 0; const BOOL ok = ReadFile(h, buf, cap - 1, &r, nullptr);
        CloseHandle(h);
        buf[ok ? r : 0] = 0;
        return ok != FALSE;
    }
    void write_small(const wchar_t* p, const char* s)
    {
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0; WriteFile(h, s, (DWORD)strlen(s), &w, nullptr);
        CloseHandle(h);
    }
    const wchar_t* kName = L"GraphicsConfig.xml";
    void prof_path(wchar_t* out, const wchar_t* mode)
    {
        join(out, g_data, L"profiles\\"); wcscat_s(out, MAX_PATH, mode); wcscat_s(out, MAX_PATH, L"\\"); wcscat_s(out, MAX_PATH, kName);
    }
    void make_dirs(const wchar_t* mode)
    {
        wchar_t d[MAX_PATH]; join(d, g_data, L"profiles\\"); CreateDirectoryW(d, nullptr);
        wcscat_s(d, mode); CreateDirectoryW(d, nullptr);
    }
}

// true = run DS3VR2 (VR); false = stay out of the game entirely (2D)
bool ds3vr2_vr_mode_decide(HINSTANCE self)
{
    wchar_t mod[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, mod, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;                      // cannot tell: stay out (2D is always safe)
    wchar_t* slash = wcsrchr(mod, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    wcscpy_s(g_dir, mod);
    join(g_data, g_dir, L"DS3VR2\\");
    CreateDirectoryW(g_data, nullptr);

    // ---- the mode ----
    const wchar_t* why = L"no VR launch note (started from Steam)";
    wchar_t flag[MAX_PATH]; join(flag, g_data, L"ds3vr2_launch_vr.flag");
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(flag, GetFileExInfoStandard, &fa))
    {
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        const ULONGLONG t = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        const ULONGLONG c = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        const bool fresh = c >= t ? (c - t) < 180ULL * 10000000ULL : true;
        DeleteFileW(flag);                                       // used up either way
        if (fresh) { g_vr = true; why = L"VR launcher"; }
        else why = L"VR launch note older than 3 minutes (ignored)";
    }
    const wchar_t* cmd = GetCommandLineW();
    if (!g_vr && cmd && (wcsstr(cmd, L" -ds3vr2") || wcsstr(cmd, L" -DS3VR2"))) { g_vr = true; why = L"-ds3vr2 on the command line"; }
    log_line(L"start in %s (%s)", g_vr ? L"VR" : L"2D", why);

    // ---- the game's graphics settings for this mode ----
    wchar_t live[MAX_PATH] = L"";
    {
        wchar_t app[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) join(live, app, L"\\DarkSoulsIII\\GraphicsConfig.xml");
    }
    wchar_t lastPath[MAX_PATH]; join(lastPath, g_data, L"profiles\\last.txt");
    char last[16] = "";
    if (!read_small(lastPath, last, sizeof(last)))
    {
        // First start of 2.0: the live file is the player's own 2D settings (an installer backs it up too).
        strcpy_s(last, "2d");
        log_line(L"first start of DS3VR2: the current graphics settings are taken as 2D's");
    }
    const bool lastVr = _strnicmp(last, "vr", 2) == 0;
    if (live[0] && lastVr != g_vr)
    {
        const wchar_t* from = lastVr ? L"vr" : L"2d";
        const wchar_t* to = g_vr ? L"vr" : L"2d";
        if (exists(live)) { make_dirs(from); wchar_t d[MAX_PATH]; prof_path(d, from); const BOOL ok = CopyFileW(live, d, FALSE); log_line(L"  saved %s settings: %s", from, ok ? L"ok" : L"FAILED"); }
        wchar_t s[MAX_PATH]; prof_path(s, to);
        if (exists(s)) { const BOOL ok = CopyFileW(s, live, FALSE); log_line(L"  %s settings back: %s", to, ok ? L"ok" : L"FAILED"); }
        else log_line(L"  no %s copy yet - %s", to, g_vr ? L"the mod sets VR's up" : L"the game keeps the current file");
    }
    make_dirs(g_vr ? L"vr" : L"2d");
    write_small(lastPath, g_vr ? "vr" : "2d");
    return g_vr;
}

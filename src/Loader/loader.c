/*
 * UMvC3 Mod Loader - dinput8.dll proxy
 *
 * This DLL is deployed into the game folder (umvc3.exe directory). Because the
 * game statically imports DINPUT8.dll, Windows loads this proxy at startup.
 * It performs two jobs:
 *
 *  1. VIRTUAL FILE SYSTEM
 *     Hook kernelbase CreateFileW/A and GetFileAttributes(W/A/Ex) and redirect
 *     any read under the game directory to the first enabled mod (in priority
 *     order) that provides the same relative file. Mod content is never copied
 *     into the game folder; it is served directly from the mod's own folder.
 *
 *  2. PLUGIN LOADER
 *     Loads each enabled mod's *.asi plugins (and any root-level runtime DLLs
 *     they need, e.g. debug CRT) directly from the mod folder, mirroring what
 *     the mods' own dinput8 loaders would do.
 *
 * The DINPUT8 exports themselves are forwarded to the real system dinput8.dll
 * by loading it from System32 and forwarding via GetProcAddress, so the game's
 * DirectInput usage is unaffected. (We cannot use .def forwarders: a forwarder
 * named "dinput8.X" would resolve to our own already-loaded module and recurse.)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include "MinHook.h"

#define MAX_MODS 128
#define MAX_PATH_LEN 2048

static const char LOADER_MARKER[] __attribute__((used)) = "UMVC3-MOD-LOADER-v1"; /* manager scans for these bytes */
static const char  CONFIG_NAME[]    = "mods.ini";
static const char  LOG_NAME[]       = "mods_loader.log";
static const wchar_t LOADER_DLL_NAME[] = L"dinput8.dll";

static wchar_t g_moduleDir[MAX_PATH];   /* directory of this DLL == game dir */
static wchar_t g_mods[MAX_MODS][MAX_PATH_LEN];
static int     g_modCount   = 0;
static BOOL    g_enabled    = FALSE;
static BOOL    g_logging    = FALSE;
static HANDLE g_logHandle  = INVALID_HANDLE_VALUE;

/* Real function pointers (filled by MH_CreateHookApi). Must be declared before
   resolve_redirect, which uses the real GetFileAttributesW to avoid recursion. */
static HANDLE(WINAPI *RealCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *RealCreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
static DWORD(WINAPI *RealGetFileAttributesW)(LPCWSTR);
static DWORD(WINAPI *RealGetFileAttributesA)(LPCSTR);
static BOOL (WINAPI *RealGetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static BOOL (WINAPI *RealGetFileAttributesExA)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static void(WINAPI *RealGetStartupInfoW)(LPSTARTUPINFOW);
static void(WINAPI *RealGetStartupInfoA)(LPSTARTUPINFOA);

/* NtCreateFile / NtOpenFile hooks. The game's content loader (MT Framework)
   reads nativePCx64 files through ntdll directly, bypassing CreateFileW, so we
   also hook these to serve mod files. Types come from winternl.h. */
#include <winternl.h>
typedef NTSTATUS(NTAPI *pNtCreateFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                       PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                                       ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS(NTAPI *pNtOpenFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                     PIO_STATUS_BLOCK, ULONG, ULONG);
static pNtCreateFile RealNtCreateFile;
static pNtOpenFile   RealNtOpenFile;

/* INI profile API. Mod ASIs (CloneEngine, ColorExpansion) read their configs
   (.\\Characters.ini, ColorExpansion.ini) through these instead of CreateFile,
   and Windows' profile reader opens the file through a path our file hooks
   never see. We swap lpFileName to the mod's real file before the API reads it. */
static UINT  (WINAPI *RealGetPrivateProfileStringA)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
static UINT  (WINAPI *RealGetPrivateProfileStringW)(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, LPCWSTR);
static UINT  (WINAPI *RealGetPrivateProfileIntA)(LPCSTR, LPCSTR, INT, LPCSTR);
static UINT  (WINAPI *RealGetPrivateProfileIntW)(LPCWSTR, LPCWSTR, INT, LPCWSTR);
static DWORD (WINAPI *RealGetPrivateProfileSectionA)(LPCSTR, LPSTR, DWORD, LPCSTR);
static DWORD (WINAPI *RealGetPrivateProfileSectionW)(LPCWSTR, LPWSTR, DWORD, LPCWSTR);
static DWORD (WINAPI *RealGetPrivateProfileSectionNamesA)(LPSTR, DWORD, LPCSTR);
static DWORD (WINAPI *RealGetPrivateProfileSectionNamesW)(LPWSTR, DWORD, LPCWSTR);
static BOOL  (WINAPI *RealWritePrivateProfileStringA)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
static BOOL  (WINAPI *RealWritePrivateProfileStringW)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);

static volatile LONG g_pluginsLoaded = 0;

/* Temp diagnostic counters */
static volatile LONG g_readsSeen  = 0;
static volatile LONG g_redirects  = 0;
static volatile LONG g_ntCalls    = 0;
static volatile LONG g_ntRedirs   = 0;
static volatile LONG g_ntRootFail = 0;
static volatile LONG g_openTrace  = 0;
static void log_line(const wchar_t *fmt, ...);
#define OPEN_TRACE_MAX 600
static void trace_open(const wchar_t *api, const wchar_t *path, BOOL redirected)
{
    if (InterlockedIncrement(&g_openTrace) <= OPEN_TRACE_MAX)
        log_line(L"OPEN %ls %ls -> %ls", api, path != NULL ? path : L"(null)",
                 redirected ? L"REDIR" : L"real");
}

/* ------------------------------------------------------------------ */
/* dinput8 export forwarding                                          */
/* ------------------------------------------------------------------ */

/*
 * Loads the real system dinput8.dll from System32 and caches the module.
 * Called from DllMain (single-threaded at process attach) and lazily by the
 * exported wrappers. A full path is required: a bare "dinput8.dll" would
 * resolve back to this very proxy and recurse.
 */
static HMODULE real_dinput8(void)
{
    static HMODULE hReal = NULL;
    if (hReal != NULL)
        return hReal;

    wchar_t sysdir[MAX_PATH];
    if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0)
        return NULL;

    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\dinput8.dll", sysdir);
    hReal = LoadLibraryW(path);
    return hReal;
}

#define FORWARD_GET(name)                                                    \
    static FARPROC name##_proc(void)                                         \
    {                                                                        \
        static FARPROC p = NULL;                                             \
        if (p == NULL)                                                       \
        {                                                                    \
            HMODULE h = real_dinput8();                                      \
            if (h != NULL)                                                   \
                p = GetProcAddress(h, #name);                                \
        }                                                                    \
        return p;                                                            \
    }

FORWARD_GET(DirectInput8Create)
FORWARD_GET(DllCanUnloadNow)
FORWARD_GET(DllGetClassObject)
FORWARD_GET(DllRegisterServer)
FORWARD_GET(DllUnregisterServer)
FORWARD_GET(GetdfDIJoystick)

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, LPCGUID riidltf,
                                  LPVOID *ppvOut, void *punkOuter)
{
    typedef HRESULT(WINAPI *fn_t)(HINSTANCE, DWORD, LPCGUID, LPVOID *, void *);
    fn_t fn = (fn_t)DirectInput8Create_proc();
    return fn != NULL ? fn(hinst, dwVersion, riidltf, ppvOut, punkOuter) : (HRESULT)0x80004005L;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    typedef HRESULT(WINAPI *fn_t)(void);
    fn_t fn = (fn_t)DllCanUnloadNow_proc();
    return fn != NULL ? fn() : (HRESULT)0x80004005L;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID FAR *ppv)
{
    typedef HRESULT(WINAPI *fn_t)(REFCLSID, REFIID, LPVOID FAR *);
    fn_t fn = (fn_t)DllGetClassObject_proc();
    return fn != NULL ? fn(rclsid, riid, ppv) : (HRESULT)0x80004005L;
}

HRESULT WINAPI DllRegisterServer(void)
{
    typedef HRESULT(WINAPI *fn_t)(void);
    fn_t fn = (fn_t)DllRegisterServer_proc();
    return fn != NULL ? fn() : (HRESULT)0x80004005L;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    typedef HRESULT(WINAPI *fn_t)(void);
    fn_t fn = (fn_t)DllUnregisterServer_proc();
    return fn != NULL ? fn() : (HRESULT)0x80004005L;
}

LPVOID WINAPI GetdfDIJoystick(void)
{
    typedef LPVOID(WINAPI *fn_t)(void);
    fn_t fn = (fn_t)GetdfDIJoystick_proc();
    return fn != NULL ? fn() : NULL;
}

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

static void log_open(void)
{
    if (!g_logging)
        return;

    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\%hs", g_moduleDir, LOG_NAME);
    g_logHandle = CreateFileW(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void log_line(const wchar_t *fmt, ...)
{
    if (g_logHandle == INVALID_HANDLE_VALUE)
        return;

    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);

    DWORD written = 0;
    DWORD len = (DWORD)wcslen(buf);
    WriteFile(g_logHandle, buf, len * sizeof(wchar_t), &written, NULL);
    WriteFile(g_logHandle, L"\r\n", 2 * sizeof(wchar_t), &written, NULL);
    FlushFileBuffers(g_logHandle);
}

/* ------------------------------------------------------------------ */
/* config parsing (mods.ini, UTF-8)                                   */
/* ------------------------------------------------------------------ */

static int is_space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

static BOOL parse_bool_value(const wchar_t *value)
{
    if (value == NULL || value[0] == L'\0')
        return FALSE;
    if (value[0] == L'1' || value[0] == L'T' || value[0] == L't')
        return TRUE;
    if (value[0] == L'0' || value[0] == L'F' || value[0] == L'f')
        return FALSE;
    return _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0 || _wcsicmp(value, L"on") == 0;
}

static void trim(wchar_t *s)
{
    wchar_t *p = s;
    while (*p && is_space(*p)) p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));

    size_t n = wcslen(s);
    while (n > 0 && is_space(s[n - 1])) s[--n] = L'\0';
}

static void parse_config(void)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\%hs", g_moduleDir, CONFIG_NAME);

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    char raw[4096];
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &read, NULL) && read > 0;
    CloseHandle(h);
    if (!ok)
        return;
    raw[read] = '\0';

    wchar_t text[4096];
    int textLen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)read, text, 4095);
    if (textLen <= 0)
        return;
    text[textLen] = L'\0';

    BOOL in_mods = FALSE;
    wchar_t *ctx = NULL;
    wchar_t *line = wcstok(text, L"\n", &ctx);
    while (line != NULL)
    {
        trim(line);
        if (line[0] == L'\0' || line[0] == L';' || line[0] == L'#')
        {
            line = wcstok(NULL, L"\n", &ctx);
            continue;
        }
        if (line[0] == L'[')
        {
            in_mods = (_wcsicmp(line, L"[Mods]") == 0);
            line = wcstok(NULL, L"\n", &ctx);
            continue;
        }

        if (in_mods)
        {
            wchar_t *eq = wcschr(line, L'=');
            if (eq != NULL && g_modCount < MAX_MODS)
            {
                wchar_t *val = eq + 1;
                trim(val);
                if (val[0] != L'\0')
                {
                    wcscpy(g_mods[g_modCount], val);
                    g_modCount++;
                }
            }
        }
        else if (_wcsicmp(line, L"Enabled=0") == 0)
        {
            g_enabled = FALSE;
        }
        else if (wcsncmp(line, L"Enabled=", 8) == 0)
        {
            g_enabled = TRUE;
        }
        else if (_wcsnicmp(line, L"Log=", 4) == 0)
        {
            g_logging = parse_bool_value(line + 4);
        }
        else if (_wcsnicmp(line, L"Logging=", 8) == 0)
        {
            g_logging = parse_bool_value(line + 8);
        }

        line = wcstok(NULL, L"\n", &ctx);
    }

    /* Reference the marker so its ASCII bytes stay in the image (manager scans for it). */
    log_line(L"marker %hs", LOADER_MARKER);

    log_line(L"config: enabled=%d mods=%d", g_enabled ? 1 : 0, g_modCount);
    for (int i = 0; i < g_modCount; i++)
        log_line(L"  mod[%d] = %ls", i, g_mods[i]);
}

/* ------------------------------------------------------------------ */
/* path redirection                                                   */
/* ------------------------------------------------------------------ */

static BOOL is_absolute(const wchar_t *p)
{
    return (p[0] == L'\\' && p[1] == L'\\') ||
           (iswalpha(p[0]) && p[1] == L':');
}

static BOOL path_starts_with(const wchar_t *path, const wchar_t *prefix)
{
    return _wcsnicmp(path, prefix, wcslen(prefix)) == 0;
}

/*
 * If `requested` is a read that maps under the game dir and an enabled mod
 * provides the same relative file, fills `out` with the mod's path and
 * returns TRUE. Returns FALSE to let the request hit the real filesystem.
 */
static BOOL resolve_redirect(const wchar_t *requested, wchar_t *out, size_t outcap)
{
    if (requested == NULL || requested[0] == L'\0' || g_modCount == 0 || !g_enabled)
        return FALSE;

    /* Build an absolute path anchored at the game dir. */
    wchar_t full[MAX_PATH_LEN];
    if (is_absolute(requested))
    {
        wcscpy(full, requested);
    }
    else
    {
        _snwprintf(full, MAX_PATH_LEN, L"%ls\\%ls", g_moduleDir, requested);
    }

    /* Canonicalize (resolves .., slashes, dots). */
    wchar_t canon[MAX_PATH_LEN];
    DWORD canonLen = GetFullPathNameW(full, MAX_PATH_LEN, canon, NULL);
    if (canonLen == 0 || canonLen >= MAX_PATH_LEN)
        return FALSE;

    /* Only files under the game dir are in scope. */
    if (!path_starts_with(canon, g_moduleDir))
        return FALSE;
    if (canon[wcslen(g_moduleDir)] != L'\\')
        return FALSE;

    const wchar_t *rel = canon + wcslen(g_moduleDir) + 1;
    if (rel[0] == L'\0')
        return FALSE;

    /* Never redirect the loader's own DLL. */
    if (_wcsicmp(rel, LOADER_DLL_NAME) == 0)
        return FALSE;

    for (int i = 0; i < g_modCount; i++)
    {
        wchar_t cand[MAX_PATH_LEN];
        _snwprintf(cand, MAX_PATH_LEN, L"%ls\\%ls", g_mods[i], rel);

        DWORD attrs = RealGetFileAttributesW(cand);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            wcsncpy(out, cand, outcap - 1);
            out[outcap - 1] = L'\0';
            return TRUE;
        }
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* hooks                                                              */
/* ------------------------------------------------------------------ */

static BOOL is_write_access(DWORD access)
{
    return (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL)) != 0;
}

/* Temp diagnostic: log requests that touch the ASI-loaded content roots, the
   mods' own config/log files, or anything under the mod folder, so we can see
   what the ASIs actually open and whether our VFS is involved. */
static BOOL should_trace_path(const wchar_t *p)
{
    if (p == NULL) return FALSE;
    if (wcsstr(p, L"CloneEngine") != NULL) return TRUE;
    if (wcsstr(p, L"ColorExpand") != NULL) return TRUE;
    if (wcsstr(p, L"ColorExpansion") != NULL) return TRUE;
    if (wcsstr(p, L"Characters.ini") != NULL) return TRUE;
    if (wcsstr(p, L"ColorExpansion.ini") != NULL) return TRUE;
    if (wcsstr(p, L"Log.txt") != NULL) return TRUE;
    if (wcsstr(p, L"Community Edition") != NULL) return TRUE;
    return FALSE;
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
                                     DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSA,
                                     DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                                     HANDLE hTemplateFile)
{
    BOOL traced = FALSE;
    if (!is_write_access(dwDesiredAccess))
    {
        InterlockedIncrement(&g_readsSeen);
        BOOL interesting = should_trace_path(lpFileName) ||
                           wcsstr(lpFileName, L"\\ui\\") != NULL ||
                           wcsstr(lpFileName, L"/ui/") != NULL ||
                           wcsstr(lpFileName, L"mnchs") != NULL ||
                           wcsstr(lpFileName, L"mnmain") != NULL;
        wchar_t redirected[MAX_PATH_LEN];
        if (resolve_redirect(lpFileName, redirected, MAX_PATH_LEN))
        {
            InterlockedIncrement(&g_redirects);
            trace_open(L"CFW", lpFileName, TRUE);
            if (interesting)
                log_line(L"CFW redirect: %ls -> %ls", lpFileName, redirected);
            traced = should_trace_path(lpFileName);
            lpFileName = redirected;
        }
        else
        {
            trace_open(L"CFW", lpFileName, FALSE);
            if (interesting)
                log_line(L"CFW miss:     %ls", lpFileName);
        }
    }
    HANDLE h = RealCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSA,
                               dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    if (traced)
        log_line(L"CFW result:   %ls (h=%p)", lpFileName, (void *)h);
    return h;
}

static HANDLE WINAPI HookCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
                                     DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSA,
                                     DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                                     HANDLE hTemplateFile)
{
    if (!is_write_access(dwDesiredAccess))
    {
        wchar_t wide[MAX_PATH_LEN];
        if (lpFileName != NULL &&
            MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide, MAX_PATH_LEN) > 0)
        {
            InterlockedIncrement(&g_readsSeen);
            wchar_t redirected[MAX_PATH_LEN];
            if (resolve_redirect(wide, redirected, MAX_PATH_LEN))
            {
                InterlockedIncrement(&g_redirects);
                trace_open(L"CFA", wide, TRUE);
                char narrow[MAX_PATH_LEN];
                if (WideCharToMultiByte(CP_ACP, 0, redirected, -1, narrow, MAX_PATH_LEN, NULL, NULL) > 0)
                    lpFileName = narrow;
            }
            else
            {
                trace_open(L"CFA", wide, FALSE);
            }
        }
    }
    return RealCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSA,
                           dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static DWORD WINAPI HookGetFileAttributesW(LPCWSTR lpFileName)
{
    wchar_t redirected[MAX_PATH_LEN];
    if (resolve_redirect(lpFileName, redirected, MAX_PATH_LEN))
    {
        if (should_trace_path(lpFileName))
            log_line(L"GFAW redirect: %ls -> %ls", lpFileName, redirected);
        return RealGetFileAttributesW(redirected);
    }
    if (should_trace_path(lpFileName))
        log_line(L"GFAW miss:     %ls", lpFileName);
    return RealGetFileAttributesW(lpFileName);
}

static DWORD WINAPI HookGetFileAttributesA(LPCSTR lpFileName)
{
    wchar_t wide[MAX_PATH_LEN];
    if (lpFileName != NULL &&
        MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide, MAX_PATH_LEN) > 0)
    {
        wchar_t redirected[MAX_PATH_LEN];
        if (resolve_redirect(wide, redirected, MAX_PATH_LEN))
        {
            char narrow[MAX_PATH_LEN];
            WideCharToMultiByte(CP_ACP, 0, redirected, -1, narrow, MAX_PATH_LEN, NULL, NULL);
            return RealGetFileAttributesA(narrow);
        }
    }
    return RealGetFileAttributesA(lpFileName);
}

static BOOL WINAPI HookGetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS infoLevel, LPVOID info)
{
    wchar_t redirected[MAX_PATH_LEN];
    if (resolve_redirect(lpFileName, redirected, MAX_PATH_LEN))
        return RealGetFileAttributesExW(redirected, infoLevel, info);
    return RealGetFileAttributesExW(lpFileName, infoLevel, info);
}

static BOOL WINAPI HookGetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS infoLevel, LPVOID info)
{
    wchar_t wide[MAX_PATH_LEN];
    if (lpFileName != NULL &&
        MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide, MAX_PATH_LEN) > 0)
    {
        wchar_t redirected[MAX_PATH_LEN];
        if (resolve_redirect(wide, redirected, MAX_PATH_LEN))
        {
            char narrow[MAX_PATH_LEN];
            WideCharToMultiByte(CP_ACP, 0, redirected, -1, narrow, MAX_PATH_LEN, NULL, NULL);
            return RealGetFileAttributesExA(narrow, infoLevel, info);
        }
    }
    return RealGetFileAttributesExA(lpFileName, infoLevel, info);
}

/* ------------------------------------------------------------------ */
/* INI profile API hooks                                              */
/* ------------------------------------------------------------------ */

static const wchar_t *redirect_profile_path(const wchar_t *name, wchar_t *out, size_t cap)
{
    if (name == NULL || name[0] == L'\0' || g_modCount == 0 || !g_enabled)
        return name;
    if (resolve_redirect(name, out, cap))
    {
        log_line(L"PPROF redirect: %ls -> %ls", name, out);
        return out;
    }
    if (should_trace_path(name))
        log_line(L"PPROF miss:     %ls", name);
    return name;
}

static const char *redirect_profile_path_a(const char *name, char *narrow, size_t cap)
{
    if (name == NULL || name[0] == '\0' || g_modCount == 0 || !g_enabled)
        return name;
    wchar_t wide[MAX_PATH_LEN], redir[MAX_PATH_LEN];
    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH_LEN) <= 0)
        return name;
    const wchar_t *r = redirect_profile_path(wide, redir, MAX_PATH_LEN);
    if (r == wide)
        return name;
    if (WideCharToMultiByte(CP_ACP, 0, r, -1, narrow, (DWORD)cap, NULL, NULL) <= 0)
        return name;
    return narrow;
}

static UINT WINAPI HookGetPrivateProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName,
                                                LPCSTR lpDefault, LPSTR lpReturnedString,
                                                DWORD nSize, LPCSTR lpFileName)
{
    char redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path_a(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileStringA(lpAppName, lpKeyName, lpDefault,
                                        lpReturnedString, nSize, lpFileName);
}

static UINT WINAPI HookGetPrivateProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName,
                                                LPCWSTR lpDefault, LPWSTR lpReturnedString,
                                                DWORD nSize, LPCWSTR lpFileName)
{
    wchar_t redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileStringW(lpAppName, lpKeyName, lpDefault,
                                        lpReturnedString, nSize, lpFileName);
}

static UINT WINAPI HookGetPrivateProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName,
                                             INT nDefault, LPCSTR lpFileName)
{
    char redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path_a(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileIntA(lpAppName, lpKeyName, nDefault, lpFileName);
}

static UINT WINAPI HookGetPrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName,
                                             INT nDefault, LPCWSTR lpFileName)
{
    wchar_t redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileIntW(lpAppName, lpKeyName, nDefault, lpFileName);
}

static DWORD WINAPI HookGetPrivateProfileSectionA(LPCSTR lpAppName, LPSTR lpReturnedString,
                                                  DWORD nSize, LPCSTR lpFileName)
{
    char redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path_a(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileSectionA(lpAppName, lpReturnedString, nSize, lpFileName);
}

static DWORD WINAPI HookGetPrivateProfileSectionW(LPCWSTR lpAppName, LPWSTR lpReturnedString,
                                                  DWORD nSize, LPCWSTR lpFileName)
{
    wchar_t redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileSectionW(lpAppName, lpReturnedString, nSize, lpFileName);
}

static DWORD WINAPI HookGetPrivateProfileSectionNamesA(LPSTR lpReturnedString, DWORD nSize,
                                                       LPCSTR lpFileName)
{
    char redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path_a(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileSectionNamesA(lpReturnedString, nSize, lpFileName);
}

static DWORD WINAPI HookGetPrivateProfileSectionNamesW(LPWSTR lpReturnedString, DWORD nSize,
                                                       LPCWSTR lpFileName)
{
    wchar_t redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path(lpFileName, redir, MAX_PATH_LEN);
    return RealGetPrivateProfileSectionNamesW(lpReturnedString, nSize, lpFileName);
}

static BOOL WINAPI HookWritePrivateProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName,
                                                  LPCSTR lpString, LPCSTR lpFileName)
{
    char redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path_a(lpFileName, redir, MAX_PATH_LEN);
    return RealWritePrivateProfileStringA(lpAppName, lpKeyName, lpString, lpFileName);
}

static BOOL WINAPI HookWritePrivateProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName,
                                                  LPCWSTR lpString, LPCWSTR lpFileName)
{
    wchar_t redir[MAX_PATH_LEN];
    lpFileName = redirect_profile_path(lpFileName, redir, MAX_PATH_LEN);
    return RealWritePrivateProfileStringW(lpAppName, lpKeyName, lpString, lpFileName);
}

/* ------------------------------------------------------------------ */
/* ntdll NtCreateFile / NtOpenFile hooks                              */
/* ------------------------------------------------------------------ */

/* Converts an NT object path ("\??\C:\...", "\DosDevices\C:\...") to a win32
   path. Returns FALSE for paths we should not rewrite. */
static BOOL nt_path_to_win32(const wchar_t *nt, wchar_t *win, size_t cap)
{
    if (nt == NULL || nt[0] == L'\0')
        return FALSE;
    if (_wcsnicmp(nt, L"\\??\\", 4) == 0)
    {
        wcsncpy(win, nt + 4, cap - 1);
        win[cap - 1] = L'\0';
        return TRUE;
    }
    if (_wcsnicmp(nt, L"\\DosDevices\\", 12) == 0)
    {
        wcsncpy(win, nt + 12, cap - 1);
        win[cap - 1] = L'\0';
        return TRUE;
    }
    if (nt[1] == L':' && ((nt[0] >= L'A' && nt[0] <= L'Z') || (nt[0] >= L'a' && nt[0] <= L'z')))
    {
        wcsncpy(win, nt, cap - 1);
        win[cap - 1] = L'\0';
        return TRUE;
    }
    return FALSE;
}

static BOOL nt_is_write_access(ACCESS_MASK access)
{
    return (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA |
                      FILE_WRITE_ATTRIBUTES | FILE_ALL_ACCESS | GENERIC_ALL)) != 0;
}

/* MT Framework opens the nativePCx64 directory handle once and opens every file
   relative to that handle (RootDirectory != NULL). Cache handle -> resolved path
   so we can turn those relative opens into full paths for resolve_redirect. */
#define DIRCACHE_N 32
static HANDLE  g_dirCacheHandle[DIRCACHE_N];
static wchar_t g_dirCachePath[DIRCACHE_N][MAX_PATH];
static int     g_dirCacheCount = 0;

static BOOL root_handle_path(HANDLE h, wchar_t *out, size_t cap)
{
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return FALSE;

    for (int i = 0; i < g_dirCacheCount; i++)
    {
        if (g_dirCacheHandle[i] == h)
        {
            wcsncpy(out, g_dirCachePath[i], cap - 1);
            out[cap - 1] = L'\0';
            return TRUE;
        }
    }

    DWORD n = GetFinalPathNameByHandleW(h, out, (DWORD)cap, 0);
    if (n == 0 || n >= cap)
        return FALSE;
    out[cap - 1] = L'\0';

    if (_wcsnicmp(out, L"\\\\?\\", 4) == 0)
        memmove(out, out + 4, (wcslen(out + 4) + 1) * sizeof(wchar_t));

    if (g_dirCacheCount < DIRCACHE_N)
    {
        g_dirCacheHandle[g_dirCacheCount] = h;
        wcscpy(g_dirCachePath[g_dirCacheCount], out);
        g_dirCacheCount++;
    }
    return TRUE;
}

/* Holds the rewritten OBJECT_ATTRIBUTES for a redirected open. */
typedef struct
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    name;
    wchar_t           ntBuf[MAX_PATH_LEN + 8];
} NtRedirect;

/* If `in` describes a read open under the game dir that an enabled mod can
   satisfy, fills `r` with an absolute \??\ path and returns TRUE. */
static BOOL try_nt_redirect(POBJECT_ATTRIBUTES in, ACCESS_MASK desired, NtRedirect *r,
                            const wchar_t *tag)
{
    if (in == NULL || in->ObjectName == NULL)
    {
        if (InterlockedIncrement(&g_ntCalls) <= OPEN_TRACE_MAX)
            log_line(L"NTC ??:        oa=%p name=NULL", (void *)in);
        return FALSE;
    }
    if (nt_is_write_access(desired))
        return FALSE;
    InterlockedIncrement(&g_ntCalls);

    const wchar_t *on = in->ObjectName->Buffer;
    wchar_t win[MAX_PATH_LEN];

    if (in->RootDirectory == NULL)
    {
        if (!nt_path_to_win32(on, win, MAX_PATH_LEN))
        {
            if (on[0] == L'\\')
            {
                if (g_ntCalls <= OPEN_TRACE_MAX)
                    log_line(L"NTC skip:     %ls (unmapped NT path)", on);
                return FALSE; /* other NT-rooted paths we cannot map */
            }
            wcsncpy(win, on, MAX_PATH_LEN - 1);
            win[MAX_PATH_LEN - 1] = L'\0';
        }
    }
    else
    {
        /* relative open against a directory handle */
        wchar_t root[MAX_PATH_LEN];
        if (!root_handle_path(in->RootDirectory, root, MAX_PATH_LEN))
        {
            InterlockedIncrement(&g_ntRootFail);
            if (g_ntCalls <= OPEN_TRACE_MAX)
                log_line(L"NTC rootfail: h=%p name=%ls", (void *)in->RootDirectory, on);
            return FALSE;
        }
        const wchar_t *rel = on;
        while (*rel == L'\\')
            rel++;
        _snwprintf(win, MAX_PATH_LEN, L"%ls\\%ls", root, rel);
    }

    wchar_t redirected[MAX_PATH_LEN];
    if (!resolve_redirect(win, redirected, MAX_PATH_LEN))
    {
        if (should_trace_path(win) || wcsstr(win, L"nativePCx64") != NULL)
            log_line(L"%ls miss:     %ls", tag, win);
        else
            trace_open(tag, win, FALSE);
        return FALSE;
    }

    InterlockedIncrement(&g_ntRedirs);
    trace_open(tag, win, TRUE);
    _snwprintf(r->ntBuf, MAX_PATH_LEN + 8, L"\\??\\%ls", redirected);
    r->name.Length = (USHORT)(wcslen(r->ntBuf) * sizeof(wchar_t));
    r->name.MaximumLength = r->name.Length + 2;
    r->name.Buffer = r->ntBuf;
    r->oa.Length = sizeof(OBJECT_ATTRIBUTES);
    r->oa.RootDirectory = NULL;
    r->oa.ObjectName = &r->name;
    r->oa.Attributes = in->Attributes;
    r->oa.SecurityDescriptor = in->SecurityDescriptor;
    r->oa.SecurityQualityOfService = in->SecurityQualityOfService;

    if (should_trace_path(win))
        log_line(L"%ls redirect: %ls -> %ls", tag, win, redirected);
    else if (wcsstr(win, L"nativePCx64") != NULL)
        log_line(L"%ls ok:       %ls -> %ls", tag, win, redirected);
    return TRUE;
}

static NTSTATUS NTAPI HookNtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                       POBJECT_ATTRIBUTES ObjectAttributes,
                                       PIO_STATUS_BLOCK IoStatusBlock,
                                       PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                       ULONG ShareAccess, ULONG CreateDisposition,
                                       ULONG CreateOptions, PVOID EaBuffer,
                                       ULONG EaBufferLength)
{
    NtRedirect r;
    if (try_nt_redirect(ObjectAttributes, DesiredAccess, &r, L"NTC"))
        return RealNtCreateFile(FileHandle, DesiredAccess, &r.oa,
                                IoStatusBlock, AllocationSize, FileAttributes,
                                ShareAccess, CreateDisposition, CreateOptions,
                                EaBuffer, EaBufferLength);
    return RealNtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                            AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                            CreateOptions, EaBuffer, EaBufferLength);
}

static NTSTATUS NTAPI HookNtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                     POBJECT_ATTRIBUTES ObjectAttributes,
                                     PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess,
                                     ULONG OpenOptions)
{
    NtRedirect r;
    if (try_nt_redirect(ObjectAttributes, DesiredAccess, &r, L"NTO"))
        return RealNtOpenFile(FileHandle, DesiredAccess, &r.oa,
                              IoStatusBlock, ShareAccess, OpenOptions);
    return RealNtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                          ShareAccess, OpenOptions);
}

/* ------------------------------------------------------------------ */
/* plugin loading                                                     */
/* ------------------------------------------------------------------ */

static int wcs_compare(const void *a, const void *b)
{
    return _wcsicmp(*(const wchar_t **)a, *(const wchar_t **)b);
}

/*
 * A mod that ships its own "dinput8.dll" is a self-contained loader (e.g. a
 * ThirteenAG Ultimate ASI Loader build). Trying to load its .asi plugins with
 * our own code would drop the loader environment those plugins expect (mag_patch
 * under the CE mod crashes with "Address contains an unsupported instruction"
 * unless its own UAL is active). For such mods we run THEIR loader instead and
 * keep our VFS redirection for the game's file reads.
 */
static BOOL mod_has_loader(const wchar_t *modDir)
{
    wchar_t path[MAX_PATH_LEN];
    _snwprintf(path, MAX_PATH_LEN, L"%ls\\dinput8.dll", modDir);
    DWORD attrs = RealGetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void load_mod_loader(const wchar_t *modDir)
{
    wchar_t path[MAX_PATH_LEN];
    _snwprintf(path, MAX_PATH_LEN, L"%ls\\dinput8.dll", modDir);
    HMODULE m = LoadLibraryW(path);
    log_line(L"mod loader %ls -> %ls", path, m != NULL ? L"ok" : L"FAILED");
}

/* Diagnostic: report which of the mod's DLLs/ASIs are present in the process. */
static void log_mod_modules(const wchar_t *modDir)
{
    wchar_t pattern[MAX_PATH_LEN];

    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*.dll", modDir);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (_wcsicmp(fd.cFileName, LOADER_DLL_NAME) == 0)
                continue;
            HMODULE m = GetModuleHandleW(fd.cFileName);
            log_line(L"module %ls -> %ls", fd.cFileName, m != NULL ? L"loaded" : L"MISSING");
        } while (FindNextFileW(hFind, &fd) != 0);
        FindClose(hFind);
    }

    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*.asi", modDir);
    hFind = FindFirstFileW(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            HMODULE m = GetModuleHandleW(fd.cFileName);
            log_line(L"module %ls -> %ls", fd.cFileName, m != NULL ? L"loaded" : L"MISSING");
        } while (FindNextFileW(hFind, &fd) != 0);
        FindClose(hFind);
    }
}

static void load_plugins_from(const wchar_t *modDir)
{
    /* Mirror Ultimate ASI Loader: set CWD to the folder holding the plugins so
       ASIs that use relative paths (logs, configs) resolve them correctly. */
    wchar_t oldDir[MAX_PATH_LEN];
    DWORD oldDirLen = GetCurrentDirectoryW(MAX_PATH_LEN, oldDir);
    SetCurrentDirectoryW(modDir);

    wchar_t *names[512];
    int count = 0;

    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*.dll", modDir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (_wcsicmp(fd.cFileName, LOADER_DLL_NAME) == 0)
                continue;
            if (count < 512)
                names[count++] = _wcsdup(fd.cFileName);
        } while (FindNextFileW(hFind, &fd) != 0);
        FindClose(hFind);
    }

    qsort(names, count, sizeof(wchar_t *), wcs_compare);
    for (int i = 0; i < count; i++)
    {
        wchar_t full[MAX_PATH_LEN];
        _snwprintf(full, MAX_PATH_LEN, L"%ls\\%ls", modDir, names[i]);
        HMODULE m = LoadLibraryW(full);
        log_line(L"preload %ls -> %ls", names[i], m != NULL ? L"ok" : L"FAILED");
        free(names[i]);
    }

    count = 0;
    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*.asi", modDir);
    hFind = FindFirstFileW(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (count < 512)
                names[count++] = _wcsdup(fd.cFileName);
        } while (FindNextFileW(hFind, &fd) != 0);
        FindClose(hFind);
    }

    qsort(names, count, sizeof(wchar_t *), wcs_compare);
    for (int i = 0; i < count; i++)
    {
        wchar_t full[MAX_PATH_LEN];
        _snwprintf(full, MAX_PATH_LEN, L"%ls\\%ls", modDir, names[i]);
        HMODULE m = LoadLibraryW(full);
        log_line(L"asi %ls -> %ls", names[i], m != NULL ? L"ok" : L"FAILED");
        free(names[i]);
    }

    if (oldDirLen > 0)
        SetCurrentDirectoryW(oldDir);
}

/*
 * Loads every enabled mod's plugins. Deferred out of DllMain: the mods' own
 * loaders (Ultimate ASI Loader) deliberately do NOT load .asi from process
 * attach -- they wait until the game's CRT calls GetStartupInfoW so the loader
 * lock is released and the game code is in a state hooks can be installed into.
 * Runs exactly once, guarded like UAL's LoadPluginsAndRestoreIAT.
 */
/* Runs a few seconds after startup so the mod's own loader has finished its
   plugin pass; logs which mod modules actually made it into the process. */
static DWORD WINAPI diagnostic_thread(LPVOID param)
{
    (void)param;
    Sleep(2000);
    log_line(L"--- module state (2s after startup) ---");
    wchar_t cwd[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0)
        log_line(L"process CWD = %ls", cwd);
    log_line(L"reads seen=%ld redirects=%ld ntCalls=%ld ntRedirs=%ld ntRootFail=%ld",
             g_readsSeen, g_redirects, g_ntCalls, g_ntRedirs, g_ntRootFail);
    for (int i = 0; i < g_modCount; i++)
        log_mod_modules(g_mods[i]);
    return 0;
}

/*
 * Loads every enabled mod's plugins. Deferred out of DllMain: the mods' own
 * loaders (Ultimate ASI Loader) deliberately do NOT load .asi from process
 * attach -- they wait until the game's CRT calls GetStartupInfoW so the loader
 * lock is released and the game code is in a state hooks can be installed into.
 * Runs exactly once, guarded like UAL's LoadPluginsAndRestoreIAT.
 *
 * Mods that ship their own dinput8.dll are delegated to that loader entirely
 * (preloaded at process attach): loading their .asi ourselves crashes them
 * (mag_patch: "Address contains an unsupported instruction").
 */
static void load_plugins_deferred(void)
{
    if (InterlockedCompareExchange(&g_pluginsLoaded, 1, 0) != 0)
        return;
    if (!g_enabled)
        return;

    for (int i = 0; i < g_modCount; i++)
    {
        if (mod_has_loader(g_mods[i]))
            log_line(L"mod has own loader; plugin loading delegated");
        else
            load_plugins_from(g_mods[i]);
    }
    log_line(L"plugins loaded (deferred)");
    CreateThread(NULL, 0, diagnostic_thread, NULL, 0, NULL);
}

static void WINAPI HookGetStartupInfoW(LPSTARTUPINFOW lpStartupInfo)
{
    load_plugins_deferred();
    RealGetStartupInfoW(lpStartupInfo);
}

static void WINAPI HookGetStartupInfoA(LPSTARTUPINFOA lpStartupInfo)
{
    load_plugins_deferred();
    RealGetStartupInfoA(lpStartupInfo);
}

/* ------------------------------------------------------------------ */
/* setup                                                              */
/* ------------------------------------------------------------------ */

static void init_module_dir(void)
{
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&init_module_dir, &self);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);

    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash != NULL)
        *slash = L'\0';
    wcscpy(g_moduleDir, path);
}

static void install_hooks(void)
{
    if (MH_Initialize() != MH_OK)
    {
        log_line(L"MH_Initialize failed");
        return;
    }

    MH_CreateHookApi(L"kernelbase.dll", "CreateFileW",
                     &HookCreateFileW, (LPVOID *)&RealCreateFileW);
    MH_CreateHookApi(L"kernelbase.dll", "CreateFileA",
                     &HookCreateFileA, (LPVOID *)&RealCreateFileA);
    MH_CreateHookApi(L"kernelbase.dll", "GetFileAttributesW",
                     &HookGetFileAttributesW, (LPVOID *)&RealGetFileAttributesW);
    MH_CreateHookApi(L"kernelbase.dll", "GetFileAttributesA",
                     &HookGetFileAttributesA, (LPVOID *)&RealGetFileAttributesA);
    MH_CreateHookApi(L"kernelbase.dll", "GetFileAttributesExW",
                     &HookGetFileAttributesExW, (LPVOID *)&RealGetFileAttributesExW);
    MH_CreateHookApi(L"kernelbase.dll", "GetFileAttributesExA",
                     &HookGetFileAttributesExA, (LPVOID *)&RealGetFileAttributesExA);
    MH_CreateHookApi(L"kernelbase.dll", "GetStartupInfoW",
                     &HookGetStartupInfoW, (LPVOID *)&RealGetStartupInfoW);
    MH_CreateHookApi(L"kernelbase.dll", "GetStartupInfoA",
                     &HookGetStartupInfoA, (LPVOID *)&RealGetStartupInfoA);
    MH_CreateHookApi(L"ntdll.dll", "NtCreateFile",
                     &HookNtCreateFile, (LPVOID *)&RealNtCreateFile);
    MH_CreateHookApi(L"ntdll.dll", "NtOpenFile",
                     &HookNtOpenFile, (LPVOID *)&RealNtOpenFile);

    /* INI profile API (implemented in kernel32, not forwarded to kernelbase).
       The mod ASIs read their configs here; Windows' profile reader bypasses
       our file hooks, so we redirect lpFileName itself. */
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileStringA",
                     &HookGetPrivateProfileStringA, (LPVOID *)&RealGetPrivateProfileStringA);
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileStringW",
                     &HookGetPrivateProfileStringW, (LPVOID *)&RealGetPrivateProfileStringW);
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileIntA",
                     &HookGetPrivateProfileIntA, (LPVOID *)&RealGetPrivateProfileIntA);
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileIntW",
                     &HookGetPrivateProfileIntW, (LPVOID *)&RealGetPrivateProfileIntW);
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileSectionA",
                     &HookGetPrivateProfileSectionA, (LPVOID *)&RealGetPrivateProfileSectionA);
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileSectionW",
                     &HookGetPrivateProfileSectionW, (LPVOID *)&RealGetPrivateProfileSectionW);
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileSectionNamesA",
                     &HookGetPrivateProfileSectionNamesA, (LPVOID *)&RealGetPrivateProfileSectionNamesA);
    MH_CreateHookApi(L"kernel32.dll", "GetPrivateProfileSectionNamesW",
                     &HookGetPrivateProfileSectionNamesW, (LPVOID *)&RealGetPrivateProfileSectionNamesW);
    MH_CreateHookApi(L"kernel32.dll", "WritePrivateProfileStringA",
                     &HookWritePrivateProfileStringA, (LPVOID *)&RealWritePrivateProfileStringA);
    MH_CreateHookApi(L"kernel32.dll", "WritePrivateProfileStringW",
                     &HookWritePrivateProfileStringW, (LPVOID *)&RealWritePrivateProfileStringW);

    MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    log_line(L"hooks installed (MH status=%d; CFW=%p CFA=%p NTC=%p NTO=%p)",
             (int)st, (void *)RealCreateFileW, (void *)RealCreateFileA,
             (void *)RealNtCreateFile, (void *)RealNtOpenFile);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;

    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);
        init_module_dir();
        parse_config();
        log_open();
        log_line(L"=== UMvC3 Mod Loader v1 ===");
        install_hooks();
        if (g_enabled)
        {
            for (int i = 0; i < g_modCount; i++)
            {
                if (mod_has_loader(g_mods[i]))
                    load_mod_loader(g_mods[i]);
            }
        }
        log_line(L"loader ready (mod loaders preloaded, plugins deferred)");
    }

    return TRUE;
}

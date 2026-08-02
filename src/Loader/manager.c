/*
 * UMVC3 Mod Manager - mods.ini editor (built as UMVC3ModManager.asi)
 *
 * The loader (dinput8.dll) loads this .asi from the game folder before the
 * game boots and calls the exported UMVC3_ModManager_Show(). The GUI blocks
 * the game's startup while the user edits mods.ini: "Launch Game" continues
 * the boot, "Exit" aborts it.
 *
 * Modeled on the MZZXLC Mod Loader GUI:
 *   - plain Win32 on the calling thread (the game's main thread during CRT
 *     startup). No extra threads (they deadlock under the loader lock here)
 *     and no COM (the modern IFileDialog needs a clean STA apartment which
 *     cannot be guaranteed in this process).
 *   - mods are auto-discovered from the "Mods" subfolder next to this ASI on
 *     startup and via the Refresh button, same as MZZXLC scans its "mods"
 *     folder. There is no Add Mod dialog. If a discovered folder contains a
 *     "nativePCx64" directory somewhere in its tree, the folder that directly
 *     contains nativePCx64 is used as the mod root (e.g. mod\sub\thing\
 *     nativePCx64 -> mod root is "thing"), so the VFS pathing lines up.
 *
 * It edits the same mods.ini the loader reads:
 *
 *   [Loader]
 *   Version=1
 *   Enabled=1        <- master switch (checkbox)
 *   Manager=1        <- show this window on launch (checkbox)
 *   Logging=0        <- preserved from previous runs
 *
 *   [Mods]           <- every known mod, in priority order (1 wins)
 *   1=C:\path\to\MyMod
 *
 *   [Disabled]       <- mods unchecked in the list
 *   1=C:\path\to\OtherMod
 *
 * Mod content is never copied into the game folder; it is served from each
 * mod's own folder by the loader's VFS.
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_MODS      128
#define MAX_PATH_LEN  2048
#define CONFIG_NAME   "mods.ini"
#define MODS_DIR      L"Mods"

#define IDC_LIST       101
#define IDC_MASTER     102
#define IDC_SHOWNEXT   103
#define IDC_REFRESH    104
#define IDC_UP         105
#define IDC_EXIT       106
#define IDC_LAUNCH     107
#define IDC_STATUS     108
#define IDC_DOWN       109
#define IDC_HINT       110

typedef struct
{
    wchar_t path[MAX_PATH_LEN];
    BOOL    enabled;
} ModEntry;

/* Loose-file mod: a publisher ships the mod file alone (no nativePCx64 folder).
   `file` is the absolute path to the actual file in the Mods folder; `virtual`
   is the game-relative path it replaces in the game's nativePCx64, resolved by
   cross-referencing against the game once and saved to mods.ini. */
typedef struct
{
    wchar_t file[MAX_PATH_LEN];
    wchar_t virtual[MAX_PATH_LEN];
} FileMod;

static ModEntry g_mods[MAX_MODS];
static int      g_modCount      = 0;
static FileMod  g_fileMods[MAX_MODS];
static int      g_fileModCount  = 0;
static BOOL     g_masterEnabled = TRUE;
static BOOL     g_showNextTime  = TRUE;
static BOOL     g_logging       = FALSE;
static wchar_t  g_baseDir[MAX_PATH_LEN];
static wchar_t  g_configPath[MAX_PATH_LEN];

/* One-time index of the game's nativePCx64: lowercased base name -> path
   relative to g_baseDir. Built lazily on the first loose-file lookup so the
   per-file full-tree walk (find_in_game) only ever happens once per manager
   session instead of once per loose file. Buffers sized for the actual game
   (10,357 files, longest name 39 chars, longest relative path 65 chars). */
#define GAME_NAME_LEN   48
#define GAME_REL_LEN    160
#define MAX_GAME_FILES  20000

static wchar_t g_gameIndexName[MAX_GAME_FILES][GAME_NAME_LEN];
static wchar_t g_gameIndexRel[MAX_GAME_FILES][GAME_REL_LEN];
static int      g_gameIndexCount = 0;
static BOOL     g_gameIndexBuilt = FALSE;

static HWND g_hwnd     = NULL;
static HWND g_list     = NULL;
static HWND g_master   = NULL;
static HWND g_showNext = NULL;
static HWND g_status   = NULL;
static HWND g_hint     = NULL;

static int g_threadResult = FALSE;

static int is_space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

/* Temporary diagnostic: append a line to <baseDir>\mgr_debug.log. */
static void mgr_log(const wchar_t *fmt, ...)
{
    if (g_baseDir[0] == L'\0')
        return;
    wchar_t path[MAX_PATH_LEN];
    _snwprintf(path, MAX_PATH_LEN, L"%ls\\mgr_debug.log", g_baseDir);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 511;
    if (n > 511)
        n = 511;
    buf[n] = L'\0';
    char utf8[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, n, utf8, sizeof(utf8) - 2, NULL, NULL);
    if (len > 0)
    {
        utf8[len++] = '\r';
        utf8[len++] = '\n';
        DWORD written = 0;
        WriteFile(h, utf8, (DWORD)len, &written, NULL);
    }
    CloseHandle(h);
}

static void trim(wchar_t *s)
{
    wchar_t *p = s;
    while (*p && is_space(*p)) p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    size_t n = wcslen(s);
    while (n > 0 && is_space(s[n - 1])) s[--n] = L'\0';
}

static BOOL parse_bool(const wchar_t *v)
{
    if (v == NULL || v[0] == L'\0') return FALSE;
    if (v[0] == L'1' || v[0] == L'T' || v[0] == L't') return TRUE;
    if (v[0] == L'0' || v[0] == L'F' || v[0] == L'f') return FALSE;
    return _wcsicmp(v, L"true") == 0 || _wcsicmp(v, L"yes") == 0 || _wcsicmp(v, L"on") == 0;
}

/* Returns TRUE if mods.ini existed and was read. */
static BOOL read_mods_ini(void)
{
    g_modCount = 0;
    g_fileModCount = 0;
    g_masterEnabled = TRUE;
    g_showNextTime = TRUE;
    g_logging = FALSE;

    HANDLE h = CreateFileW(g_configPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    char raw[4096];
    DWORD got = 0;
    BOOL ok = ReadFile(h, raw, sizeof(raw) - 1, &got, NULL) && got > 0;
    CloseHandle(h);
    if (!ok)
        return FALSE;
    raw[got] = '\0';

    wchar_t text[4096];
    int textLen = MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, text, 4095);
    if (textLen <= 0)
        return FALSE;
    text[textLen] = L'\0';

    int section = 0;   /* 0 = none, 1 = [Mods], 2 = [Disabled], 3 = [ModFiles] */
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
            if (_wcsicmp(line, L"[Mods]") == 0)           section = 1;
            else if (_wcsicmp(line, L"[Disabled]") == 0)  section = 2;
            else if (_wcsicmp(line, L"[ModFiles]") == 0)  section = 3;
            else section = 0;
            line = wcstok(NULL, L"\n", &ctx);
            continue;
        }
        if (section == 1)
        {
            wchar_t *eq = wcschr(line, L'=');
            if (eq != NULL && g_modCount < MAX_MODS)
            {
                wchar_t *val = eq + 1;
                trim(val);
                if (val[0] != L'\0')
                {
                    wcscpy(g_mods[g_modCount].path, val);
                    g_mods[g_modCount].enabled = TRUE;
                    g_modCount++;
                }
            }
        }
        else if (section == 2)
        {
            wchar_t *eq = wcschr(line, L'=');
            if (eq != NULL)
            {
                wchar_t val[MAX_PATH_LEN];
                wcscpy(val, eq + 1);
                trim(val);
                if (val[0] != L'\0')
                {
                    for (int i = 0; i < g_modCount; i++)
                        if (_wcsicmp(g_mods[i].path, val) == 0)
                            g_mods[i].enabled = FALSE;
                }
            }
        }
        else if (section == 3)
        {
            /* N=<absolute path to mod file>|<game-relative virtual path> */
            wchar_t *eq = wcschr(line, L'=');
            if (eq != NULL && g_fileModCount < MAX_MODS)
            {
                wchar_t *val = eq + 1;
                trim(val);
                wchar_t *pipe = wcschr(val, L'|');
                if (pipe != NULL)
                {
                    *pipe = L'\0';
                    trim(val);
                    wchar_t *vpath = pipe + 1;
                    trim(vpath);
                    if (val[0] != L'\0' && vpath[0] != L'\0')
                    {
                        wcscpy(g_fileMods[g_fileModCount].file, val);
                        wcscpy(g_fileMods[g_fileModCount].virtual, vpath);
                        g_fileModCount++;
                    }
                }
            }
        }
        else if (_wcsnicmp(line, L"Enabled=", 8) == 0)
            g_masterEnabled = parse_bool(line + 8);
        else if (_wcsnicmp(line, L"Manager=", 8) == 0)
            g_showNextTime = parse_bool(line + 8);
        else if (_wcsnicmp(line, L"Logging=", 8) == 0)
            g_logging = parse_bool(line + 8);
        else if (_wcsnicmp(line, L"Log=", 4) == 0)
            g_logging = parse_bool(line + 4);
        line = wcstok(NULL, L"\n", &ctx);
    }
    return TRUE;
}

static void write_mods_ini(void)
{
    /* Snapshot the checkbox states straight from the list before saving. */
    if (g_list != NULL)
    {
        for (int i = 0; i < g_modCount; i++)
            g_mods[i].enabled = ListView_GetCheckState(g_list, i) != 0;
    }

    wchar_t out[8192];
    int len = 0;
    len += _snwprintf(out + len, 8191 - len,
                      L"[Loader]\r\nVersion=1\r\nEnabled=%d\r\nManager=%d\r\nLogging=%d\r\n",
                      g_masterEnabled ? 1 : 0, g_showNextTime ? 1 : 0, g_logging ? 1 : 0);
    len += _snwprintf(out + len, 8191 - len, L"\r\n[Mods]\r\n");
    for (int i = 0; i < g_modCount; i++)
        len += _snwprintf(out + len, 8191 - len, L"%d=%ls\r\n", i + 1, g_mods[i].path);

    int disabledCount = 0;
    for (int i = 0; i < g_modCount; i++)
        if (!g_mods[i].enabled)
            disabledCount++;
    if (disabledCount > 0)
    {
        len += _snwprintf(out + len, 8191 - len, L"\r\n[Disabled]\r\n");
        int n = 0;
        for (int i = 0; i < g_modCount; i++)
            if (!g_mods[i].enabled)
                len += _snwprintf(out + len, 8191 - len, L"%d=%ls\r\n", ++n, g_mods[i].path);
    }

    /* Loose-file mods: save the cross-referenced mapping so the loader never
       needs to scan the game's nativePCx64 again. Only keep mappings that still
       belong to a known mod folder. */
    int fmWrote = 0;
    for (int i = 0; i < g_fileModCount; i++)
    {
        BOOL known = FALSE;
        for (int j = 0; j < g_modCount && !known; j++)
        {
            size_t p = wcslen(g_mods[j].path);
            if (_wcsnicmp(g_fileMods[i].file, g_mods[j].path, p) == 0 &&
                g_fileMods[i].file[p] == L'\\')
                known = TRUE;
        }
        if (known)
        {
            if (fmWrote == 0)
                len += _snwprintf(out + len, 8191 - len, L"\r\n[ModFiles]\r\n");
            len += _snwprintf(out + len, 8191 - len, L"%d=%ls|%ls\r\n", ++fmWrote,
                              g_fileMods[i].file, g_fileMods[i].virtual);
        }
    }
    g_fileModCount = fmWrote;

    char utf8[16384];
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, out, len, utf8, sizeof(utf8) - 1, NULL, NULL);
    if (utf8Len <= 0)
        return;
    utf8[utf8Len] = '\0';

    HANDLE h = CreateFileW(g_configPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, utf8, (DWORD)utf8Len, &written, NULL);
    CloseHandle(h);
}

/* Is `path` already in the mod list? */
static BOOL mod_known(const wchar_t *path)
{
    for (int i = 0; i < g_modCount; i++)
        if (_wcsicmp(g_mods[i].path, path) == 0)
            return TRUE;
    return FALSE;
}

/* Auto-discovery: make sure the Mods folder next to this ASI exists, then add
   every subfolder that isn't already in the list, same as MZZXLC scanning its
   "mods" folder. */

/* If `start` (or a folder below it) directly contains a "nativePCx64"
   directory, copies the folder that contains it into `out` and returns TRUE.
   So mod\sub\thing\nativePCx64 resolves the mod root to mod\sub\thing. */
static BOOL locate_nativepc_root(const wchar_t *start, wchar_t *out, size_t cap, int depth)
{
    if (start == NULL || start[0] == L'\0' || depth > 8)
        return FALSE;

    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*", start);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    BOOL rootHere = FALSE;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || fd.cFileName[0] == L'.')
            continue;
        if (_wcsicmp(fd.cFileName, L"nativePCx64") == 0)
        {
            rootHere = TRUE;
            break;
        }
    } while (FindNextFileW(h, &fd) != 0);
    FindClose(h);

    if (rootHere)
    {
        wcsncpy(out, start, cap - 1);
        out[cap - 1] = L'\0';
        return TRUE;
    }

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    BOOL found = FALSE;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || fd.cFileName[0] == L'.')
            continue;
        wchar_t sub[MAX_PATH_LEN];
        _snwprintf(sub, MAX_PATH_LEN, L"%ls\\%ls", start, fd.cFileName);
        if (locate_nativepc_root(sub, out, cap, depth + 1))
        {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(h, &fd) != 0);
    FindClose(h);
    return found;
}

/* Non-content files a publisher may ship alongside the real mod file. */
static BOOL is_skip_ext(const wchar_t *name)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (dot == NULL || dot[1] == L'\0')
        return FALSE;
    static const wchar_t *skip[] = {
        L"jpg", L"jpeg", L"png", L"bmp", L"gif", L"ico", L"tga",
        L"txt", L"md", L"pdf", L"htm", L"html", L"log", L"ini",
        L"dll", L"exe", L"asi"
    };
    for (int i = 0; i < (int)(sizeof(skip) / sizeof(skip[0])); i++)
        if (_wcsicmp(dot + 1, skip[i]) == 0)
            return TRUE;
    return FALSE;
}

static BOOL filemod_known(const wchar_t *file)
{
    for (int i = 0; i < g_fileModCount; i++)
        if (_wcsicmp(g_fileMods[i].file, file) == 0)
            return TRUE;
    return FALSE;
}

/* One-time walk of `dir` (the game's nativePCx64) collecting every file's
   lowercased base name and its path relative to g_baseDir. */
static void build_game_index_dir(const wchar_t *dir, int depth)
{
    if (depth > 12 || g_gameIndexCount >= MAX_GAME_FILES)
        return;

    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (fd.cFileName[0] == L'.')
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            wchar_t sub[MAX_PATH_LEN];
            _snwprintf(sub, MAX_PATH_LEN, L"%ls\\%ls", dir, fd.cFileName);
            build_game_index_dir(sub, depth + 1);
            continue;
        }
        if (g_gameIndexCount >= MAX_GAME_FILES)
            break;
        _snwprintf(g_gameIndexName[g_gameIndexCount], GAME_NAME_LEN, L"%ls", fd.cFileName);
        _wcslwr(g_gameIndexName[g_gameIndexCount]);
        const wchar_t *rel = dir + wcslen(g_baseDir);
        while (*rel == L'\\')
            rel++;
        _snwprintf(g_gameIndexRel[g_gameIndexCount], GAME_REL_LEN, L"%ls\\%ls", rel, fd.cFileName);
        g_gameIndexCount++;
    } while (FindNextFileW(h, &fd) != 0);
    FindClose(h);
}

static BOOL ensure_game_index(void)
{
    if (g_gameIndexBuilt)
        return g_gameIndexCount > 0;
    g_gameIndexBuilt = TRUE;
    wchar_t gameNative[MAX_PATH_LEN];
    _snwprintf(gameNative, MAX_PATH_LEN, L"%ls\\nativePCx64", g_baseDir);
    if (GetFileAttributesW(gameNative) == INVALID_FILE_ATTRIBUTES)
        return FALSE;
    build_game_index_dir(gameNative, 0);
    return g_gameIndexCount > 0;
}

/* Look up `name` in the cached index; copies the game-relative path into
   `outRel` if found. */
static BOOL game_index_find(const wchar_t *name, wchar_t *outRel, size_t cap)
{
    if (!ensure_game_index())
        return FALSE;
    wchar_t low[GAME_NAME_LEN];
    _snwprintf(low, GAME_NAME_LEN, L"%ls", name);
    _wcslwr(low);
    for (int i = 0; i < g_gameIndexCount; i++)
        if (wcscmp(g_gameIndexName[i], low) == 0)
        {
            _snwprintf(outRel, cap, L"%ls", g_gameIndexRel[i]);
            return TRUE;
        }
    return FALSE;
}

/* Cross-reference every loose file under `dir` (a mod with no nativePCx64
   folder) against the game's nativePCx64, which is next to umvc3.exe. Files
   that already have a saved mapping are left alone so we only scan once. */
static void resolve_loose_files(const wchar_t *dir)
{
    wchar_t gameNative[MAX_PATH_LEN];
    _snwprintf(gameNative, MAX_PATH_LEN, L"%ls\\nativePCx64", g_baseDir);
    if (GetFileAttributesW(gameNative) == INVALID_FILE_ATTRIBUTES)
    {
        mgr_log(L"resolve: no game nativePCx64 at %ls", gameNative);
        return;
    }

    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        mgr_log(L"resolve: FindFirstFileW failed for %ls err=%lu", dir, GetLastError());
        return;
    }
    do
    {
        if (fd.cFileName[0] == L'.')
            continue;
        wchar_t full[MAX_PATH_LEN];
        _snwprintf(full, MAX_PATH_LEN, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            resolve_loose_files(full);
            continue;
        }
        if (is_skip_ext(fd.cFileName) || filemod_known(full))
            continue;
        wchar_t vrel[MAX_PATH_LEN];
        if (game_index_find(fd.cFileName, vrel, MAX_PATH_LEN) &&
            g_fileModCount < MAX_MODS)
        {
            wcscpy(g_fileMods[g_fileModCount].file, full);
            wcscpy(g_fileMods[g_fileModCount].virtual, vrel);
            g_fileModCount++;
            mgr_log(L"resolve: %ls -> %ls", full, vrel);
        }
    } while (FindNextFileW(h, &fd) != 0);
    FindClose(h);
}

static void scan_mods_folder(void)
{
    wchar_t root[MAX_PATH_LEN];
    _snwprintf(root, MAX_PATH_LEN, L"%ls\\%ls", g_baseDir, MODS_DIR);
    CreateDirectoryW(root, NULL);
    mgr_log(L"scan: root=%ls", root);

    /* Pass 1: re-root legacy entries that point above their nativePCx64 parent. */
    for (int i = 0; i < g_modCount; i++)
    {
        wchar_t r[MAX_PATH_LEN];
        if (locate_nativepc_root(g_mods[i].path, r, MAX_PATH_LEN, 0) &&
            _wcsicmp(r, g_mods[i].path) != 0)
        {
            BOOL dup = FALSE;
            for (int j = 0; j < g_modCount; j++)
                if (i != j && _wcsicmp(g_mods[j].path, r) == 0)
                {
                    dup = TRUE;
                    break;
                }
            if (!dup)
                wcscpy(g_mods[i].path, r);
            else
                g_mods[i].path[0] = L'\0';
        }
    }

    /* Drop entries marked empty above (or duplicated by re-rooting). */
    int n = 0;
    for (int i = 0; i < g_modCount; i++)
    {
        if (g_mods[i].path[0] == L'\0')
            continue;
        BOOL dup = FALSE;
        for (int j = 0; j < n; j++)
            if (_wcsicmp(g_mods[j].path, g_mods[i].path) == 0)
            {
                dup = TRUE;
                break;
            }
        if (!dup)
            g_mods[n++] = g_mods[i];
    }
    g_modCount = n;

    /* Pass 2: add every top-level folder (rooted) that isn't listed yet. */
    wchar_t pattern[MAX_PATH_LEN];
    _snwprintf(pattern, MAX_PATH_LEN, L"%ls\\*", root);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        mgr_log(L"scan: FindFirstFileW(%ls) failed err=%lu", pattern, GetLastError());
        return;
    }
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            continue;
        if (fd.cFileName[0] == L'.')
            continue;
        wchar_t mod[MAX_PATH_LEN];
        _snwprintf(mod, MAX_PATH_LEN, L"%ls\\%ls", root, fd.cFileName);
        wchar_t r[MAX_PATH_LEN];
        BOOL hasNative = locate_nativepc_root(mod, r, MAX_PATH_LEN, 0);
        if (hasNative)
            wcscpy(mod, r);
        BOOL known = mod_known(mod);
        if (!known && g_modCount < MAX_MODS)
        {
            wcscpy(g_mods[g_modCount].path, mod);
            g_mods[g_modCount].enabled = TRUE;
            g_modCount++;
        }
        mgr_log(L"scan: folder=%ls native=%d known=%d -> mods=%d", fd.cFileName,
                hasNative ? 1 : 0, known ? 1 : 0, g_modCount);
        /* A mod with no nativePCx64 folder ships loose files: cross-reference
           them against the game's nativePCx64 and save the mapping. */
        if (!hasNative)
            resolve_loose_files(mod);
    } while (FindNextFileW(hFind, &fd) != 0);
    FindClose(hFind);
}

static void update_status(void)
{
    if (g_status == NULL)
        return;
    int n = 0;
    for (int i = 0; i < g_modCount; i++)
        if (ListView_GetCheckState(g_list, i))
            n++;
    wchar_t buf[128];
    _snwprintf(buf, 128, L"%d of %d mods enabled", n, g_modCount);
    SetWindowTextW(g_status, buf);
}

static void rebuild_list(void)
{
    if (g_list == NULL)
        return;
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_modCount; i++)
    {
        const wchar_t *slash = wcsrchr(g_mods[i].path, L'\\');
        const wchar_t *name = slash != NULL ? slash + 1 : g_mods[i].path;

        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = (LPWSTR)name;
        item.lParam = i;
        ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, i, 1, g_mods[i].path);
        ListView_SetCheckState(g_list, i, g_mods[i].enabled ? TRUE : FALSE);
    }
    update_status();
    mgr_log(L"rebuild: showing %d items", g_modCount);
}

static void on_refresh(void)
{
    scan_mods_folder();
    rebuild_list();
}

static void on_master(void)
{
    BOOL on = (SendMessageW(g_master, BM_GETCHECK, 0, 0) == BST_CHECKED);
    for (int i = 0; i < g_modCount; i++)
    {
        g_mods[i].enabled = on;
        ListView_SetCheckState(g_list, i, on ? TRUE : FALSE);
    }
    update_status();
}

/* Move the selected mod up (-1) or down (+1) in the list. The order IS the
   priority: mods.ini is written in list order and the loader gives the first
   matching mod precedence, so the top of the list wins conflicts. */
static void on_move(int delta)
{
    if (g_modCount < 2)
        return;
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0)
        return;
    int dst = sel + delta;
    if (dst < 0 || dst >= g_modCount)
        return;

    ModEntry tmp = g_mods[sel];
    g_mods[sel] = g_mods[dst];
    g_mods[dst] = tmp;

    rebuild_list();
    ListView_SetItemState(g_list, dst, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, dst, FALSE);
    SetFocus(g_list);
}

static void layout_controls(HWND hwnd)
{
    if (g_list == NULL)
        return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int margin = 8;
    int y = margin;

    MoveWindow(g_master, margin, y, 300, 20, TRUE);
    if (g_hint != NULL)
        MoveWindow(g_hint, margin + 306, y, w - 2 * margin - 306, 20, TRUE);
    y += 24;
    MoveWindow(g_showNext, margin, y, 220, 20, TRUE);
    y += 24;

    int btnW = 90, btnH = 26;
    int listBottom = h - margin - btnH - 10;
    if (listBottom <= y)
        listBottom = y + 100;
    MoveWindow(g_list, margin, y, w - 2 * margin, listBottom - y, TRUE);
    y = listBottom + 6;

    MoveWindow(GetDlgItem(hwnd, IDC_REFRESH), margin, y, btnW, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_UP), margin + btnW + 6, y, btnW, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_DOWN), margin + 2 * (btnW + 6), y, btnW, btnH, TRUE);

    int x = w - margin - 2 * btnW - 6;
    MoveWindow(GetDlgItem(hwnd, IDC_EXIT), x, y, btnW, btnH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_LAUNCH), x + btnW + 6, y, btnW, btnH, TRUE);

    int statusX = margin + 3 * (btnW + 6);
    int statusW = x - statusX - 6;
    if (statusW < 60)
        statusW = 60;
    MoveWindow(g_status, statusX, y, statusW, btnH, TRUE);
}

static LRESULT CALLBACK manager_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_REFRESH: on_refresh(); break;
        case IDC_UP: on_move(-1); break;
        case IDC_DOWN: on_move(1); break;
        case IDC_MASTER:
            if (HIWORD(wParam) == BN_CLICKED)
                on_master();
            break;
        case IDC_EXIT:
        case IDC_LAUNCH:
            g_showNextTime = (SendMessageW(g_showNext, BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_masterEnabled = (SendMessageW(g_master, BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_threadResult = (LOWORD(wParam) == IDC_LAUNCH);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        break;
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lParam;
        if (nm->hwndFrom == g_list && nm->code == LVN_ITEMCHANGED)
            update_status();
        break;
    }
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 560;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_SIZE:
        layout_controls(hwnd);
        break;
    case WM_CLOSE:
        mgr_log(L"close: saving mods=%d filemods=%d launch=%d", g_modCount, g_fileModCount,
                g_threadResult ? 1 : 0);
        write_mods_ini();
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void create_window(HINSTANCE hInst)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = manager_wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"UMVC3ModManagerWnd";
    RegisterClassW(&wc);

    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);
    int ww = 680, wh = 500;
    int x = (cx - ww) / 2;
    int y = (cy - wh) / 3;

    g_hwnd = CreateWindowExW(0, L"UMVC3ModManagerWnd", L"UMVC3 Mod Manager",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                             x, y, ww, wh, NULL, NULL, hInst, NULL);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_master = CreateWindowW(L"BUTTON", L"Enable all mods (master switch)",
                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_MASTER, hInst, NULL);
    SendMessageW(g_master, BM_SETCHECK, g_masterEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_master, WM_SETFONT, (WPARAM)font, TRUE);

    g_showNext = CreateWindowW(L"BUTTON", L"Show this window on launch",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_SHOWNEXT, hInst, NULL);
    SendMessageW(g_showNext, BM_SETCHECK, g_showNextTime ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_showNext, WM_SETFONT, (WPARAM)font, TRUE);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                             LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_LIST, hInst, NULL);
    ListView_SetExtendedListViewStyle(g_list,
                                      LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                      LVS_EX_DOUBLEBUFFER);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)font, TRUE);

    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = L"Mod";
    col.cx = 300;
    col.iSubItem = 0;
    ListView_InsertColumn(g_list, 0, &col);
    col.pszText = L"Path";
    col.cx = 340;
    col.iSubItem = 1;
    ListView_InsertColumn(g_list, 1, &col);

    static const wchar_t *names[] = { L"Refresh", L"Move Up", L"Move Down", L"Exit", L"Launch Game" };
    static const int ids[] = { IDC_REFRESH, IDC_UP, IDC_DOWN, IDC_EXIT, IDC_LAUNCH };
    for (int i = 0; i < 5; i++)
    {
        HWND btn = CreateWindowW(L"BUTTON", names[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)ids[i], hInst, NULL);
        SendMessageW(btn, WM_SETFONT, (WPARAM)font, TRUE);
    }

    g_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUS, hInst, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)font, TRUE);

    g_hint = CreateWindowW(L"STATIC", L"Priority: higher in the list wins",
                           WS_CHILD | WS_VISIBLE,
                           0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_HINT, hInst, NULL);
    SendMessageW(g_hint, WM_SETFONT, (WPARAM)font, TRUE);

    layout_controls(g_hwnd);
    rebuild_list();
    SetForegroundWindow(g_hwnd);
    SetFocus(g_list);
}

__declspec(dllexport) int WINAPI UMVC3_ModManager_Show(const wchar_t *gameDir)
{
    if (gameDir == NULL || gameDir[0] == L'\0')
        return FALSE;

    /* The Mods folder and mods.ini live next to this ASI. */
    if (g_baseDir[0] == L'\0')
        wcscpy(g_baseDir, gameDir);
    _snwprintf(g_configPath, MAX_PATH_LEN, L"%ls\\%hs", g_baseDir, CONFIG_NAME);

    mgr_log(L"manager: show baseDir=%ls gameDir=%ls", g_baseDir, gameDir);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    g_threadResult = FALSE;
    read_mods_ini();
    mgr_log(L"manager: after read mods=%d filemods=%d", g_modCount, g_fileModCount);
    scan_mods_folder();
    mgr_log(L"manager: after scan mods=%d filemods=%d", g_modCount, g_fileModCount);

    create_window(GetModuleHandleW(NULL));

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return g_threadResult;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        /* Remember the folder this ASI sits in. */
        GetModuleFileNameW(hinst, g_baseDir, MAX_PATH_LEN);
        wchar_t *slash = wcsrchr(g_baseDir, L'\\');
        if (slash != NULL)
            *slash = L'\0';
    }
    return TRUE;
}

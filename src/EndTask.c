/*
 * Taskbar End Task
 *
 * Force-closes an app straight from the Windows taskbar, without opening Task Manager:
 *
 *   1) Right-click an app on the taskbar: an "End task" button appears on top of its menu.
 *   2) Or hover the app on the taskbar and press Ctrl+Shift+E.
 *
 * Plain C and the Win32 API. No DLL injection, no hooks, no administrator rights, no network.
 * It does not record keystrokes: it only polls the current state of Ctrl, Shift, Alt, Win, E and
 * Esc, and listens for mouse button presses while the cursor is on the taskbar.
 *
 * Build: see build.ps1 (zig cc).
 */
#define UNICODE
#define _UNICODE
#define COBJMACROS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <stdarg.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <oleauto.h>
#include <oleacc.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <taskschd.h>

#include "resource.h"

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

#define WIDE2(x)      L##x
#define WIDE(x)       WIDE2(x)

#define APP_NAME      L"Taskbar End Task"
#ifdef ENDTASK_TEST_BUILD                        /* a second identity, so tests never touch a real install */
#define APP_ID        L"TaskbarEndTaskTest"
#else
#define APP_ID        L"TaskbarEndTask"          /* registry, window class, install folder and log names */
#endif
#define APP_VERSION   WIDE(VER_STRING)
#define MAIN_CLASS    APP_ID L".Main"
#define POPUP_CLASS   APP_ID L".Popup"
#define MUTEX_NAME    L"Local\\" APP_ID L".Instance"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPROVED_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define LAYERS_KEY    L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers"
#define SETTINGS_KEY  L"Software\\" APP_ID
#define THEME_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define NATIVE_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\TaskbarDeveloperSettings"
#define NATIVE_VALUE  L"TaskbarEndTask"

#define HOTKEY_VK     'E'                        /* together with Ctrl+Shift */
#define HOTKEY_TEXT   L"Ctrl+Shift+E"

#define WM_TRAY       (WM_APP + 1)
#define TIMER_POLL    1
#define POLL_FAST_MS  30     /* cursor is on the taskbar, or the button is pending/visible */
#define POLL_IDLE_MS  120    /* everything else */
#define MENU_WAIT_MS  800    /* how long to wait for the right-click menu to appear */
#define MENU_GRACE_MS 400    /* a menu that vanished may be the previous one closing: keep looking this long */
#define RAW_DEDUPE_MS 400    /* a polled click this soon after an event-reported one is the same click */
#define RETRY_MS      150    /* the shell sometimes cannot name a button right away (Start menu closing): */
#define MAX_RETRIES   4      /* ...look it up again this often before giving up */
#define POPUP_LIFE_MS 10000  /* the button hides itself after this long */
#define POPUP_HEIGHT  34     /* at 96 DPI */

#define ID_STARTUP    100
#define ID_HOTKEY     101
#define ID_NATIVE     102
#define ID_HELP       103
#define ID_EXIT       104

#define MAX_WINDOWS   256
#define MAX_PROCS     128
#define MAX_TARGETS   64
#define MAX_TREE      512

/* How well a window matches the taskbar button under the cursor. */
#define SCORE_ALIAS       85
#define SCORE_DESC        80
#define SCORE_DESC_PREFIX 75
#define SCORE_TITLE       70
#define SCORE_PRODUCT     65
#define SCORE_TITLE_END   60
#define SCORE_TITLE_WORD  50
#define SCORE_EXE_NAME    40

enum { EDGE_BOTTOM, EDGE_TOP, EDGE_LEFT, EDGE_RIGHT };

/* ------------------------------------------------------------------ state */

typedef struct {
    DWORD    pid;
    FILETIME created;        /* a PID alone can be reused; PID + creation time cannot */
} ProcRef;

typedef struct {
    WCHAR   label[256];      /* what the taskbar button is called */
    WCHAR   exe[140];        /* "notepad.exe" */
    ProcRef procs[MAX_TARGETS];
    int     count;
} Target;

typedef struct {
    DWORD    pid;
    BOOL     usable;         /* could be queried and is not a protected process */
    FILETIME created;
    WCHAR    exe[128];       /* file name without ".exe" */
    WCHAR    desc[128];      /* version info: FileDescription */
    WCHAR    product[128];   /* version info: ProductName */
} ProcInfo;

typedef struct {
    int   proc;              /* index into g_procs */
    int   score;
    WCHAR title[260];
} WinInfo;

typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
typedef BOOL    (WINAPI *IsProcessCriticalFn)(HANDLE, PBOOL);
typedef BOOL    (WINAPI *SetDpiContextFn)(HANDLE);

static HINSTANCE g_inst;
static HWND      g_main, g_popup;
static NOTIFYICONDATAW g_nid;
static UINT      g_msgTaskbarCreated;
static UINT      g_pollMs;
static BOOL      g_trayAdded;

static GetDpiForMonitorFn  g_getDpiForMonitor;
static IsProcessCriticalFn g_isProcessCritical;

/* options */
static BOOL      g_hotkey = TRUE;
static BOOL      g_dryRun;           /* --dry-run: do everything except TerminateProcess */
static BOOL      g_log;              /* --log: write a diagnostic log */
static WCHAR     g_logPath[MAX_PATH];

/* Windows version */
static BOOL      g_isWin10;
static BOOL      g_nativeCapable;    /* Windows 11 22631.2715+: has its own "End task" item */
static BOOL      g_useNative;        /* ...and it is switched on, so our button stays out of the way */
static ULONGLONG g_nativeCheckAt;

/* scratch space for one target lookup */
static ProcInfo  g_procs[MAX_PROCS];
static int       g_nprocs;
static WinInfo   g_wins[MAX_WINDOWS];
static int       g_nwins;

/* the "End task" button */
static HFONT     g_fontText, g_fontIcon;
static int       g_fontDpi;
static WCHAR     g_glyph[2] = { 0xE711, 0 };     /* Segoe MDL2 Assets: Cancel */
static int       g_popupDpi = 96;
static BOOL      g_popupLight, g_popupHover, g_popupPressed;
static WCHAR     g_popupLabel[320], g_popupDetail[160];
static Target    g_popupTarget;
static HWND      g_menuWnd;          /* the right-click menu the button is attached to */
static RECT      g_menuRect;
static ULONGLONG g_shownAt, g_hideAt;

/* A right-click on a taskbar button starts a "session": the target is known and the button
 * follows that button's menu for as long as it is open. */
static BOOL      g_sess, g_sessResolved, g_sessReleased, g_sessAttached;
static Target    g_sessTarget;
static POINT     g_sessPt;
static RECT      g_sessTask;
static int       g_sessEdge;
static ULONGLONG g_sessDeadline;     /* until then, keep waiting for the menu to (re)appear */
static ULONGLONG g_sessRetryAt;      /* the target lookup failed: when to try again */
static int       g_sessRetries;

static BOOL      g_rawMouse;         /* mouse button events are being delivered (Raw Input) */
static ULONGLONG g_rawCtxPressAt;
static HWND      g_logMenu, g_logRejected[4];   /* so each window is logged once per right-click */
static BOOL      g_prevHot, g_prevCtx;

/* Never offered as a target and never killed as part of a process tree. */
static const WCHAR *PROTECTED[] = {
    L"explorer", L"dwm", L"csrss", L"winlogon", L"wininit", L"services", L"lsass", L"lsaiso",
    L"smss", L"system", L"registry", L"ApplicationFrameHost", L"ShellExperienceHost",
    L"StartMenuExperienceHost", L"SearchHost", L"SearchApp", L"SearchUI", L"TextInputHost",
    L"LogonUI", L"LockApp", L"sihost", L"ctfmon", L"fontdrvhost", L"svchost", L"RuntimeBroker",
    L"taskhostw", NULL
};

/* Apps whose taskbar label matches neither their window title nor their version info. */
static const struct { const WCHAR *label, *exe; } ALIASES[] = {
    { L"Command Prompt",        L"cmd" },
    { L"Komut \u0130stemi",     L"cmd" },          /* Turkish */
    { L"Eingabeaufforderung",   L"cmd" },          /* German */
    { L"Windows PowerShell",    L"powershell" },
};

/* ---------------------------------------------------------------- logging */

/* Appends one line to the log (only with --log / --dry-run). Output is capped at 1024 chars. */
static void Log(const WCHAR *fmt, ...)
{
    WCHAR line[1100];
    char utf8[3400];
    SYSTEMTIME st;
    va_list ap;
    DWORD written;
    HANDLE f;
    int n;

    if (!g_log) return;
    GetLocalTime(&st);
    n = wsprintfW(line, L"%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    n += wvsprintfW(line + n, fmt, ap);
    va_end(ap);
    line[n++] = L'\r';
    line[n++] = L'\n';
    n = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), NULL, NULL);
    f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, utf8, (DWORD)n, &written, NULL);
    CloseHandle(f);
}

/* ---------------------------------------------------------------- helpers */

static BOOL IsProtectedName(const WCHAR *exe)
{
    for (int i = 0; PROTECTED[i]; i++)
        if (lstrcmpiW(exe, PROTECTED[i]) == 0) return TRUE;
    return FALSE;
}

/* "C:\x\Notepad.exe" -> "Notepad" */
static void BaseNameNoExt(const WCHAR *path, WCHAR *out, int cch)
{
    const WCHAR *p = path, *base = path;
    int n;
    for (; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
    lstrcpynW(out, base, cch);
    n = lstrlenW(out);
    if (n > 4 && lstrcmpiW(out + n - 4, L".exe") == 0) out[n - 4] = 0;
}

static void Trim(WCHAR *s)
{
    int n = lstrlenW(s), k = 0;
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
    while (s[k] == L' ' || s[k] == L'\t') k++;
    if (k) MoveMemory(s, s + k, (n - k + 1) * sizeof(WCHAR));
}

static BOOL IsDash(WCHAR c) { return c == L'-' || c == 0x2013 || c == 0x2014; }

static BOOL SameTextI(const WCHAR *a, const WCHAR *b, int n)
{
    return CompareStringOrdinal(a, n, b, n, TRUE) == CSTR_EQUAL;
}

/* TRUE if `word` occurs in `text` as a whole word ("Mail" is in "Inbox - Mail", not in "Gmail"). */
static BOOL ContainsWordI(const WCHAR *text, const WCHAR *word, BOOL atEndOnly)
{
    int nt = lstrlenW(text), nw = lstrlenW(word);
    if (nw == 0 || nt < nw) return FALSE;
    for (int i = atEndOnly ? nt - nw : 0; i + nw <= nt; i++) {
        if (!SameTextI(text + i, word, nw)) continue;
        if (i > 0 && IsCharAlphaNumericW(text[i - 1])) continue;
        if (text[i + nw] && IsCharAlphaNumericW(text[i + nw])) continue;
        return TRUE;
    }
    return FALSE;
}

/* TRUE if `raw` is `label` alone or `label` followed by " - ..." ("Notepad - 1 running window"). */
static BOOL IsLabelPrefix(const WCHAR *raw, const WCHAR *label)
{
    int nr = lstrlenW(raw), nl = lstrlenW(label);
    if (nl == 0 || nr < nl || !SameTextI(raw, label, nl)) return FALSE;
    if (nr == nl) return TRUE;
    return raw[nl] == L' ' && IsDash(raw[nl + 1]) && raw[nl + 2] == L' ';
}

static void ClassOf(HWND h, WCHAR *out, int cch)
{
    out[0] = 0;
    if (h) GetClassNameW(h, out, cch);
}

/* Visible and not cloaked (hidden UWP windows and other virtual desktops are cloaked). */
static BOOL IsShown(HWND h)
{
    int cloaked = 0;
    if (!h || !IsWindowVisible(h)) return FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked == 0;
}

static BOOL KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* bit 0 = down right now, bit 1 = pressed since the last poll (so short clicks are not missed) */
static int KeyState(int vk)
{
    SHORT s = GetAsyncKeyState(vk);
    return ((s & 0x8000) ? 1 : 0) | ((s & 1) ? 2 : 0);
}

static int DpiAt(POINT pt)
{
    HDC dc;
    int dpi;
    if (g_getDpiForMonitor) {
        UINT x = 96, y = 96;
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (SUCCEEDED(g_getDpiForMonitor(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) return (int)x;
    }
    dc = GetDC(NULL);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return dpi ? dpi : 96;
}

static BOOL IsCritical(HANDLE process)
{
    BOOL critical = FALSE;
    return g_isProcessCritical && g_isProcessCritical(process, &critical) && critical;
}

/* File name (without ".exe") of the process that owns a window. */
static BOOL ExeOfWindow(HWND h, WCHAR *out, int cch)
{
    WCHAR path[1024];
    DWORD pid = 0, len = ARRAYSIZE(path);
    HANDLE hp;
    BOOL ok;

    out[0] = 0;
    GetWindowThreadProcessId(h, &pid);
    hp = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
    if (!hp) return FALSE;
    ok = QueryFullProcessImageNameW(hp, 0, path, &len);
    CloseHandle(hp);
    if (ok) BaseNameNoExt(path, out, cch);
    return ok;
}

/* --------------------------------------------------------------- settings */

static DWORD GetSetting(const WCHAR *name, DWORD fallback)
{
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS)
        return v;
    return fallback;
}

static void SetSetting(const WCHAR *name, DWORD v)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_DWORD, &v, sizeof(v));
}

static BOOL GetStartupCommand(WCHAR *out, DWORD cch)
{
    DWORD size = cch * sizeof(WCHAR);
    out[0] = 0;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_ID, RRF_RT_REG_SZ, NULL, out, &size) == ERROR_SUCCESS;
}

/* ------------------------------------------------- starting as administrator */

static BOOL IsElevated(void)
{
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    HANDLE token;
    BOOL elevated = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
        elevated = elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

/* Is this exe marked "Run this program as an administrator" (Properties > Compatibility)? */
static BOOL ForcedToRunAsAdmin(void)
{
    const HKEY hives[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    WCHAR self[MAX_PATH], flags[256];

    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return FALSE;
    for (int i = 0; i < (int)ARRAYSIZE(hives); i++) {
        DWORD size = sizeof(flags);
        if (RegGetValueW(hives[i], LAYERS_KEY, self, RRF_RT_REG_SZ, NULL, flags, &size) == ERROR_SUCCESS &&
            StrStrIW(flags, L"RUNASADMIN"))
            return TRUE;
    }
    return FALSE;
}

/*
 * Windows never starts a program that needs administrator rights from the normal startup list
 * (the Run key): it is skipped silently at sign-in. The supported way is a scheduled task that
 * runs "with highest privileges" when the user signs in. Creating one needs administrator
 * rights, so this is only used by a copy that is itself running as administrator.
 */
static const GUID CLSID_TaskScheduler_ = { 0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };
static const GUID IID_ITaskService_    = { 0x2faba4c7, 0x4da9, 0x4013, { 0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85 } };

static ITaskFolder *OpenTaskRoot(void)
{
    ITaskService *service = NULL;
    ITaskFolder *root = NULL;
    VARIANT none;
    BSTR path;

    VariantInit(&none);
    if (FAILED(CoCreateInstance(&CLSID_TaskScheduler_, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService_, (void **)&service)) || !service)
        return NULL;
    if (SUCCEEDED(ITaskService_Connect(service, none, none, none, none))) {
        path = SysAllocString(L"\\");
        if (path) {
            if (FAILED(ITaskService_GetFolder(service, path, &root))) root = NULL;
            SysFreeString(path);
        }
    }
    ITaskService_Release(service);
    return root;
}

static BOOL StartupTaskExists(void)
{
    ITaskFolder *root = OpenTaskRoot();
    IRegisteredTask *task = NULL;
    BSTR name;
    BOOL exists = FALSE;

    if (!root) return FALSE;
    name = SysAllocString(APP_ID);
    if (name) {
        exists = SUCCEEDED(ITaskFolder_GetTask(root, name, &task)) && task;
        if (task) IRegisteredTask_Release(task);
        SysFreeString(name);
    }
    ITaskFolder_Release(root);
    return exists;
}

static BOOL DeleteStartupTask(void)
{
    ITaskFolder *root = OpenTaskRoot();
    BSTR name;
    HRESULT hr = E_FAIL;

    if (!root) return FALSE;
    name = SysAllocString(APP_ID);
    if (name) {
        hr = ITaskFolder_DeleteTask(root, name, 0);
        SysFreeString(name);
    }
    ITaskFolder_Release(root);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

static void XmlAppend(WCHAR *dst, int cch, const WCHAR *src, BOOL escape)
{
    int n = lstrlenW(dst);
    for (; *src && n < cch - 8; src++) {
        const WCHAR *entity = NULL;
        if (escape) {
            if (*src == L'&') entity = L"&amp;";
            else if (*src == L'<') entity = L"&lt;";
            else if (*src == L'>') entity = L"&gt;";
            else if (*src == L'"') entity = L"&quot;";
        }
        if (entity) {
            lstrcpyW(dst + n, entity);
            n += lstrlenW(entity);
        } else {
            dst[n++] = *src;
        }
    }
    dst[n] = 0;
}

/* Registers a task that starts `exe` when this user signs in; `highest` = as administrator. */
static BOOL CreateStartupTask(const WCHAR *exe, BOOL highest)
{
    static WCHAR xml[4096];
    WCHAR user[300], domain[128], name[128];
    ITaskFolder *root;
    IRegisteredTask *task = NULL;
    VARIANT none;
    BSTR bname, bxml;
    HRESULT hr = E_FAIL;

    if (!GetEnvironmentVariableW(L"USERNAME", name, ARRAYSIZE(name))) return FALSE;
    if (GetEnvironmentVariableW(L"USERDOMAIN", domain, ARRAYSIZE(domain))) wsprintfW(user, L"%s\\%s", domain, name);
    else lstrcpyW(user, name);

    xml[0] = 0;
    XmlAppend(xml, ARRAYSIZE(xml),
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Starts " APP_NAME L" when you sign in. Created by its \"Start with Windows\" option.</Description></RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>", FALSE);
    XmlAppend(xml, ARRAYSIZE(xml), user, TRUE);
    XmlAppend(xml, ARRAYSIZE(xml), L"</UserId></LogonTrigger></Triggers><Principals><Principal id=\"Author\"><UserId>", FALSE);
    XmlAppend(xml, ARRAYSIZE(xml), user, TRUE);
    XmlAppend(xml, ARRAYSIZE(xml), L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>", FALSE);
    XmlAppend(xml, ARRAYSIZE(xml), highest ? L"HighestAvailable" : L"LeastPrivilege", FALSE);
    /* The defaults would refuse to start on battery power and would stop the program after three days. */
    XmlAppend(xml, ARRAYSIZE(xml),
        L"</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>", FALSE);
    XmlAppend(xml, ARRAYSIZE(xml), exe, TRUE);
    XmlAppend(xml, ARRAYSIZE(xml), L"</Command></Exec></Actions></Task>", FALSE);

    root = OpenTaskRoot();
    if (!root) return FALSE;
    VariantInit(&none);
    bname = SysAllocString(APP_ID);
    bxml = SysAllocString(xml);
    if (bname && bxml)
        hr = ITaskFolder_RegisterTask(root, bname, bxml, TASK_CREATE_OR_UPDATE, none, none,
                                      TASK_LOGON_INTERACTIVE_TOKEN, none, &task);
    if (task) IRegisteredTask_Release(task);
    if (bname) SysFreeString(bname);
    if (bxml) SysFreeString(bxml);
    ITaskFolder_Release(root);
    Log(L"startup task for %s: result 0x%08lX", exe, (unsigned long)hr);
    return SUCCEEDED(hr);
}

/* Task Manager's Startup tab (and tune-up tools) can switch a startup entry off without removing it. */
static BOOL StartupDisabledByWindows(void)
{
    BYTE state[16];
    DWORD size = sizeof(state);
    return RegGetValueW(HKEY_CURRENT_USER, APPROVED_KEY, APP_ID, RRF_RT_REG_BINARY, NULL, state, &size) == ERROR_SUCCESS
           && size >= 1 && (state[0] & 1);
}

static BOOL RunValueEnabled(void)
{
    WCHAR cmd[MAX_PATH + 8];
    return GetStartupCommand(cmd, ARRAYSIZE(cmd)) && !StartupDisabledByWindows();
}

/* Makes Windows start `exe` at sign-in. */
static void SetStartupTo(const WCHAR *exe)
{
    WCHAR cmd[MAX_PATH + 8];
    wsprintfW(cmd, L"\"%s\"", exe);
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_ID, REG_SZ, cmd, (lstrlenW(cmd) + 1) * sizeof(WCHAR));
    RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, APP_ID);    /* the user asked for it: switch it back on */
}

static void SetStartup(BOOL on)
{
    if (on) {
        WCHAR self[MAX_PATH];
        if (GetModuleFileNameW(NULL, self, MAX_PATH)) SetStartupTo(self);
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_ID);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, APPROVED_KEY, APP_ID);
    }
}

/* If "Start with Windows" points at a file that no longer exists (the exe was moved), point it here. */
static void RefreshStartupPath(void)
{
    WCHAR current[MAX_PATH + 8];
    if (!GetStartupCommand(current, ARRAYSIZE(current))) return;
    PathUnquoteSpacesW(current);
    if (GetFileAttributesW(current) == INVALID_FILE_ATTRIBUTES) SetStartup(TRUE);
}

static BOOL StartupEnabled(void)
{
    return RunValueEnabled() || StartupTaskExists();
}

/* Makes `exe` start at sign-in, the same way this copy is running: as administrator or not. */
static BOOL EnableStartup(const WCHAR *exe)
{
    if (!IsElevated()) {
        SetStartupTo(exe);
        return TRUE;
    }
    if (!CreateStartupTask(exe, TRUE)) return FALSE;
    /* A Run entry as well would be skipped by Windows, or start a second, non-administrator copy. */
    SetStartup(FALSE);
    return TRUE;
}

/* FALSE if a task created as administrator is in the way and we are not one. */
static BOOL DisableStartup(void)
{
    SetStartup(FALSE);
    return !StartupTaskExists() || DeleteStartupTask();
}

static BOOL NativeEndTaskEnabled(void)
{
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, NATIVE_KEY, NATIVE_VALUE, RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS
           && v != 0;
}

static void SetNativeEndTask(BOOL on)
{
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, NATIVE_KEY, NATIVE_VALUE, REG_DWORD, &v, sizeof(v));
}

static BOOL SystemUsesLightTheme(void)
{
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, THEME_KEY, L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS
           && v != 0;
}

/* ------------------------------------------------------------ tray notice */

static void Notify(const WCHAR *title, const WCHAR *text, DWORD icon)
{
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = icon;
    lstrcpynW(n.szInfoTitle, title, ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, text, ARRAYSIZE(n.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

/* ------------------------------------------------------------ the taskbar */

/* Is the cursor on a taskbar? If so, also returns the taskbar's rectangle and whether the cursor
 * is on an app button (as opposed to Start, the clock or the notification area). */
static BOOL TaskbarAt(POINT pt, RECT *taskRect, BOOL *onButton)
{
    WCHAR cls[64];
    HWND h = WindowFromPoint(pt), root;

    if (!h) return FALSE;
    root = GetAncestor(h, GA_ROOT);
    if (!root) root = h;
    ClassOf(root, cls, 64);
    if (lstrcmpW(cls, L"Shell_TrayWnd") != 0 && lstrcmpW(cls, L"Shell_SecondaryTrayWnd") != 0)
        return FALSE;
    if (taskRect) GetWindowRect(root, taskRect);
    if (onButton) {
        /* Windows 10 has a dedicated window for the app buttons. Windows 11 draws the whole
         * taskbar in one XAML surface, so there the accessible name has to do. */
        ClassOf(h, cls, 64);
        *onButton = !g_isWin10 || lstrcmpW(cls, L"MSTaskListWClass") == 0;
    }
    return TRUE;
}

/* Which screen edge the taskbar is docked to. */
static int TaskbarEdge(const RECT *task)
{
    MONITORINFO mi;
    int w = task->right - task->left, h = task->bottom - task->top;

    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromRect(task, MONITOR_DEFAULTTONEAREST), &mi)) return EDGE_BOTTOM;
    if (w >= h)
        return (task->top + task->bottom) / 2 < (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2 ? EDGE_TOP : EDGE_BOTTOM;
    return (task->left + task->right) / 2 < (mi.rcMonitor.left + mi.rcMonitor.right) / 2 ? EDGE_LEFT : EDGE_RIGHT;
}

/* Accessible (MSAA) name of whatever is under the cursor: "Notepad - 1 running window". */
static BOOL AccNameAt(POINT pt, WCHAR *out, int cch)
{
    IAccessible *acc = NULL;
    VARIANT child;
    BSTR name = NULL;
    BOOL ok = FALSE;

    out[0] = 0;
    VariantInit(&child);
    if (AccessibleObjectFromPoint(pt, &acc, &child) != S_OK || !acc) return FALSE;
    if (IAccessible_get_accName(acc, child, &name) == S_OK && name) {
        lstrcpynW(out, name, cch);
        ok = out[0] != 0;
    }
    if (name) SysFreeString(name);
    VariantClear(&child);
    IAccessible_Release(acc);
    return ok;
}

/* {618736E0-3C3D-11CF-810C-00AA00389B71}; spelled out so no extra library is needed */
static const GUID IID_IAccessible_ = { 0x618736e0, 0x3c3d, 0x11cf, { 0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static BOOL CopyAccName(IAccessible *acc, VARIANT child, WCHAR *out, int cch)
{
    BSTR name = NULL;
    BOOL ok = FALSE;
    if (IAccessible_get_accName(acc, child, &name) == S_OK && name) {
        lstrcpynW(out, name, cch);
        ok = out[0] != 0;
    }
    if (name) SysFreeString(name);
    return ok;
}

/*
 * Name of the Windows 10 taskbar button at `pt`, asked from the button list itself.
 * AccNameAt() goes through the whole screen's hit-testing, and while the Start menu or Search
 * is open that answers with the taskbar as a whole ("Taskbar") instead of the button.
 */
static BOOL TaskListNameAt(POINT pt, WCHAR *out, int cch)
{
    WCHAR cls[64];
    HWND h = WindowFromPoint(pt);
    IAccessible *list = NULL;
    VARIANT child;
    BOOL ok = FALSE;
    long count = 0;

    out[0] = 0;
    ClassOf(h, cls, 64);
    if (lstrcmpW(cls, L"MSTaskListWClass") != 0) return FALSE;
    if (AccessibleObjectFromWindow(h, (DWORD)OBJID_CLIENT, &IID_IAccessible_, (void **)&list) != S_OK || !list)
        return FALSE;

    VariantInit(&child);
    if (IAccessible_accHitTest(list, pt.x, pt.y, &child) == S_OK) {
        if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
            ok = CopyAccName(list, child, out, cch);
        } else if (child.vt == VT_DISPATCH && child.pdispVal) {
            IAccessible *item = NULL;
            if (IDispatch_QueryInterface(child.pdispVal, &IID_IAccessible_, (void **)&item) == S_OK && item) {
                VARIANT self;
                self.vt = VT_I4;
                self.lVal = CHILDID_SELF;
                ok = CopyAccName(item, self, out, cch);
                IAccessible_Release(item);
            }
        }
    }
    VariantClear(&child);

    /* Hit-testing failed as well: compare the cursor with each button's rectangle. */
    if (!ok && IAccessible_get_accChildCount(list, &count) == S_OK) {
        for (long i = 1; i <= count && i <= 512 && !ok; i++) {
            long x = 0, y = 0, w = 0, ht = 0;
            VARIANT v;
            v.vt = VT_I4;
            v.lVal = i;
            if (IAccessible_accLocation(list, &x, &y, &w, &ht, v) == S_OK &&
                pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + ht)
                ok = CopyAccName(list, v, out, cch);
        }
    }
    IAccessible_Release(list);
    return ok;
}

/* "Chrome - 2 running windows" -> "Chrome". Returns TRUE if a window count was removed, which
 * tells us the button belongs to a running app rather than to a pinned shortcut. */
static BOOL StripCountSuffix(WCHAR *s)
{
    int cut = -1;
    for (int i = 0; s[i]; i++) {
        int j;
        if (s[i] != L' ' || !IsDash(s[i + 1]) || s[i + 2] != L' ') continue;
        j = i + 3;
        if (s[j] < L'0' || s[j] > L'9') continue;
        while (s[j] >= L'0' && s[j] <= L'9') j++;
        if (s[j] == L' ') cut = i;      /* keep the last match: titles can contain " - 2 " too */
    }
    if (cut >= 0) s[cut] = 0;
    Trim(s);
    return cut >= 0;
}

/* ----------------------------------------------------- finding the target */

static BOOL VersionString(const BYTE *info, WORD lang, WORD codepage, const WCHAR *key, WCHAR *out, int cch)
{
    WCHAR query[96];
    WCHAR *value = NULL;
    UINT len = 0;

    wsprintfW(query, L"\\StringFileInfo\\%04x%04x\\%s", lang, codepage, key);
    if (!VerQueryValueW(info, query, (LPVOID *)&value, &len) || !value || !len) return FALSE;
    lstrcpynW(out, value, cch);
    Trim(out);
    return out[0] != 0;
}

/* "File description" and "Product name" from an exe's version resource. */
static void ReadVersionInfo(const WCHAR *path, ProcInfo *p)
{
    static const struct { WORD lang, codepage; } FALLBACK[] = { { 0x0409, 1200 }, { 0x0409, 1252 }, { 0, 1200 } };
    struct { WORD lang, codepage; } *tr = NULL;
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path, &dummy);
    UINT len = 0;
    BYTE *info;

    if (!size) return;
    info = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!info) return;
    if (GetFileVersionInfoW(path, 0, size, info)) {
        if (VerQueryValueW(info, L"\\VarFileInfo\\Translation", (LPVOID *)&tr, &len) && tr && len >= 4) {
            VersionString(info, tr[0].lang, tr[0].codepage, L"FileDescription", p->desc, ARRAYSIZE(p->desc));
            VersionString(info, tr[0].lang, tr[0].codepage, L"ProductName", p->product, ARRAYSIZE(p->product));
        }
        for (int i = 0; i < (int)ARRAYSIZE(FALLBACK); i++) {
            if (!p->desc[0])
                VersionString(info, FALLBACK[i].lang, FALLBACK[i].codepage, L"FileDescription", p->desc, ARRAYSIZE(p->desc));
            if (!p->product[0])
                VersionString(info, FALLBACK[i].lang, FALLBACK[i].codepage, L"ProductName", p->product, ARRAYSIZE(p->product));
        }
    }
    HeapFree(GetProcessHeap(), 0, info);
}

/* Looks a process up once per target lookup. Returns its index in g_procs, or -1. */
static int ProcIndex(DWORD pid)
{
    WCHAR path[1024];
    DWORD len = ARRAYSIZE(path);
    FILETIME e, k, u;
    ProcInfo *p;
    HANDLE hp;

    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i].pid == pid) return i;
    if (g_nprocs >= MAX_PROCS) return -1;

    p = &g_procs[g_nprocs];
    ZeroMemory(p, sizeof(*p));
    p->pid = pid;
    hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) {
        if (QueryFullProcessImageNameW(hp, 0, path, &len) && GetProcessTimes(hp, &p->created, &e, &k, &u)) {
            BaseNameNoExt(path, p->exe, ARRAYSIZE(p->exe));
            if (p->exe[0] && !IsProtectedName(p->exe) && !IsCritical(hp)) {
                ReadVersionInfo(path, p);
                p->usable = TRUE;
            }
        }
        CloseHandle(hp);
    }
    return g_nprocs++;
}

/* Collects visible, titled top-level windows together with the process behind each. */
static BOOL CALLBACK CollectWindow(HWND h, LPARAM lp)
{
    WCHAR cls[64];
    DWORD pid = 0;
    WinInfo *w;
    int pi;
    (void)lp;

    if (g_nwins >= MAX_WINDOWS) return FALSE;
    if (!IsShown(h) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    w = &g_wins[g_nwins];
    if (!GetWindowTextW(h, w->title, ARRAYSIZE(w->title))) return TRUE;

    GetWindowThreadProcessId(h, &pid);
    ClassOf(h, cls, 64);
    /* Store (UWP) apps: the frame belongs to ApplicationFrameHost, the app owns the CoreWindow inside. */
    if (lstrcmpW(cls, L"ApplicationFrameWindow") == 0) {
        HWND core = FindWindowExW(h, NULL, L"Windows.UI.Core.CoreWindow", NULL);
        if (core) {
            DWORD cpid = 0;
            GetWindowThreadProcessId(core, &cpid);
            if (cpid) pid = cpid;
        }
    }
    if (!pid || pid == GetCurrentProcessId()) return TRUE;

    pi = ProcIndex(pid);
    if (pi < 0 || !g_procs[pi].usable) return TRUE;
    w->proc = pi;
    w->score = 0;
    g_nwins++;
    return TRUE;
}

/* `raw` is the button's full accessible name, `label` the same without the window count. */
static int ScoreWindow(const WinInfo *w, const ProcInfo *p, const WCHAR *raw, const WCHAR *label)
{
    for (int i = 0; i < (int)ARRAYSIZE(ALIASES); i++)
        if (lstrcmpiW(label, ALIASES[i].label) == 0 && lstrcmpiW(p->exe, ALIASES[i].exe) == 0)
            return SCORE_ALIAS;

    if (lstrlenW(p->desc) >= 3) {
        if (lstrcmpiW(p->desc, label) == 0) return SCORE_DESC;
        if (IsLabelPrefix(raw, p->desc)) return SCORE_DESC_PREFIX;
    }
    if (lstrcmpiW(w->title, label) == 0) return SCORE_TITLE;
    if (lstrlenW(p->product) >= 3 && lstrcmpiW(p->product, label) == 0) return SCORE_PRODUCT;
    if (ContainsWordI(w->title, label, TRUE)) return SCORE_TITLE_END;       /* "Document - App" */
    if (ContainsWordI(w->title, label, FALSE)) return SCORE_TITLE_WORD;
    if (lstrlenW(p->exe) >= 3) {
        /* "Visual Studio Code" ~ Code.exe, "Word" ~ WINWORD.EXE */
        WCHAR compact[256];
        int k = 0;
        for (int i = 0; label[i] && k < 255; i++)
            if (label[i] != L' ') compact[k++] = label[i];
        compact[k] = 0;
        if (StrStrIW(compact, p->exe) || (k >= 3 && StrStrIW(p->exe, compact))) return SCORE_EXE_NAME;
    }
    return 0;
}

/* Works out which processes belong to the taskbar button under the cursor. */
static BOOL ResolveTarget(POINT pt, Target *t)
{
    WCHAR raw[256], label[256];
    const ProcInfo *bp;
    BOOL counted;
    int best = -1, minScore;

    ZeroMemory(t, sizeof(*t));
    if (!TaskListNameAt(pt, raw, 256) && !AccNameAt(pt, raw, 256)) {
        Log(L"resolve: no accessible name at (%d,%d)", pt.x, pt.y);
        return FALSE;
    }
    lstrcpynW(label, raw, 256);
    counted = StripCountSuffix(label);
    Log(L"resolve: button \"%s\" -> label \"%s\"%s", raw, label, counted ? L"" : L" (no window count)");
    if (lstrlenW(label) < 2) return FALSE;

    /* Without a window count the button may be a pinned shortcut of an app that is not running.
     * Then only an exact match is good enough; a loose one could hit an unrelated app. */
    minScore = counted ? SCORE_EXE_NAME : SCORE_TITLE;

    g_nwins = g_nprocs = 0;
    EnumWindows(CollectWindow, 0);
    for (int i = 0; i < g_nwins; i++) {
        const ProcInfo *p = &g_procs[g_wins[i].proc];
        g_wins[i].score = ScoreWindow(&g_wins[i], p, raw, label);
        if (g_wins[i].score > 0)
            Log(L"  score %d  pid %lu  %s.exe  desc \"%s\"  title \"%.120s\"",
                g_wins[i].score, p->pid, p->exe, p->desc, g_wins[i].title);
        if (g_wins[i].score >= minScore && (best < 0 || g_wins[i].score > g_wins[best].score)) best = i;
    }
    if (best < 0) {
        Log(L"resolve: no window matches (%d windows checked, minimum score %d)", g_nwins, minScore);
        return FALSE;
    }

    /* Every process of the same app that matched equally well (for example two Notepads). */
    bp = &g_procs[g_wins[best].proc];
    for (int i = 0; i < g_nwins && t->count < MAX_TARGETS; i++) {
        const ProcInfo *p = &g_procs[g_wins[i].proc];
        BOOL seen = FALSE;
        if (g_wins[i].score != g_wins[best].score || lstrcmpiW(p->exe, bp->exe) != 0) continue;
        for (int j = 0; j < t->count; j++)
            if (t->procs[j].pid == p->pid) { seen = TRUE; break; }
        if (seen) continue;
        t->procs[t->count].pid = p->pid;
        t->procs[t->count].created = p->created;
        t->count++;
    }
    wsprintfW(t->exe, L"%s.exe", bp->exe);
    lstrcpynW(t->label, label, ARRAYSIZE(t->label));
    Log(L"resolve: target %s, %d process(es), score %d", t->exe, t->count, g_wins[best].score);
    return t->count > 0;
}

/* ---------------------------------------------------------------- killing */

/* Opens a process for termination and reports when it was created. NULL + *err on failure. */
static HANDLE OpenForKill(DWORD pid, FILETIME *created, DWORD *err)
{
    FILETIME e, k, u;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);

    if (!h) { *err = GetLastError(); return NULL; }
    if (!GetProcessTimes(h, created, &e, &k, &u)) { *err = GetLastError(); CloseHandle(h); return NULL; }
    if (IsCritical(h)) { *err = ERROR_ACCESS_DENIED; CloseHandle(h); return NULL; }   /* would crash Windows */
    *err = ERROR_SUCCESS;
    return h;
}

/*
 * Ends a process and everything it started (like "taskkill /T /F").
 * Handles to the whole tree are opened first and each one is checked against the creation time,
 * so a PID that was recycled in the meantime can never be hit by mistake.
 * Returns ERROR_SUCCESS if the root process is gone afterwards.
 */
static DWORD KillTree(const ProcRef *root)
{
    HANDLE   handles[MAX_TREE];
    DWORD    pids[MAX_TREE];
    FILETIME times[MAX_TREE];
    DWORD    err = 0, rootErr = ERROR_SUCCESS;
    HANDLE   snap;
    int      n = 0;

    handles[0] = OpenForKill(root->pid, &times[0], &err);
    if (!handles[0]) {
        Log(L"  pid %lu: cannot open (error %lu)", root->pid, err);
        return err == ERROR_INVALID_PARAMETER ? ERROR_SUCCESS : err;       /* no such process: already gone */
    }
    if (CompareFileTime(&times[0], &root->created) != 0 || WaitForSingleObject(handles[0], 0) == WAIT_OBJECT_0) {
        Log(L"  pid %lu: already exited", root->pid);
        CloseHandle(handles[0]);
        return ERROR_SUCCESS;
    }
    pids[0] = root->pid;
    n = 1;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        for (int i = 0; i < n; i++) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (!Process32FirstW(snap, &pe)) break;
            do {
                WCHAR name[128];
                FILETIME created;
                DWORD cpid = pe.th32ProcessID;
                BOOL seen = FALSE;
                HANDLE h;

                if (pe.th32ParentProcessID != pids[i] || !cpid || cpid == GetCurrentProcessId() || n >= MAX_TREE)
                    continue;
                for (int j = 0; j < n; j++)
                    if (pids[j] == cpid) { seen = TRUE; break; }
                if (seen) continue;
                BaseNameNoExt(pe.szExeFile, name, 128);
                if (IsProtectedName(name)) continue;
                h = OpenForKill(cpid, &created, &err);
                if (!h) continue;
                /* The parent PID may be a recycled number: a real child cannot be older than its parent. */
                if (CompareFileTime(&created, &times[i]) < 0) { CloseHandle(h); continue; }
                handles[n] = h;
                pids[n] = cpid;
                times[n] = created;
                n++;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    /* Root first, so it cannot restart the children while they are being ended. */
    for (int i = 0; i < n; i++) {
        BOOL ok = g_dryRun ? TRUE : TerminateProcess(handles[i], 1);
        err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) {  /* exited on its own meanwhile */
            ok = TRUE;
            err = ERROR_SUCCESS;
        }
        Log(L"  %s pid %lu%s (result %lu)", g_dryRun ? L"would end" : L"end", pids[i], i ? L" (child)" : L"", err);
        if (i == 0) rootErr = err;
        CloseHandle(handles[i]);
    }
    return rootErr;
}

static void KillTarget(const Target *t)
{
    DWORD lastErr = ERROR_SUCCESS;
    int ended = 0;

    Log(L"%s: %s \"%s\" (%d process(es))", g_dryRun ? L"dry run" : L"end task", t->exe, t->label, t->count);
    for (int i = 0; i < t->count; i++) {
        DWORD err = KillTree(&t->procs[i]);
        if (err == ERROR_SUCCESS) ended++;
        else lastErr = err;
    }
    /* Stay quiet on success; only speak up when nothing could be ended. */
    if (!ended && t->count > 0) {
        WCHAR msg[600];
        if (lastErr == ERROR_ACCESS_DENIED)
            wsprintfW(msg, L"%.60s is running as administrator or is protected by Windows. "
                           L"Run " APP_NAME L" as administrator to end apps like this.", t->label);
        else
            wsprintfW(msg, L"%.60s could not be ended (error %lu).", t->label, lastErr);
        Notify(L"Couldn't end task", msg, NIIF_WARNING);
    }
}

/* ----------------------------------------------------- the End task button */

static void EnsureFonts(int dpi)
{
    WCHAR face[LF_FACESIZE];
    HFONT old;
    HDC dc;

    if (g_fontText && g_fontDpi == dpi) return;
    if (g_fontText) DeleteObject(g_fontText);
    if (g_fontIcon) DeleteObject(g_fontIcon);
    g_fontDpi = dpi;
    g_glyph[0] = 0xE711;
    g_fontText = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontIcon = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    /* If the icon font is missing, fall back to a plain multiplication sign. */
    dc = GetDC(NULL);
    old = (HFONT)SelectObject(dc, g_fontIcon);
    face[0] = 0;
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    if (lstrcmpiW(face, L"Segoe MDL2 Assets") != 0) {
        DeleteObject(g_fontIcon);
        g_fontIcon = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
        g_glyph[0] = 0x2715;
    }
}

static void HidePopup(const WCHAR *why)
{
    if (IsWindowVisible(g_popup)) {
        ShowWindow(g_popup, SW_HIDE);
        Log(L"button hidden (%s)", why);
    }
    g_popupHover = g_popupPressed = FALSE;
    g_menuWnd = NULL;
}

static void EndSession(const WCHAR *why)
{
    HidePopup(why);
    g_sess = FALSE;
}

/*
 * Finds the right-click menu (jump list) that is open next to the taskbar button of the session.
 * There is no API for this, so it is recognised by its owner (the shell), its size and its
 * position: it touches the taskbar and lines up with the cursor.
 */
static HWND FindJumpList(RECT *out)
{
    MONITORINFO mi;
    int dpi = DpiAt(g_sessPt);
    int minW = MulDiv(120, dpi, 96), maxW = MulDiv(900, dpi, 96), minH = MulDiv(40, dpi, 96);
    int near_ = MulDiv(60, dpi, 96), overlap = MulDiv(20, dpi, 96), side = MulDiv(40, dpi, 96);
    HWND h = NULL;

    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromPoint(g_sessPt, MONITOR_DEFAULTTONEAREST), &mi)) return NULL;

    /* EnumWindows skips the shell's own "immersive" windows; walking with FindWindowEx does not. */
    for (int i = 0; i < 4000; i++) {
        WCHAR cls[64], exe[64];
        RECT r;
        int w, ht, gap, along, lo, hi;

        h = FindWindowExW(NULL, h, NULL, NULL);
        if (!h) break;
        if (h == g_popup || h == g_main || !IsShown(h) || !GetWindowRect(h, &r)) continue;
        w = r.right - r.left;
        ht = r.bottom - r.top;
        if (w < minW || w > maxW || ht < minH || ht >= mi.rcMonitor.bottom - mi.rcMonitor.top) continue;

        switch (g_sessEdge) {
        case EDGE_TOP:   gap = r.top - g_sessTask.bottom;  along = g_sessPt.x; lo = r.left; hi = r.right;  break;
        case EDGE_LEFT:  gap = r.left - g_sessTask.right;  along = g_sessPt.y; lo = r.top;  hi = r.bottom; break;
        case EDGE_RIGHT: gap = g_sessTask.left - r.right;  along = g_sessPt.y; lo = r.top;  hi = r.bottom; break;
        default:         gap = g_sessTask.top - r.bottom;  along = g_sessPt.x; lo = r.left; hi = r.right;  break;
        }
        if (gap < -overlap || gap > near_ || along < lo - side || along > hi + side) continue;

        ClassOf(h, cls, 64);
        ExeOfWindow(h, exe, 64);
        /*
         * Plenty of other things sit right above a taskbar button: an app window snapped there,
         * and above all the thumbnail preview that shows while hovering (TaskListOverlayWnd),
         * which is still fading out when the menu opens. Only two windows are real menus:
         * the jump list (a CoreWindow of ShellExperienceHost) and the classic menu that
         * Shift+right-click opens (#32768, owned by Explorer).
         */
        if (!lstrcmpW(cls, L"Windows.UI.Core.CoreWindow") && !lstrcmpiW(exe, L"ShellExperienceHost")) {
            /* jump list */
        } else if (!lstrcmpW(cls, L"#32768") && !lstrcmpiW(exe, L"explorer")) {
            /* classic context menu */
        } else if (!g_isWin10 && (!lstrcmpiW(exe, L"explorer") || !lstrcmpiW(exe, L"ShellExperienceHost")) &&
                   lstrcmpW(cls, L"Shell_TrayWnd") && lstrcmpW(cls, L"Shell_SecondaryTrayWnd") &&
                   lstrcmpW(cls, L"TaskListThumbnailWnd") && lstrcmpW(cls, L"TaskListOverlayWnd") &&
                   lstrcmpW(cls, L"CabinetWClass") && lstrcmpW(cls, L"ExploreWClass") &&
                   lstrcmpW(cls, L"Progman") && lstrcmpW(cls, L"WorkerW") && lstrcmpW(cls, L"tooltips_class32")) {
            /* Windows 11 before the built-in End task: the menu's class is not known for sure, so accept any shell window that is not a known non-menu. */
        } else {
            for (int k = 0; k < (int)ARRAYSIZE(g_logRejected); k++) {
                if (g_logRejected[k] == h) break;
                if (g_logRejected[k]) continue;
                g_logRejected[k] = h;
                Log(L"menu candidate rejected: class %s, owner %s", cls, exe[0] ? exe : L"?");
                break;
            }
            continue;
        }
        if (h != g_logMenu || !EqualRect(&r, &g_menuRect)) {
            g_logMenu = h;
            Log(L"menu found: class %s, owner %s, rect (%d,%d)-(%d,%d)", cls, exe, r.left, r.top, r.right, r.bottom);
        }
        *out = r;
        return h;
    }
    return NULL;
}

/*
 * Shows the button, or moves it if it is already up: glued to the menu (same width) if there is
 * one, otherwise next to the cursor.
 */
static void PlacePopup(const RECT *menu, HWND menuWnd)
{
    BOOL wasVisible = IsWindowVisible(g_popup);
    MONITORINFO mi;
    RECT mon;
    int dpi = DpiAt(g_sessPt);
    int h = MulDiv(POPUP_HEIGHT, dpi, 96), gap = MulDiv(4, dpi, 96), x, y, w;

    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(g_sessPt, MONITOR_DEFAULTTONEAREST), &mi)) mon = mi.rcMonitor;
    else SetRect(&mon, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    if (menu) {
        x = menu->left;
        w = menu->right - menu->left;
        y = g_sessEdge == EDGE_TOP ? menu->bottom : menu->top - h;
        if (y < mon.top) y = menu->bottom;
        if (y + h > mon.bottom) y = menu->top - h;
        lstrcpyW(g_popupLabel, L"End task");
    } else {
        int off = MulDiv(170, dpi, 96);
        w = MulDiv(280, dpi, 96);
        switch (g_sessEdge) {
        case EDGE_TOP:
            x = g_sessPt.x + off;
            if (x + w > mon.right) x = g_sessPt.x - off - w;
            y = g_sessTask.bottom + gap;
            break;
        case EDGE_LEFT:
            x = g_sessTask.right + gap;
            y = g_sessPt.y - h / 2;
            break;
        case EDGE_RIGHT:
            x = g_sessTask.left - gap - w;
            y = g_sessPt.y - h / 2;
            break;
        default:
            x = g_sessPt.x + off;
            if (x + w > mon.right) x = g_sessPt.x - off - w;
            y = g_sessTask.top - gap - h;
            break;
        }
        wsprintfW(g_popupLabel, L"End task: %.200s", g_sessTarget.label);
    }
    if (x + w > mon.right) x = mon.right - w;
    if (x < mon.left) x = mon.left;
    if (y + h > mon.bottom) y = mon.bottom - h;
    if (y < mon.top) y = mon.top;

    /* Show which program is about to be ended, so a wrong guess is visible before the click. */
    if (g_sessTarget.count > 1)
        wsprintfW(g_popupDetail, L"%.100s \u00D7%d", g_sessTarget.exe, g_sessTarget.count);
    else
        lstrcpynW(g_popupDetail, g_sessTarget.exe, ARRAYSIZE(g_popupDetail));

    EnsureFonts(dpi);
    g_popupDpi = dpi;
    g_popupLight = SystemUsesLightTheme();
    g_popupTarget = g_sessTarget;
    g_menuWnd = menuWnd;
    if (menu) {
        g_menuRect = *menu;
        g_sessAttached = TRUE;
    }
    if (!wasVisible) {
        g_popupHover = g_popupPressed = FALSE;
        g_shownAt = GetTickCount64();
        g_hideAt = g_shownAt + POPUP_LIFE_MS;
    }
    SetWindowPos(g_popup, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_popup, NULL, TRUE);
    Log(L"button %s at (%d,%d) %dx%d, %s, dpi %d", wasVisible ? L"moved" : L"shown", x, y, w, h,
        menu ? L"attached to menu" : L"menu not found", dpi);
}

static void SendEsc(void)
{
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_ESCAPE;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_ESCAPE;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void PaintPopup(HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT rc, r;
    SIZE labelSize = { 0, 0 };
    HDC dc = BeginPaint(hwnd, &ps);
    COLORREF bgColor, textColor, dimColor, lineColor;
    HBRUSH bg, line;
    HFONT old;
    int iconW = MulDiv(40, g_popupDpi, 96), pad = MulDiv(12, g_popupDpi, 96);

    if (g_popupHover) {
        bgColor = g_popupPressed ? RGB(164, 38, 24) : RGB(196, 43, 28);
        textColor = RGB(255, 255, 255);
        dimColor = RGB(255, 208, 200);
        lineColor = bgColor;
    } else if (g_popupLight) {
        bgColor = RGB(242, 242, 242);
        textColor = RGB(0, 0, 0);
        dimColor = RGB(96, 96, 96);
        lineColor = RGB(204, 204, 204);
    } else {
        bgColor = RGB(43, 43, 43);
        textColor = RGB(255, 255, 255);
        dimColor = RGB(160, 160, 160);
        lineColor = RGB(70, 70, 70);
    }
    bg = CreateSolidBrush(bgColor);
    line = CreateSolidBrush(lineColor);

    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, bg);
    r = rc;
    r.top = rc.bottom - 1;
    FillRect(dc, &r, line);                         /* thin separator towards the menu below */

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textColor);
    r = rc;
    r.right = iconW;
    old = (HFONT)SelectObject(dc, g_fontIcon);
    DrawTextW(dc, g_glyph, 1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);

    SelectObject(dc, g_fontText);
    GetTextExtentPoint32W(dc, g_popupLabel, lstrlenW(g_popupLabel), &labelSize);
    r = rc;
    r.left = iconW;
    r.right -= pad;
    DrawTextW(dc, g_popupLabel, -1, &r, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    /* program name, right-aligned and dimmed, if there is room for it */
    r.left = iconW + labelSize.cx + pad;
    if (g_popupDetail[0] && r.right - r.left >= MulDiv(48, g_popupDpi, 96)) {
        SetTextColor(dc, dimColor);
        DrawTextW(dc, g_popupDetail, -1, &r, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    SelectObject(dc, old);
    DeleteObject(bg);
    DeleteObject(line);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;               /* never take focus: the menu must stay open */
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    case WM_MOUSEMOVE:
        if (!g_popupHover) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_popupHover = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        g_popupHover = g_popupPressed = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        g_popupPressed = TRUE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        /* Like a menu item: acts on release, so sliding off the button cancels. */
        if (g_popupPressed) {
            Target t = g_popupTarget;
            BOOL menuOpen = g_menuWnd && IsShown(g_menuWnd);
            Log(L"button clicked");
            EndSession(L"clicked");
            if (menuOpen) SendEsc();        /* close the right-click menu that is still open */
            KillTarget(&t);
        }
        return 0;
    case WM_PAINT:
        PaintPopup(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* -------------------------------------------------------------- main loop */

static void SetPollRate(UINT ms)
{
    if (ms == g_pollMs) return;
    g_pollMs = ms;
    SetTimer(g_main, TIMER_POLL, ms, NULL);
}

/*
 * Mouse button events come from Raw Input: unlike polling, it cannot miss a click, however short
 * (a touchpad tap presses and releases within a millisecond). It is not a hook: events are only
 * delivered to us, never intercepted. To cost nothing while the mouse is busy elsewhere (games),
 * it is switched on only while the cursor is on the taskbar or the button is up.
 */
static void SetMouseEvents(BOOL on)
{
    RAWINPUTDEVICE dev;
    if (on == g_rawMouse) return;
    dev.usUsagePage = 0x01;                 /* generic desktop */
    dev.usUsage = 0x02;                     /* mouse */
    dev.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    dev.hwndTarget = on ? g_main : NULL;
    if (RegisterRawInputDevices(&dev, 1, sizeof(dev))) g_rawMouse = on;
}

/* The context-menu mouse button went down: work out the target right away. */
static void OnContextPress(POINT pt, const WCHAR *via)
{
    BOOL onButton = FALSE;
    RECT task;

    EndSession(L"right-click");
    if (g_useNative || !TaskbarAt(pt, &task, &onButton) || !onButton) return;
    Log(L"right-click at (%d,%d), seen by %s", pt.x, pt.y, via);
    g_sess = TRUE;
    g_sessReleased = g_sessAttached = FALSE;
    g_sessPt = pt;
    g_sessTask = task;
    g_sessEdge = TaskbarEdge(&task);
    g_sessResolved = ResolveTarget(pt, &g_sessTarget);
    g_sessRetries = 0;
    g_sessRetryAt = GetTickCount64() + RETRY_MS;
    g_logMenu = NULL;
    ZeroMemory(g_logRejected, sizeof(g_logRejected));
}

/* ...and came back up: Windows opens the menu now. */
static void OnContextRelease(ULONGLONG now)
{
    if (!g_sess || g_sessReleased) return;
    g_sessReleased = TRUE;
    g_sessDeadline = now + MENU_WAIT_MS;
}

static void OnPrimaryPress(POINT pt)
{
    RECT r;
    if (!IsWindowVisible(g_popup)) return;
    GetWindowRect(g_popup, &r);
    if (!PtInRect(&r, pt)) EndSession(L"click elsewhere");
}

static void OnRawMouse(USHORT flags)
{
    /* With swapped mouse buttons the physical left button opens context menus. */
    BOOL swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    USHORT ctxDown = swapped ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_RIGHT_BUTTON_DOWN;
    USHORT ctxUp   = swapped ? RI_MOUSE_LEFT_BUTTON_UP : RI_MOUSE_RIGHT_BUTTON_UP;
    USHORT priDown = swapped ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_DOWN;
    ULONGLONG now = GetTickCount64();
    POINT pt;

    if (g_useNative || !(flags & (ctxDown | ctxUp | priDown))) return;
    GetCursorPos(&pt);
    if (flags & ctxDown) {
        g_rawCtxPressAt = now;
        OnContextPress(pt, L"mouse event");
    }
    if (flags & ctxUp) OnContextRelease(now);
    if (flags & priDown) OnPrimaryPress(pt);
}

static void OnTick(void)
{
    ULONGLONG now = GetTickCount64();
    BOOL onBar, onButton = FALSE, hot, watching;
    RECT task;
    POINT pt;

    /* Started before the taskbar existed (sign-in): keep trying until the icon is in. */
    if (!g_trayAdded) g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);

    GetCursorPos(&pt);
    onBar = TaskbarAt(pt, &task, &onButton);

    /* Hotkey: Ctrl+Shift+E while the cursor is on an app's taskbar button. */
    hot = g_hotkey && KeyDown(VK_CONTROL) && KeyDown(VK_SHIFT) && KeyDown(HOTKEY_VK) &&
          !KeyDown(VK_MENU) && !KeyDown(VK_LWIN) && !KeyDown(VK_RWIN);
    if (hot && !g_prevHot && onBar && onButton) {
        Target t;
        Log(L"hotkey at (%d,%d)", pt.x, pt.y);
        EndSession(L"hotkey");
        if (ResolveTarget(pt, &t)) KillTarget(&t);
    }
    g_prevHot = hot;

    /* Someone may flip the Windows 11 setting in the Settings app while we run. */
    if (g_nativeCapable && now >= g_nativeCheckAt) {
        g_useNative = NativeEndTaskEnabled();
        g_nativeCheckAt = now + 2000;
        if (g_useNative) EndSession(L"native End task is on");
    }

    if (!g_useNative) {
        /* Backup for the mouse events above, in case Raw Input is not delivering (yet). */
        BOOL swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        int cs = KeyState(swapped ? VK_LBUTTON : VK_RBUTTON);      /* context-menu button */
        int ps = KeyState(swapped ? VK_RBUTTON : VK_LBUTTON);      /* primary button */
        BOOL ctxDown = (cs & 1) != 0;

        if (!g_prevCtx && cs != 0 && now - g_rawCtxPressAt > RAW_DEDUPE_MS) OnContextPress(pt, L"polling");
        g_prevCtx = ctxDown;
        if (!ctxDown) OnContextRelease(now);

        if (g_sess && !g_sessResolved && now >= g_sessRetryAt) {
            g_sessResolved = ResolveTarget(g_sessPt, &g_sessTarget);
            g_sessRetryAt = now + RETRY_MS;
            if (!g_sessResolved && ++g_sessRetries >= MAX_RETRIES) EndSession(L"no target");
        }

        /* Follow the menu: show the button when it opens, move with it, hide when it closes. */
        if (g_sess && g_sessResolved && g_sessReleased) {
            BOOL visible = IsWindowVisible(g_popup);
            RECT menu;
            HWND menuWnd = FindJumpList(&menu);
            if (menuWnd) {
                if (!visible || menuWnd != g_menuWnd || !EqualRect(&menu, &g_menuRect)) PlacePopup(&menu, menuWnd);
            } else if (visible && g_menuWnd) {
                /* It may have been the previous app's menu on its way out; the right one can still come. */
                HidePopup(L"menu closed");
                if (g_sessDeadline < now + MENU_GRACE_MS) g_sessDeadline = now + MENU_GRACE_MS;
            } else if (!visible && now >= g_sessDeadline) {
                if (g_sessAttached) EndSession(L"menu gone");
                else PlacePopup(NULL, NULL);                       /* never saw a menu: show it by the cursor */
            }
        }

        /* Hide the button: timed out, Esc, or a click somewhere else. */
        if (IsWindowVisible(g_popup) && now >= g_shownAt + 300) {
            if (now >= g_hideAt) EndSession(L"timeout");
            else if (KeyDown(VK_ESCAPE)) EndSession(L"Esc");
            else if (ps != 0) OnPrimaryPress(pt);
        }
    }

    /* Watch closely only while there is something to watch. */
    watching = onBar || g_sess || IsWindowVisible(g_popup);
    SetPollRate(watching ? POLL_FAST_MS : POLL_IDLE_MS);
    SetMouseEvents(watching && !g_useNative);
}

/* -------------------------------------------------------------- tray icon */

static void AddTrayIcon(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, g_dryRun ? APP_NAME L" (dry run)" : APP_NAME);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);   /* fails while the taskbar is not up yet: OnTick retries */
}

static void ShowHelp(void)
{
    MessageBoxW(NULL,
        APP_NAME L" " APP_VERSION L"\n\n"
        L"Force-close an app straight from the taskbar:\n\n"
        L"  \u2022  Right-click the app on the taskbar and choose \"End task\" on top of the menu.\n"
        L"  \u2022  Or hover the app on the taskbar and press " HOTKEY_TEXT L".\n\n"
        L"The app is ended immediately, together with the processes it started. "
        L"Unsaved work in that app is lost.\n\n"
        L"Apps running as administrator can only be ended if " APP_NAME L" runs as administrator too.\n\n"
        L"Command line:  --install   --uninstall   --exit   --log   --dry-run",
        APP_NAME, MB_ICONINFORMATION);
}

static void ShowTrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int choice;

    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, APP_NAME L" " APP_VERSION);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP,
                IsElevated() ? L"Start with Windows (as administrator)" : L"Start with Windows");
    AppendMenuW(m, MF_STRING | (g_hotkey ? MF_CHECKED : 0), ID_HOTKEY, HOTKEY_TEXT L" hotkey");
    if (g_nativeCapable)
        AppendMenuW(m, MF_STRING | (NativeEndTaskEnabled() ? MF_CHECKED : 0), ID_NATIVE,
                    L"Use Windows 11's built-in \"End task\"");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_HELP, L"How to use\u2026");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    choice = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (choice) {
    case ID_STARTUP:
        if (StartupEnabled()) {
            if (!DisableStartup())
                MessageBoxW(NULL, L"This startup entry was created as administrator.\n\n"
                                  L"Run " APP_NAME L" as administrator to switch it off.",
                            APP_NAME, MB_ICONWARNING | MB_SETFOREGROUND);
        } else {
            WCHAR self[MAX_PATH];
            if (!GetModuleFileNameW(NULL, self, MAX_PATH) || !EnableStartup(self))
                MessageBoxW(NULL, L"The task that starts " APP_NAME L" with Windows could not be created.",
                            APP_NAME, MB_ICONWARNING | MB_SETFOREGROUND);
        }
        break;
    case ID_HOTKEY:
        g_hotkey = !g_hotkey;
        SetSetting(L"Hotkey", g_hotkey);
        break;
    case ID_NATIVE:
        SetNativeEndTask(!NativeEndTaskEnabled());
        g_useNative = NativeEndTaskEnabled();
        break;
    case ID_HELP:
        ShowHelp();
        break;
    case ID_EXIT:
        DestroyWindow(g_main);
        break;
    }
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        /* Explorer (re)started: put the icon back. */
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_POLL) OnTick();
        return 0;
    case WM_INPUT: {
        RAWINPUT in;
        UINT size = sizeof(in);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &in, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            in.header.dwType == RIM_TYPEMOUSE && in.data.mouse.usButtonFlags)
            OnRawMouse(in.data.mouse.usButtonFlags);
        break;                              /* DefWindowProc releases the input record */
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- startup */

static void LoadOptionalApis(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE shcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDpiContextFn setContext = NULL;

    /* The manifest already asks for per-monitor DPI awareness; this covers builds without it. */
    if (user32) setContext = (SetDpiContextFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (!setContext || !setContext((HANDLE)(INT_PTR)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();

    if (shcore) g_getDpiForMonitor = (GetDpiForMonitorFn)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
    if (kernel32) g_isProcessCritical = (IsProcessCriticalFn)(void *)GetProcAddress(kernel32, "IsProcessCritical");
}

/* Windows 11 (22631.2715 and later) has "End task" built into the taskbar menu, behind a setting. */
static void DetectWindows(void)
{
    const WCHAR *cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    WCHAR buf[32];
    DWORD size = sizeof(buf), ubr = 0, usize = sizeof(ubr);
    int build = 0;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, cv, L"CurrentBuildNumber", RRF_RT_REG_SZ, NULL, buf, &size) == ERROR_SUCCESS)
        build = StrToIntW(buf);
    RegGetValueW(HKEY_LOCAL_MACHINE, cv, L"UBR", RRF_RT_REG_DWORD, NULL, &ubr, &usize);

    g_isWin10 = build > 0 && build < 22000;
    g_nativeCapable = build > 22631 || (build == 22631 && ubr >= 2715);
    g_useNative = g_nativeCapable && NativeEndTaskEnabled();
    Log(L"Windows build %d.%lu, win10=%d, nativeCapable=%d, useNative=%d", build, ubr, g_isWin10, g_nativeCapable, g_useNative);
}

static BOOL HasSwitch(int argc, WCHAR **argv, const WCHAR *name)
{
    for (int i = 1; i < argc; i++) {
        const WCHAR *a = argv[i];
        if (a[0] == L'-' && a[1] == L'-') a += 2;
        else if (a[0] == L'-' || a[0] == L'/') a += 1;
        else continue;
        if (lstrcmpiW(a, name) == 0) return TRUE;
    }
    return FALSE;
}

/*
 * Asks a running copy to exit and waits for it. Returns FALSE if it is still there: Windows does
 * not let a normal process send messages to one that runs as administrator.
 */
static BOOL StopRunningInstance(void)
{
    for (int round = 0; round < 4; round++) {
        HWND old = FindWindowW(MAIN_CLASS, NULL);
        if (!old) return TRUE;
        if (!PostMessageW(old, WM_CLOSE, 0, 0)) return FALSE;
        for (int i = 0; i < 50 && IsWindow(old); i++) Sleep(100);
    }
    return FindWindowW(MAIN_CLASS, NULL) == NULL;
}

static void ExplainStillRunning(void)
{
    MessageBoxW(NULL,
        APP_NAME L" is already running as administrator, so this copy cannot close it.\n\n"
        L"Exit it from its icon near the clock (the red X), then try again.",
        APP_NAME, MB_ICONWARNING | MB_SETFOREGROUND);
}

/* ------------------------------------------------------------------ setup */

/*
 * Where the installed copy lives (MAX_PATH chars each):
 *   normally          %LOCALAPPDATA%\Programs\TaskbarEndTask\EndTask.exe
 *   as administrator  %ProgramFiles%\TaskbarEndTask\EndTask.exe
 * A copy that Windows starts with administrator rights must not sit where any program can
 * overwrite it, hence the second place.
 */
static BOOL GetInstallPathsFor(BOOL admin, WCHAR *dir, WCHAR *exe)
{
    DWORD n = GetEnvironmentVariableW(admin ? L"ProgramFiles" : L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n > MAX_PATH - 48) return FALSE;
    lstrcatW(dir, admin ? L"\\" APP_ID : L"\\Programs\\" APP_ID);
    wsprintfW(exe, L"%s\\EndTask.exe", dir);
    return TRUE;
}

static BOOL GetInstallPaths(WCHAR *dir, WCHAR *exe)
{
    return GetInstallPathsFor(IsElevated(), dir, exe);
}

static BOOL IsInstalledCopy(void)
{
    WCHAR self[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return FALSE;
    for (int admin = 0; admin <= 1; admin++)
        if (GetInstallPathsFor(admin, dir, exe) && lstrcmpiW(self, exe) == 0) return TRUE;
    return FALSE;
}

static BOOL InstalledCopyExists(void)
{
    WCHAR dir[MAX_PATH], exe[MAX_PATH];
    return GetInstallPaths(dir, exe) && GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES;
}

/*
 * Copies this exe to the install folder, makes it start with Windows and starts it.
 * Only ever runs because the user asked for it: "Yes" in the first-run question, or --install.
 */
static void Install(void)
{
    WCHAR self[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], parent[MAX_PATH], mark[MAX_PATH + 20], text[700];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL stopped, started = FALSE;

    if (!GetModuleFileNameW(NULL, self, MAX_PATH) || !GetInstallPaths(dir, exe)) return;
    stopped = StopRunningInstance();
    if (lstrcmpiW(self, exe) != 0) {
        BOOL copied = FALSE;
        lstrcpyW(parent, dir);
        PathRemoveFileSpecW(parent);
        CreateDirectoryW(parent, NULL);
        CreateDirectoryW(dir, NULL);
        for (int i = 0; i < 20 && !copied; i++) {       /* the old copy may still be shutting down */
            copied = CopyFileW(self, exe, FALSE);
            if (!copied) Sleep(150);
        }
        if (!copied) {
            if (!stopped) ExplainStillRunning();
            else MessageBoxW(NULL, APP_NAME L" could not be set up: copying to the install folder failed.\n\n"
                                   L"You can still run this file directly.", APP_NAME, MB_ICONERROR | MB_SETFOREGROUND);
            return;
        }
        /* A downloaded exe carries a "from the internet" mark, and Windows would ask about the
         * copy at every sign-in. The user has just chosen to install it, so drop the mark. */
        wsprintfW(mark, L"%s:Zone.Identifier", exe);
        DeleteFileW(mark);
    }
    if (!EnableStartup(exe))
        MessageBoxW(NULL, APP_NAME L" was copied, but the task that starts it with Windows could not be created.",
                    APP_NAME, MB_ICONWARNING | MB_SETFOREGROUND);

    if (stopped) {
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        started = CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi);
        if (started) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    if (!started) {
        /* Installed, but the copy that runs as administrator is still the active one. */
        wsprintfW(text, APP_NAME L" is set up in\n%s\nand will start with Windows.\n\n%s", dir,
                  stopped ? L"It could not be started right now; start it from that folder."
                          : L"A copy running as administrator is still active and could not be closed. "
                            L"Exit it from its icon near the clock to switch to the installed one.");
        MessageBoxW(NULL, text, APP_NAME, MB_ICONINFORMATION | MB_SETFOREGROUND);
    }
}

/* Asks whether to set the program up (or to update the copy that is already set up). */
static BOOL OfferSetup(BOOL update)
{
    WCHAR dir[MAX_PATH], exe[MAX_PATH], text[900];

    if (!GetInstallPaths(dir, exe)) return FALSE;
    if (update)
        wsprintfW(text, APP_NAME L" is already set up on this PC.\n\n"
                        L"Replace the installed copy with this one (version " APP_VERSION L")?\n\n"
                        L"No: runs this copy from here and leaves the installed one alone.");
    else
        wsprintfW(text, L"Set up " APP_NAME L" on this PC?\n\n"
                        L"Yes: copies it to\n%s\nand starts it with Windows%s.\n\n"
                        L"No: just runs it from here. You can still switch on \"Start with Windows\" "
                        L"from the tray icon, or run it with --install later.", dir,
                  IsElevated() ? L", as administrator" : L"");
    return MessageBoxW(NULL, text, APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
}

/* Removes the startup entry, the settings and, if possible, the installed copy. */
static void Uninstall(void)
{
    WCHAR dir[MAX_PATH], exe[MAX_PATH], text[600];

    if (!StopRunningInstance()) {
        ExplainStillRunning();
        return;
    }
    if (!DisableStartup())
        MessageBoxW(NULL, APP_NAME L" starts with Windows through a task that was created as administrator.\n\n"
                          L"Run this again as administrator to remove that task too.",
                    APP_NAME, MB_ICONWARNING | MB_SETFOREGROUND);
    RegDeleteTreeW(HKEY_CURRENT_USER, SETTINGS_KEY);
    if (!GetInstallPaths(dir, exe)) return;
    if (IsInstalledCopy()) {
        /* A running exe cannot delete itself. */
        wsprintfW(text, APP_NAME L" was removed from startup and its settings were deleted.\n\n"
                        L"To finish, delete this folder:\n%s", dir);
        MessageBoxW(NULL, text, APP_NAME, MB_ICONINFORMATION | MB_SETFOREGROUND);
    } else {
        for (int i = 0; i < 20 && !DeleteFileW(exe) && GetLastError() != ERROR_FILE_NOT_FOUND &&
                        GetLastError() != ERROR_PATH_NOT_FOUND; i++)
            Sleep(150);
        RemoveDirectoryW(dir);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show)
{
    BOOL wantExit = FALSE, wantUninstall = FALSE, wantInstall = FALSE;
    WNDCLASSEXW wc;
    HANDLE mutex;
    WCHAR **argv;
    int argc = 0;
    MSG msg;
    (void)prev; (void)cmdLine; (void)show;

    g_inst = inst;
    LoadOptionalApis();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        wantExit = HasSwitch(argc, argv, L"exit");
        wantUninstall = HasSwitch(argc, argv, L"uninstall");
        wantInstall = HasSwitch(argc, argv, L"install");
        g_dryRun = HasSwitch(argc, argv, L"dry-run");
        g_log = g_dryRun || HasSwitch(argc, argv, L"log");
#ifdef ENDTASK_TEST_BUILD
        /* Lets the task code be exercised without administrator rights (run level "least privilege"). */
        if (HasSwitch(argc, argv, L"task-create")) {
            WCHAR self[MAX_PATH];
            return GetModuleFileNameW(NULL, self, MAX_PATH) && CreateStartupTask(self, FALSE) ? 0 : 1;
        }
        if (HasSwitch(argc, argv, L"task-exists")) return StartupTaskExists() ? 0 : 1;
        if (HasSwitch(argc, argv, L"task-delete")) return DeleteStartupTask() ? 0 : 1;
        if (HasSwitch(argc, argv, L"startup-state")) return (StartupEnabled() ? 1 : 0) | (IsElevated() ? 2 : 0) | (ForcedToRunAsAdmin() ? 4 : 0);
#endif
        LocalFree(argv);
    }
    if (wantUninstall) {
        Uninstall();
        return 0;
    }
    if (wantExit) {
        if (!StopRunningInstance()) ExplainStillRunning();
        return 0;
    }
    if (wantInstall) {
        Install();
        return 0;
    }
    /* Started by hand from somewhere else (typically the Downloads folder): offer to set it up,
     * once. Diagnostic runs (--log, --dry-run) never ask. */
    if (!g_log && !IsInstalledCopy()) {
        if (InstalledCopyExists()) {
            if (OfferSetup(TRUE)) { Install(); return 0; }
        } else if (!GetSetting(L"SetupAsked", 0)) {
            SetSetting(L"SetupAsked", 1);
            if (OfferSetup(FALSE)) { Install(); return 0; }
        }
    }

    /* One copy at a time: a newly started copy replaces the one already running (handy for updates). */
    mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (!StopRunningInstance()) {
        ExplainStillRunning();          /* two copies would both react to every click */
        return 0;
    }

    if (g_log) {
        DWORD n = GetTempPathW(MAX_PATH - 24, g_logPath);
        if (!n || n > MAX_PATH - 24) g_log = FALSE;
        else lstrcatW(g_logPath, APP_ID L".log");
    }
    Log(L"---- " APP_NAME L" " APP_VERSION L" started%s", g_dryRun ? L" (dry run: nothing will be ended)" : L"");

    DetectWindows();
    g_hotkey = GetSetting(L"Hotkey", 1) != 0;
    RefreshStartupPath();
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = PopupProc;
    wc.lpszClassName = POPUP_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_HAND);
    RegisterClassExW(&wc);

    /* Invisible main window: owns the timer and the tray icon. */
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, MAIN_CLASS, APP_NAME, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    /* The button: always on top, never takes focus. */
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, POPUP_CLASS, L"",
                              WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_main || !g_popup) return 1;

    /* A copy running as administrator does not get broadcasts from Explorer unless it asks for them. */
    if (g_msgTaskbarCreated) ChangeWindowMessageFilterEx(g_main, g_msgTaskbarCreated, MSGFLT_ALLOW, NULL);

    AddTrayIcon();
    SetPollRate(POLL_IDLE_MS);

    /* "Always run as administrator" plus a normal startup entry never starts: Windows skips it at
     * sign-in. Move the entry to a scheduled task, which is allowed to start with those rights. */
    if (IsElevated() && ForcedToRunAsAdmin() && RunValueEnabled()) {
        WCHAR self[MAX_PATH];
        if (GetModuleFileNameW(NULL, self, MAX_PATH) && EnableStartup(self))
            Notify(L"Start with Windows was repaired",
                   L"This copy always runs as administrator, and Windows does not start such programs from "
                   L"its normal startup list. It now starts through a scheduled task instead.", NIIF_INFO);
    }

    if (!GetSetting(L"FirstRunDone", 0)) {
        SetSetting(L"FirstRunDone", 1);
        if (g_nativeCapable && !g_useNative) {
            SetNativeEndTask(TRUE);
            g_useNative = TRUE;
        }
        if (g_useNative)
            Notify(APP_NAME L" is running",
                   L"Windows 11's own \"End task\" is now in the taskbar right-click menu. "
                   L"You can also hover an app and press " HOTKEY_TEXT L". Options: this icon's menu.", NIIF_INFO);
        else
            Notify(APP_NAME L" is running",
                   L"Right-click an app on the taskbar and choose \"End task\", or hover it and press "
                   HOTKEY_TEXT L". Options: this icon's menu.", NIIF_INFO);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log(L"---- exited");
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}

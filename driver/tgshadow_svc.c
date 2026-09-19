/*==============================================================================
  tgshadow_svc.c - TimeGuard 影子保护控制服务（P4）

  职责：
    1) 系统稳定后自动对受保护卷启用影子保护（配置见 HKLM\SOFTWARE\TimeGuard\Shadow）
    2) 关机/重启前弹出确认窗口："本次开机以来的修改是否保留？"
         - 保留：需要家长密码 -> IOCTL COMMIT（把影子数据写回真实卷）
         - 还原 / 超时 / 无人登录：IOCTL DISCARD（丢弃，重启即复原）
    3) install / uninstall / console / testask / ask128 / setpass 子命令便于部署调试

  ==== 安全设计（2026-09-19 事故教训，务必保留）====
  事故：第一版服务在【开机瞬间】就启用保护，结果系统连续启动失败，
        被 Windows 判定为"未正确启动"而进入自动修复，虚拟机彻底起不来。
  三个直接原因与对应安全阀：
    1) 开机期 I/O 最密集，COW 要在写路径里同步向下层设备再发 IRP -> 死锁/超时。
       对策：延迟启用（AutoEnableDelaySec，默认 120 秒）+ 必须已有用户登录。
    2) 启动瞬间分配大块非分页内存 -> 系统最缺内存时被吃光。
       对策：默认影子降到 128MB，并且不得超过物理内存的 25%。
    3) 没有任何"连续失败就别再试"的机制 -> 反复把机器拖进修复循环。
       对策：自锁计数 BootAttempts；连续 3 轮"启用后没跑满 180 秒就重启"
             就停止自动启用，必须人工排查。
  另外驱动侧还有第四个安全阀：分页 I/O（页面文件/映射文件）一律透传。

  安全要点：
    - 服务以 LocalSystem 运行；弹窗必须跨会话，用 WTSQueryUserToken +
      CreateProcessAsUser 在活动控制台会话里启动 tgshadow_ask.exe（Session 0
      隔离下服务自己弹窗用户看不到）。
    - 默认动作永远是"还原"：任何异常、超时、没有用户会话都走还原。
==============================================================================*/
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdlib.h>
#include "tgshadow.h"
#include "tgsha256.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

#define SVC_NAME        L"TgShadowSvc"
#define SVC_DISPLAY     L"TimeGuard Shadow Protection"
#define CFG_KEY         L"SOFTWARE\\TimeGuard\\Shadow"
#define ASK_EXE_NAME    L"tgshadow_ask.exe"
#define HEALTHY_SECONDS 180

static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static SERVICE_STATUS        g_Status;
static HANDLE                g_StopEvent = NULL;

/* 配置 */
static DWORD g_CfgEnabled   = 1;
static DWORD g_CfgVolume    = 0;
static DWORD g_CfgShadowMB  = 128;   /* 默认 128MB：启动期内存压力大，别贪多 */
static DWORD g_CfgTimeout   = 30;
static DWORD g_CfgDelaySec  = 120;   /* 开机后延迟启用：等系统真正起来再上保护 */
static WCHAR g_CfgAskPath[MAX_PATH] = L"";
static WCHAR g_CfgShadowPath[MAX_PATH] = L"";   /* 影子文件（磁盘后端）；空 = 内存后端 */
static HANDLE g_HealthThread = NULL;

/* ------------------------------------------------------------------ 日志 */

static void Log(const WCHAR *fmt, ...)
{
    WCHAR    buf[1024];
    va_list  ap;
    HANDLE   h;
    DWORD    written = 0;
    SYSTEMTIME st;
    WCHAR    line[1200];

    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    GetLocalTime(&st);
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"%04d-%02d-%02d %02d:%02d:%02d [tgshadow_svc] %s\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);

    /* 关机流程里 OutputDebugString 可能没人接，必须落盘才能事后查证 */
    h = CreateFileW(L"C:\\Windows\\Temp\\tgshadow_svc.log",
                    FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(WCHAR)), &written, NULL);
        CloseHandle(h);
    }
    OutputDebugStringW(line);
}

/* ------------------------------------------------------------------ 驱动接口 */

static HANDLE OpenDevice(void)
{
    return CreateFileW(TGSHADOW_WIN32_DEVICE, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
}

static BOOL QueryVolumeSize(DWORD vol, ULONGLONG *bytes)
{
    WCHAR path[64];
    HANDLE hv;
    DISK_GEOMETRY_EX geo;
    DWORD ret = 0;

    _snwprintf_s(path, _countof(path), _TRUNCATE, L"\\\\.\\HarddiskVolume%lu", vol);
    hv = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, 0, NULL);
    if (hv == INVALID_HANDLE_VALUE) return FALSE;
    if (!DeviceIoControl(hv, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                         &geo, sizeof(geo), &ret, NULL)) {
        CloseHandle(hv);
        return FALSE;
    }
    CloseHandle(hv);
    *bytes = (ULONGLONG)geo.DiskSize.QuadPart;
    return TRUE;
}

/* 用盘符查卷容量：比 \.HarddiskVolumeN + IOCTL_DISK_GET_LENGTH_INFO 更可靠
   （SYSTEM 会话里后者会失败，导致映射表退回 32GB 兜底，超出部分静默不受保护）。 */
static ULONGLONG QueryVolumeSizeByDrive(WCHAR drive)
{
    WCHAR root[4];
    ULARGE_INTEGER freeBytes, totalBytes, totalFree;

    root[0] = drive; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    if (!GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, &totalFree)) {
        return 0;
    }
    return (ULONGLONG)totalBytes.QuadPart;
}

/* 从卷的 DOS 设备名解析卷号（会话 0 里往往拿不到，失败返回 0 = 让驱动自动识别） */
static DWORD VolumeNumberFromDrive(WCHAR drive)
{
    WCHAR target[512];
    WCHAR letter[3];
    const WCHAR *p;
    DWORD n = 0;

    letter[0] = drive; letter[1] = L':'; letter[2] = 0;
    if (QueryDosDeviceW(letter, target, _countof(target)) == 0) {
        return 0;
    }
    p = wcsstr(target, L"HarddiskVolume");
    if (p == NULL) return 0;
    p += 13;
    while (*p >= L'0' && *p <= L'9') {
        n = n * 10 + (DWORD)(*p - L'0');
        p++;
    }
    return n;
}

/* 创建/扩充影子文件，返回实际字节数（0 = 失败）。主程序侧同一份逻辑见 share/shadow.py */
static ULONGLONG EnsureShadowFile(_In_ PCWSTR Path, _In_ ULONGLONG Want)
{
    HANDLE        h;
    LARGE_INTEGER sz, cur;

    h = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"打开影子文件失败 %s（错误 %lu）", Path, GetLastError());
        return 0;
    }
    ZeroMemory(&cur, sizeof(cur));
    if (GetFileSizeEx(h, &cur) && (ULONGLONG)cur.QuadPart >= Want) {
        CloseHandle(h);
        return (ULONGLONG)cur.QuadPart;
    }
    sz.QuadPart = (LONGLONG)Want;
    if (!SetFilePointerEx(h, sz, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
        Log(L"预分配影子文件失败 %s（错误 %lu）", Path, GetLastError());
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    Log(L"影子文件已就绪：%s（%llu MB）", Path, Want / (1024ULL * 1024ULL));
    return Want;
}

static BOOL DriverEnable(DWORD vol, DWORD mb)
{
    TGSHADOW_ENABLE_INPUT in;
    HANDLE h = OpenDevice();
    DWORD  ret = 0;
    BOOL   ok;

    if (h == INVALID_HANDLE_VALUE) {
        Log(L"启用失败：无法打开 \\\\.\\TgShadow（错误 %lu）", GetLastError());
        return FALSE;
    }
    ZeroMemory(&in, sizeof(in));
    in.VolumeNumber = vol;      /* 0 = 驱动用 DOS 名自动识别系统卷 */
    in.ShadowBytes  = (ULONGLONG)mb * 1024ULL * 1024ULL;
    /* 磁盘影子：主程序（管理界面）在注册表里配好 ShadowPath/ShadowMB，
       这里必须把它传给驱动，否则会退化成内存影子（容量受内存限制）。 */
    if (g_CfgShadowPath[0] != 0) {
        ULONGLONG actual = EnsureShadowFile(g_CfgShadowPath, in.ShadowBytes);

        if (actual == 0) {
            /* ⚠️ 绝不在这种情况下退化成内存影子：内存影子容量有限，
               用满后会"部分透传"，让卷处于半保护状态（曾因此把测试机搞成
               无法启动）。宁可不启用保护，也不进入半保护。 */
            Log(L"影子文件不可用（%s），本次【不启用保护】。"
                L"请检查影子所在磁盘的可用空间（需要 %llu MB）",
                g_CfgShadowPath, in.ShadowBytes / (1024ULL * 1024ULL));
            CloseHandle(h);
            return FALSE;
        }
        in.ShadowBytes = actual;
        /* ⚠️ 驱动要的是 NT 路径（\??\S:\x.bin），主程序配置里是 DOS 路径（S:\x.bin）。
           直接传 DOS 路径会得到 ERROR_BAD_PATHNAME(161)。 */
        {
            WCHAR nt[MAX_PATH];
            if (_wcsnicmp(g_CfgShadowPath, L"\\??\\", 4) == 0) {
                wcsncpy_s(nt, _countof(nt), g_CfgShadowPath, _TRUNCATE);
            } else {
                _snwprintf_s(nt, _countof(nt), _TRUNCATE,
                             L"\\??\\%s", g_CfgShadowPath);
            }
            wcsncpy_s(in.ShadowPath, _countof(in.ShadowPath), nt, _TRUNCATE);
            Log(L"影子路径(NT)：%s", in.ShadowPath);
        }
    }
    /* 容量必须由用户态查询后传入（内核里查会因缺 FileObject 崩溃）。
       优先按卷设备查，失败再按盘符查——SYSTEM 会话里前者经常失败，
       一旦传 0 驱动就退回 32GB 兜底，超出部分会静默不受保护。 */
    in.VolumeBytes = 0;
    if (vol != 0) {
        QueryVolumeSize(vol, &in.VolumeBytes);
    }
    if (in.VolumeBytes == 0) {
        in.VolumeBytes = QueryVolumeSizeByDrive(L'C');
    }
    if (in.VolumeBytes == 0) {
        Log(L"警告：无法确定受保护卷容量，映射表将由驱动按 32GB 兜底");
    }
    ok = DeviceIoControl(h, IOCTL_TGSHADOW_ENABLE, &in, sizeof(in), NULL, 0, &ret, NULL);
    if (!ok) {
        Log(L"启用失败：DeviceIoControl(ENABLE) 错误 %lu（卷 %lu, %lu MB）",
            GetLastError(), vol, mb);
    } else {
        Log(L"影子保护已启用：卷 %lu（0=自动识别）, 影子 %lu MB", vol, mb);
    }
    CloseHandle(h);
    return ok;
}

static BOOL DriverDisable(void)
{
    HANDLE h = OpenDevice();
    DWORD  ret = 0;
    BOOL   ok;

    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = DeviceIoControl(h, IOCTL_TGSHADOW_DISABLE, NULL, 0, NULL, 0, &ret, NULL);
    CloseHandle(h);
    return ok;
}

static BOOL DriverGetStatus(TGSHADOW_STATUS *st)
{
    HANDLE h = OpenDevice();
    DWORD  ret = 0;
    BOOL   ok;

    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(st, sizeof(*st));
    ok = DeviceIoControl(h, IOCTL_TGSHADOW_GET_STATUS, NULL, 0,
                         st, sizeof(*st), &ret, NULL);
    CloseHandle(h);
    return ok;
}

/**
 * 启用保护，并在"卷号自动识别"时按真实容量重建映射表。
 *
 * 为什么需要两步：会话 0 里用户态算不出 C: 的卷号（QueryDosDevice 拿不到映射），
 * 所以先让驱动用 \SystemRoot 自己解析（此时只能按 32GB 兜底建图）；拿回驱动
 * 解析出的卷号后，用户态就能查真实容量（\.\HarddiskVolumeN + IOCTL_DISK_GET_LENGTH_INFO），
 * 再重建一次映射表。否则【超过 32GB 的部分会静默地不受保护】。
 */
static BOOL DriverEnableAuto(DWORD vol, DWORD mb)
{
    TGSHADOW_STATUS st;
    ULONGLONG      bytes = 0;

    if (!DriverEnable(vol, mb)) {
        return FALSE;
    }
    if (vol != 0) {
        return TRUE;        /* 已给了卷号，一次到位 */
    }
    if (!DriverGetStatus(&st) || st.TargetVolumeNumber == 0) {
        Log(L"警告：无法取回驱动解析出的卷号，映射表按 32GB 兜底");
        return TRUE;
    }
    bytes = QueryVolumeSizeByDrive(L'C');            /* 受保护卷 = 系统卷 */
    if (bytes == 0) {
        bytes = 0;
        QueryVolumeSize(st.TargetVolumeNumber, &bytes);   /* 兜底：按卷设备查 */
    }
    if (bytes == 0) {
        Log(L"警告：查询受保护卷容量失败，映射表按 32GB 兜底", st.TargetVolumeNumber);
        return TRUE;
    }
    Log(L"驱动自动识别系统卷 = %lu（%llu MB），按其真实容量重建映射表",
        st.TargetVolumeNumber, (unsigned long long)(bytes / (1024ULL * 1024ULL)));
    DriverDisable();
    return DriverEnable(st.TargetVolumeNumber, mb);
}

static BOOL DriverCommit(DWORD *failedOut)
{
    HANDLE h = OpenDevice();
    DWORD  ret = 0;
    BOOL   ok;
    TGSHADOW_STATUS st;

    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = DeviceIoControl(h, IOCTL_TGSHADOW_COMMIT, NULL, 0, NULL, 0, &ret, NULL);
    if (ok && failedOut != NULL) {
        ZeroMemory(&st, sizeof(st));
        if (DeviceIoControl(h, IOCTL_TGSHADOW_GET_STATUS, NULL, 0,
                            &st, sizeof(st), &ret, NULL)) {
            *failedOut = st.LastCommitFailed;
        }
    }
    CloseHandle(h);
    return ok;
}

static BOOL DriverDiscard(void)
{
    HANDLE h = OpenDevice();
    DWORD  ret = 0;
    BOOL   ok;

    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = DeviceIoControl(h, IOCTL_TGSHADOW_DISCARD, NULL, 0, NULL, 0, &ret, NULL);
    CloseHandle(h);
    return ok;
}

/* --------------------------------------------------- 安全阀工具（启动自锁等） */

static BOOL IsSafeMode(void)
{
    return GetSystemMetrics(SM_CLEANBOOT) != 0;
}

static ULONGLONG UptimeSeconds(void)
{
    return GetTickCount64() / 1000ULL;
}

static DWORD ReadDword(const WCHAR *name, DWORD def)
{
    HKEY  key = NULL;
    DWORD val = def, size = sizeof(val), type = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CFG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return def;
    if (RegQueryValueExW(key, name, NULL, &type, (LPBYTE)&val, &size) != ERROR_SUCCESS)
        val = def;
    RegCloseKey(key);
    return val;
}

static void WriteDword(const WCHAR *name, DWORD val)
{
    HKEY key = NULL;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, CFG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                        &key, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
    RegCloseKey(key);
}

/* 是否有已登录的交互用户（没有用户就不该弹窗，也不该急着上保护） */
static BOOL HasInteractiveUser(void)
{
    PWTS_SESSION_INFOW info = NULL;
    DWORD count = 0, i;
    BOOL  found = FALSE;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count))
        return FALSE;
    for (i = 0; i < count; i++) {
        if (info[i].State == WTSActive) {
            found = TRUE;
            break;
        }
    }
    WTSFreeMemory(info);
    return found;
}

/* 保护跑满 HEALTHY_SECONDS 且期间没出问题，就算"健康的一轮"，把自锁计数清零 */
static DWORD WINAPI HealthMonitor(LPVOID param)
{
    DWORD i;

    UNREFERENCED_PARAMETER(param);
    for (i = 0; i < HEALTHY_SECONDS; i++) {
        if (WaitForSingleObject(g_StopEvent, 1000) == WAIT_OBJECT_0)
            return 0;
    }
    WriteDword(L"BootAttempts", 0);
    Log(L"保护已稳定运行 %d 秒，自锁计数清零（本轮安全）", HEALTHY_SECONDS);
    return 1;
}

/* ------------------------------------------------------------------ 配置 */

static void LoadConfig(void)
{
    HKEY  key = NULL;
    DWORD size, type, val;

    g_CfgVolume   = VolumeNumberFromDrive(L'C');   /* 0 = 驱动自动识别系统卷 */
    g_CfgShadowMB = 128;
    g_CfgTimeout  = 30;
    g_CfgDelaySec = 120;
    g_CfgEnabled  = 1;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CFG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        Log(L"未找到配置键，使用默认值（卷 %lu=自动, 影子 %lu MB, 延迟 %lu 秒）",
            g_CfgVolume, g_CfgShadowMB, g_CfgDelaySec);
        return;
    }
    size = sizeof(val);
    if (RegQueryValueExW(key, L"Enabled", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS)
        g_CfgEnabled = val;
    size = sizeof(val);
    if (RegQueryValueExW(key, L"VolumeNumber", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS)
        g_CfgVolume = val;
    size = sizeof(val);
    if (RegQueryValueExW(key, L"ShadowMB", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS)
        g_CfgShadowMB = val;
    size = sizeof(val);
    if (RegQueryValueExW(key, L"AskTimeoutSec", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS)
        g_CfgTimeout = val;
    size = sizeof(val);
    if (RegQueryValueExW(key, L"AutoEnableDelaySec", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS)
        g_CfgDelaySec = val;
    size = sizeof(g_CfgAskPath);
    RegQueryValueExW(key, L"AskExePath", NULL, &type, (LPBYTE)g_CfgAskPath, &size);
    size = sizeof(g_CfgShadowPath);
    if (RegQueryValueExW(key, L"ShadowPath", NULL, &type,
                         (LPBYTE)g_CfgShadowPath, &size) != ERROR_SUCCESS) {
        g_CfgShadowPath[0] = 0;
    }

    RegCloseKey(key);

    /* 影子容量不得超过物理内存的 25%：非分页池被吃光 = 系统假死。
       这是 2026-09-19 "启动失败进自动修复" 事故的直接教训之一。 */
    {
        MEMORYSTATUSEX ms;
        ULONGLONG limitMB;
        ZeroMemory(&ms, sizeof(ms));
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            limitMB = (ms.ullTotalPhys / (1024ULL * 1024ULL)) / 4;
            if (limitMB < 64) limitMB = 64;
            if ((ULONGLONG)g_CfgShadowMB > limitMB) {
                Log(L"影子容量 %lu MB 超过物理内存 25%%（上限 %llu MB），已下调",
                    g_CfgShadowMB, limitMB);
                g_CfgShadowMB = (DWORD)limitMB;
            }
        }
    }

    Log(L"配置：Enabled=%lu 卷=%lu 影子=%lu MB 询问超时=%lu 秒 延迟启用=%lu 秒",
        g_CfgEnabled, g_CfgVolume, g_CfgShadowMB, g_CfgTimeout, g_CfgDelaySec);
}

/* ------------------------------------------------------------------ 跨会话弹窗 */

/* 在活动控制台会话中启动 tgshadow_ask.exe 并等待结果。
   返回 TRUE 表示"保留修改"（用户输入了正确密码），其余一切情况都是 FALSE=还原。 */
static BOOL AskUserKeepChanges(void)
{
    DWORD  sessionId;
    HANDLE hToken = NULL, hDup = NULL;
    LPVOID env = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR  cmd[MAX_PATH * 2];
    WCHAR  askPath[MAX_PATH];
    WCHAR  resultPath[MAX_PATH];
    DWORD  wait;
    BOOL   keep = FALSE;
    HANDLE hResult;

    sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        Log(L"没有活动的控制台会话，默认还原");
        return FALSE;
    }
    if (g_CfgAskPath[0] != 0) {
        wcsncpy_s(askPath, _countof(askPath), g_CfgAskPath, _TRUNCATE);
    } else {
        GetModuleFileNameW(NULL, askPath, _countof(askPath));
        {
            WCHAR *slash = wcsrchr(askPath, L'\\');
            if (slash) *(slash + 1) = 0;
        }
        wcsncat_s(askPath, _countof(askPath), ASK_EXE_NAME, _TRUNCATE);
    }
    if (GetFileAttributesW(askPath) == INVALID_FILE_ATTRIBUTES) {
        Log(L"找不到询问程序，默认还原：%s", askPath);
        return FALSE;
    }
    _snwprintf_s(resultPath, _countof(resultPath), _TRUNCATE,
                 L"C:\\Windows\\Temp\\tgshadow_ask.result");
    DeleteFileW(resultPath);

    if (!WTSQueryUserToken(sessionId, &hToken)) {
        Log(L"WTSQueryUserToken 失败 %lu，默认还原", GetLastError());
        return FALSE;
    }
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                          TokenPrimary, &hDup)) {
        Log(L"DuplicateTokenEx 失败 %lu", GetLastError());
        CloseHandle(hToken);
        return FALSE;
    }
    CloseHandle(hToken);

    if (!CreateEnvironmentBlock(&env, hDup, FALSE)) {
        env = NULL;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"winsta0\\default";
    ZeroMemory(&pi, sizeof(pi));

    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE,
                 L"\"%s\" %lu \"%s\"", askPath, g_CfgTimeout, resultPath);

    if (!CreateProcessAsUserW(hDup, NULL, cmd, NULL, NULL, FALSE,
                              CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE,
                              env, NULL, &si, &pi)) {
        Log(L"CreateProcessAsUser 失败 %lu（命令行：%s）", GetLastError(), cmd);
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(hDup);
        return FALSE;
    }
    Log(L"已弹出确认窗口（会话 %lu，超时 %lu 秒）", sessionId, g_CfgTimeout);

    wait = WaitForSingleObject(pi.hProcess, (g_CfgTimeout + 30) * 1000);
    if (wait == WAIT_TIMEOUT) {
        Log(L"等待询问程序超时，终止它并默认还原");
        TerminateProcess(pi.hProcess, 0);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(hDup);

    hResult = CreateFileW(resultPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hResult != INVALID_HANDLE_VALUE) {
        char buf[32] = {0};
        DWORD got = 0;
        if (ReadFile(hResult, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
            if (_stricmp(buf, "keep") == 0) {
                keep = TRUE;
            }
        }
        CloseHandle(hResult);
    } else {
        Log(L"没有拿到结果文件（错误 %lu），默认还原", GetLastError());
    }
    Log(L"用户选择：%s", keep ? L"保留修改" : L"还原（丢弃）");
    return keep;
}

/* ------------------------------------------------------------------ 关机处理 */

static void HandleShutdownDecision(void)
{
    BOOL keep;
    DWORD failed = 0;

    Log(L"收到关机通知，开始询问是否保留本次修改");
    keep = AskUserKeepChanges();

    if (keep) {
        if (DriverCommit(&failed)) {
            Log(L"已提交改动到真实卷（失败块数 %lu）", failed);
        } else {
            Log(L"提交失败（错误 %lu）—— 保守起见不丢弃，交回系统处理", GetLastError());
        }
    } else {
        if (DriverDiscard()) {
            Log(L"已丢弃本次改动，重启后恢复原状");
        } else {
            Log(L"丢弃失败（错误 %lu）；影子不会持久化，重启后依然是原状", GetLastError());
        }
    }

    /* 走完一次完整的"启用 -> 关机决策"流程，说明这台机器扛得住，清掉自锁计数 */
    WriteDword(L"BootAttempts", 0);
}

/* ------------------------------------------------------------------ 服务主体 */

static DWORD WINAPI HandlerEx(DWORD control, DWORD eventType,
                              LPVOID eventData, LPVOID context)
{
    UNREFERENCED_PARAMETER(eventType);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(context);

    switch (control) {
    case SERVICE_CONTROL_PRESHUTDOWN:
    case SERVICE_CONTROL_SHUTDOWN:
        Log(L"服务控制：%s",
            control == SERVICE_CONTROL_PRESHUTDOWN ? L"PRESHUTDOWN" : L"SHUTDOWN");
        g_Status.dwCurrentState = SERVICE_STOP_PENDING;
        g_Status.dwWaitHint = (g_CfgTimeout + 40) * 1000;
        SetServiceStatus(g_StatusHandle, &g_Status);
        HandleShutdownDecision();
        g_Status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_StatusHandle, &g_Status);
        SetEvent(g_StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_STOP:
        Log(L"服务控制：STOP");
        g_Status.dwCurrentState = SERVICE_STOP_PENDING;
        g_Status.dwWaitHint = 10000;
        SetServiceStatus(g_StatusHandle, &g_Status);
        DriverDiscard();       /* 服务被停 = 不再保护，改动一律丢弃（默认安全侧） */
        SetEvent(g_StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_StatusHandle, &g_Status);
        return NO_ERROR;

    case 128:
        /* 自定义控制码：不做关机，只走一遍"询问+提交/丢弃"流程。
           必须以 SYSTEM 身份运行才能跨会话弹窗，所以测试时用
               sc.exe control TgShadowSvc 128
           而不是直接跑 testask（普通管理员没有 SE_TCB，WTSQueryUserToken 会失败）。 */
        Log(L"服务控制：128 = 测试一次询问流程");
        HandleShutdownDecision();
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_StatusHandle = RegisterServiceCtrlHandlerExW(SVC_NAME, HandlerEx, NULL);
    if (g_StatusHandle == NULL) {
        return;
    }
    ZeroMemory(&g_Status, sizeof(g_Status));
    g_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_Status.dwCurrentState = SERVICE_START_PENDING;
    g_Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                  SERVICE_ACCEPT_PRESHUTDOWN;
    g_Status.dwWaitHint = 30000;
    SetServiceStatus(g_StatusHandle, &g_Status);

    Log(L"服务启动");
    LoadConfig();

    if (!g_CfgEnabled) {
        Log(L"配置为 Enabled=0，不启用保护");
    } else if (IsSafeMode()) {
        Log(L"当前是安全模式，跳过保护（安全模式下绝不能挂影子）");
    } else {
        DWORD attempts = ReadDword(L"BootAttempts", 0);

        if (attempts >= 3) {
            /* 自锁：连续多轮"启用后系统没跑满健康时长"，说明当前配置会拖垮这台机器。
               继续自动启用只会把它锁死在修复循环里，必须人工介入。 */
            Log(L"已连续 %lu 轮启用后系统异常（未跑满 %d 秒就重启），"
                L"自动启用已自锁。排查后删除注册表 %s 下的 BootAttempts 可解锁。",
                attempts, HEALTHY_SECONDS, CFG_KEY);
        } else {
            /* 延迟启用：等系统真正起来 + 有用户登录。
               开机瞬间 I/O 最密集、内存最紧张，这时挂影子是最危险的做法。 */
            Log(L"等待系统稳定：目标开机 %lu 秒后启用（当前 %llu 秒）",
                g_CfgDelaySec, UptimeSeconds());
            while (UptimeSeconds() < g_CfgDelaySec) {
                if (WaitForSingleObject(g_StopEvent, 1000) == WAIT_OBJECT_0) {
                    Log(L"等待期间收到停止信号，放弃启用");
                    goto skip_enable;
                }
            }
            for (;;) {
                if (HasInteractiveUser()) break;
                if (WaitForSingleObject(g_StopEvent, 2000) == WAIT_OBJECT_0) {
                    Log(L"等待用户登录期间收到停止信号，放弃启用");
                    goto skip_enable;
                }
            }

            WriteDword(L"BootAttempts", attempts + 1);
            Log(L"开始启用保护（第 %lu 轮尝试）", attempts + 1);
            if (DriverEnableAuto(g_CfgVolume, g_CfgShadowMB)) {
                if (g_HealthThread == NULL) {
                    g_HealthThread = CreateThread(NULL, 0, HealthMonitor, NULL, 0, NULL);
                }
            } else {
                Log(L"启用失败，本轮不重试（避免反复触发问题）");
            }
        }
    }
skip_enable:

    g_Status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_Status);

    WaitForSingleObject(g_StopEvent, INFINITE);

    g_Status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_Status);
    Log(L"服务已停止");
}

/* ------------------------------------------------------------------ 部署命令 */

static int InstallService(void)
{
    WCHAR path[MAX_PATH];
    SC_HANDLE scm, svc;
    SERVICE_DESCRIPTIONW desc;
    SERVICE_DELAYED_AUTO_START_INFO delayed;

    GetModuleFileNameW(NULL, path, _countof(path));
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (scm == NULL) {
        wprintf(L"打开服务管理器失败 %lu（需要管理员权限）\n", GetLastError());
        return 1;
    }
    svc = CreateServiceW(scm, SVC_NAME, SVC_DISPLAY,
                         SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                         SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                         path, NULL, NULL, NULL, NULL, NULL);
    if (svc == NULL) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
            if (svc != NULL) {
                ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                     SERVICE_NO_CHANGE, path, NULL, NULL, NULL,
                                     NULL, NULL, SVC_DISPLAY);
                wprintf(L"服务已存在，已更新路径。\n");
            }
        }
        if (svc == NULL) {
            wprintf(L"创建服务失败 %lu\n", GetLastError());
            CloseServiceHandle(scm);
            return 1;
        }
    } else {
        wprintf(L"服务已创建。\n");
    }

    desc.lpDescription = L"TimeGuard 磁盘影子保护：关机时询问是否保留本次修改，默认还原。";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    delayed.fDelayedAutostart = FALSE;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);

    {
        HKEY key;
        DWORD timeout = 120000;   /* 预关机处理超时（毫秒）：给足弹窗时间 */
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                            L"SYSTEM\\CurrentControlSet\\Control", 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                            &key, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(key, L"PreshutdownTimeout", 0, REG_DWORD,
                           (const BYTE *)&timeout, sizeof(timeout));
            RegCloseKey(key);
            wprintf(L"已设置 PreshutdownTimeout = 120000 ms\n");
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    wprintf(L"完成。启动：sc.exe start %s\n", SVC_NAME);
    return 0;
}

static int UninstallService(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) return 1;
    svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc == NULL) {
        wprintf(L"服务不存在。\n");
        CloseServiceHandle(scm);
        return 1;
    }
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    if (DeleteService(svc)) {
        wprintf(L"服务已删除。\n");
    } else {
        wprintf(L"删除失败 %lu\n", GetLastError());
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

/* 设置家长密码：只把 SHA-256 哈希写进注册表，明文不落盘 */
static int SetPassword(const WCHAR *pw)
{
    HKEY  key = NULL;
    WCHAR hash[128] = L"";
    DWORD disp = 0;
    LONG  rc;

    if (pw == NULL || pw[0] == 0) {
        wprintf(L"用法: tgshadow_svc setpass <密码>\n");
        return 2;
    }
    if (!TgSha256HexOfPassword(pw, hash, _countof(hash))) {
        wprintf(L"计算哈希失败\n");
        return 1;
    }
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, CFG_KEY, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, &disp);
    if (rc != ERROR_SUCCESS) {
        wprintf(L"打开注册表失败 %ld（需要管理员权限）\n", rc);
        return 1;
    }
    rc = RegSetValueExW(key, L"ParentPasswordHash", 0, REG_SZ,
                        (const BYTE *)hash, (DWORD)((wcslen(hash) + 1) * sizeof(WCHAR)));
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        wprintf(L"写入失败 %ld\n", rc);
        return 1;
    }
    wprintf(L"家长密码已设置（SHA-256: %s）\n", hash);
    return 0;
}

/* 交互式调试：等同于服务启动后的行为，回车触发一次询问流程，Ctrl+C 退出 */
static int ConsoleRun(void)
{
    wprintf(L"[console] 加载配置并启用保护...\n");
    LoadConfig();
    if (g_CfgEnabled) {
        DriverEnableAuto(g_CfgVolume, g_CfgShadowMB);
    }
    wprintf(L"[console] 运行中。回车执行一次【关机询问】流程做测试，Ctrl+C 退出。\n");
    for (;;) {
        if (getchar() == EOF) break;
        wprintf(L"[console] 触发询问流程...\n");
        HandleShutdownDecision();
        wprintf(L"[console] 完成。回车再来一次，Ctrl+C 退出。\n");
    }
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)SVC_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };

    if (argc >= 2) {
        if (_wcsicmp(argv[1], L"install") == 0)   return InstallService();
        if (_wcsicmp(argv[1], L"uninstall") == 0) return UninstallService();
        if (_wcsicmp(argv[1], L"console") == 0)   return ConsoleRun();
        if (_wcsicmp(argv[1], L"setpass") == 0)   return SetPassword(argc >= 3 ? argv[2] : NULL);
        if (_wcsicmp(argv[1], L"ask128") == 0) {
            SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
            SC_HANDLE svc;
            SERVICE_STATUS st;
            if (scm == NULL) { wprintf(L"打开 SCM 失败 %lu\n", GetLastError()); return 1; }
            svc = OpenServiceW(scm, SVC_NAME, SERVICE_USER_DEFINED_CONTROL);
            if (svc == NULL) {
                wprintf(L"打开服务失败 %lu\n", GetLastError());
                CloseServiceHandle(scm);
                return 1;
            }
            if (ControlService(svc, 128, &st)) {
                wprintf(L"已触发询问流程（结果见 C:\\Windows\\Temp\\tgshadow_svc.log）\n");
            } else {
                wprintf(L"触发失败 %lu\n", GetLastError());
            }
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return 0;
        }
        if (_wcsicmp(argv[1], L"testask") == 0) {
            BOOL keep;
            LoadConfig();
            keep = AskUserKeepChanges();
            wprintf(L"结果：%s\n", keep ? L"保留" : L"还原");
            return keep ? 0 : 1;
        }
        wprintf(L"用法: tgshadow_svc [install|uninstall|console|ask128|testask|setpass <密码>]\n"
                L"  不带参数 = 作为服务运行（由 SCM 调用）\n");
        return 2;
    }
    if (!StartServiceCtrlDispatcherW(table)) {
        wprintf(L"作为服务启动失败 %lu；用 'console' 子命令可在前台调试。\n", GetLastError());
        return 1;
    }
    return 0;
}

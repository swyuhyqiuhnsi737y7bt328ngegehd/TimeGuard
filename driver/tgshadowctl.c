/*==============================================================================
  tgshadowctl.c - tgshadow 驱动的用户态控制/测试工具（P1）

  用法:
    tgshadowctl version              查询驱动版本
    tgshadowctl status               查询保护状态与 I/O 计数
    tgshadowctl volumes              枚举系统中存在的卷
    tgshadowctl enable <卷号> [MB]   启用对指定卷的影子保护（P1 仅挂载过滤）
    tgshadowctl disable              停用保护并卸载过滤

  构建: driver\build_ctl.bat   （产物 build\Debug\tgshadowctl.exe）
  需要在虚拟机内以【管理员】运行（打开 \.TgShadow 设备）。
==============================================================================*/
#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include "../tgshadow/tgshadow.h"

#pragma comment(lib, "advapi32.lib")

/**
 * 设备打不开时的诊断：查询 tgshadow 服务状态并给出可执行的下一步。
 * 常见原因：demand 启动的驱动在系统重启后不会自动加载。
 */
static void DiagnoseDriverService(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    const char *stateText = "未知";

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        fprintf(stderr, "       诊断: 无法查询服务管理器 (GetLastError=%lu)\n",
                GetLastError());
        return;
    }
    svc = OpenServiceW(scm, L"tgshadow", SERVICE_QUERY_STATUS);
    if (svc == NULL) {
        fprintf(stderr,
                "       诊断: 服务 tgshadow 【未安装】\n"
                "       处理: 运行 vm_test.bat（自动签名+加载），或手动执行\n"
                "             sc.exe create tgshadow type= kernel start= demand error= normal binPath= C:\\tg\\tgshadow.sys\n"
                "             sc.exe start tgshadow\n");
        CloseServiceHandle(scm);
        return;
    }
    if (QueryServiceStatus(svc, &st)) {
        switch (st.dwCurrentState) {
        case SERVICE_STOPPED:       stateText = "【已停止】"; break;
        case SERVICE_RUNNING:       stateText = "运行中（但设备未创建，可能 DriverEntry 失败）"; break;
        case SERVICE_START_PENDING: stateText = "启动中"; break;
        case SERVICE_STOP_PENDING:  stateText = "停止中"; break;
        case SERVICE_PAUSED:        stateText = "已暂停"; break;
        default: break;
        }
        fprintf(stderr, "       诊断: 服务状态 = %s\n", stateText);
        if (st.dwCurrentState == SERVICE_STOPPED) {
            fprintf(stderr,
                    "       说明: 本驱动是 demand(按需) 启动，系统重启后不会自动加载\n"
                    "       处理: 执行  sc.exe start tgshadow\n");
        } else if (st.dwCurrentState == SERVICE_RUNNING) {
            fprintf(stderr,
                    "       提示: 服务在跑但符号链接不存在，检查内核日志 [TgShadow] 前缀\n");
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

static HANDLE OpenDevice(void)
{
    HANDLE h = CreateFileW(TGSHADOW_WIN32_DEVICE,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr, "[错误] 无法打开 %ws (GetLastError=%lu)\n",
                TGSHADOW_WIN32_DEVICE, err);
        if (err == ERROR_ACCESS_DENIED) {
            fprintf(stderr, "       诊断: 访问被拒绝 —— 请以【管理员】身份运行\n");
        } else {
            DiagnoseDriverService();
        }
    }
    return h;
}

static int DoIoctl(HANDLE h, DWORD code, void *in, DWORD inLen,
                   void *out, DWORD outLen, DWORD *ret)
{
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, code, in, inLen, out, outLen, &bytes, NULL);
    if (ret) *ret = bytes;
    if (!ok) {
        fprintf(stderr, "[错误] DeviceIoControl(0x%08lX) 失败, GetLastError=%lu\n",
                code, GetLastError());
        return -1;
    }
    return 0;
}

static int CmdVersion(HANDLE h)
{
    TGSHADOW_STATUS st;
    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_GET_VERSION, NULL, 0u, &st, (DWORD)sizeof(st), NULL) != 0)
        return 1;
    printf("驱动版本: %u.%u\n", st.VersionMajor, st.VersionMinor);
    return 0;
}

static int CmdStatus(HANDLE h)
{
    TGSHADOW_STATUS st;
    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_GET_STATUS, NULL, 0u, &st, (DWORD)sizeof(st), NULL) != 0)
        return 1;
    printf("版本          : %u.%u\n", st.VersionMajor, st.VersionMinor);
    printf("影子保护      : %s\n", st.Protected ? "已启用" : "未启用");
    printf("卷过滤挂载    : %s\n", st.FilterAttached ? "已挂载" : "未挂载");
    printf("受保护卷号    : %lu\n", st.TargetVolumeNumber);
    printf("影子容量      : %llu 字节 (%.1f MB)\n", st.ShadowBytesTotal,
           (double)st.ShadowBytesTotal / (1024.0 * 1024.0));
    printf("影子已用      : %llu 字节\n", st.ShadowBytesUsed);
    printf("读 I/O 计数   : %llu\n", st.ReadCount);
    printf("写 I/O 计数   : %llu\n", st.WriteCount);
    printf("已重定向块数  : %llu\n", st.RedirectedBlocks);
    return 0;
}

static int CmdVolumes(HANDLE h)
{
    TGSHADOW_VOLUME_INFO vols[32];
    DWORD bytes = 0;
    ULONG count, i;

    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_GET_VOLUMES, NULL, 0u,
                vols, (DWORD)sizeof(vols), &bytes) != 0)
        return 1;

    count = bytes / sizeof(TGSHADOW_VOLUME_INFO);
    printf("发现 %lu 个卷设备:\n", count);
    for (i = 0; i < count; i++) {
        printf("  HarddiskVolume%-3lu %s\n", vols[i].VolumeNumber,
               vols[i].IsTarget ? "  <== 当前受保护" : "");
    }
    return 0;
}

static int CmdEnable(HANDLE h, ULONG volume, ULONG mb)
{
    TGSHADOW_ENABLE_INPUT in;
    DWORD err;

    in.VolumeNumber = volume;
    in.Flags = 0;
    in.ShadowBytes = (ULONG64)mb * 1024ULL * 1024ULL;

    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_ENABLE, &in, (DWORD)sizeof(in), NULL, 0u, NULL) != 0) {
        err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            fprintf(stderr,
                    "提示: 卷号 %lu 不存在（驱动无法打开 \\Device\\HarddiskVolume%lu）。\n"
                    "      先运行 'tgshadowctl volumes' 查看可用卷号；\n"
                    "      再确认系统盘卷号：PowerShell 执行\n"
                    "        [Text.Encoding]::Unicode.GetString((Get-ItemProperty\n"
                    "          'HKLM:\\SYSTEM\\MountedDevices').'\\DosDevices\\C:')\n",
                    volume, volume);
        } else if (err == ERROR_ACCESS_DENIED) {
            fprintf(stderr, "提示: 访问被拒绝 —— 请以【管理员】身份运行。\n");
        } else if (err == ERROR_ALREADY_ASSIGNED || err == 183) {
            fprintf(stderr, "提示: 该卷已处于保护中，先执行 disable。\n");
        }
        return 1;
    }
    printf("已启用保护: 卷 %lu, 影子容量 %lu MB\n", volume, mb);
    return 0;
}

static int CmdDisable(HANDLE h)
{
    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_DISABLE, NULL, 0u, NULL, 0u, NULL) != 0)
        return 1;
    printf("已停用保护并卸载卷过滤\n");
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    HANDLE h;
    int rc = 0;

    /* UTF-8 console output: this source file is UTF-8, but the Windows console
       defaults to codepage 936 (GBK), which garbles Chinese output. Switch the
       console output codepage to UTF-8 for this process. */
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        wprintf(L"用法: tgshadowctl <version|status|volumes|enable <卷号> [MB]|disable>\n");
        return 2;
    }

    h = OpenDevice();
    if (h == INVALID_HANDLE_VALUE)
        return 1;

    if (_wcsicmp(argv[1], L"version") == 0) {
        rc = CmdVersion(h);
    } else if (_wcsicmp(argv[1], L"status") == 0) {
        rc = CmdStatus(h);
    } else if (_wcsicmp(argv[1], L"volumes") == 0) {
        rc = CmdVolumes(h);
    } else if (_wcsicmp(argv[1], L"enable") == 0) {
        ULONG vol = (argc >= 3) ? (ULONG)_wtoi(argv[2]) : 3;
        ULONG mb = (argc >= 4) ? (ULONG)_wtoi(argv[3]) : 32768;
        rc = CmdEnable(h, vol, mb);
    } else if (_wcsicmp(argv[1], L"disable") == 0) {
        rc = CmdDisable(h);
    } else {
        wprintf(L"未知命令: %ws\n", argv[1]);
        rc = 2;
    }

    CloseHandle(h);
    return rc;
}

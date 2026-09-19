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
#include <winioctl.h>
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
                    "       处理: 执行  sc.exe start tgshadow\n"
                    "       若 start 报 577(数字签名无法验证)：说明 .sys 被替换过但没重新签名，\n"
                    "             运行 sign.bat 重新签名（或 vm_test.bat 一键完成）\n");
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
    printf("--- P3 诊断（COW 分支）---\n");
    printf("PASSIVE 写 I/O: %llu\n", st.WritesPassive);
    printf("高 IRQL 写 I/O: %llu\n", st.WritesHighIrql);
    printf("进入 COW 判定 : %llu\n", st.CowEntered);
    printf("取不到缓冲区  : %llu\n", st.CowNoBuffer);
    printf("写重定向成功  : %llu\n", st.CowWriteOk);
    printf("读影子命中    : %llu\n", st.CowReadOk);
    printf("COW 失败透传  : %llu", st.CowFailed);
    if (st.CowFailed > 0) {
        printf("  (最后状态 0x%08X)", st.CowLastStatus);
    }
    printf("\n");
    printf("--- P4 提交/丢弃 ---\n");
    printf("累计提交块数  : %u\n", st.CommittedBlocks);
    printf("累计丢弃块数  : %u\n", st.DiscardedBlocks);
    printf("上次提交失败  : %u\n", st.LastCommitFailed);
    printf("分页 I/O 透传 : %u  (安全阀：页面文件/映射文件不参与 COW)\n", st.PagingIo);
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

/**
 * 查询卷容量（字节）。在用户态完成，再通过 IOCTL_ENABLE 传给驱动。
 * 原因：内核里用 IoBuildDeviceIoControlRequest 向卷设备发同步 IRP 时，
 * IRP 缺少 FileObject，卷/磁盘驱动解引用它会导致 0x3B + 0xC0000005 蓝屏。
 */
static int QueryVolumeSize(ULONG volNum, ULONG64 *bytes)
{
    WCHAR path[64];
    HANDLE hv;
    GET_LENGTH_INFORMATION info;
    DWORD ret = 0;

    swprintf_s(path, 64, L"\\\\.\\\\HarddiskVolume%lu", volNum);
    /* 必须请求读取权限：用 0 访问权限调 IOCTL_DISK_GET_LENGTH_INFO 会返回
       ERROR_ACCESS_DENIED(5)（实测），导致驱动只能按 32GB 兜底建位图。 */
    hv = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, 0, NULL);
    if (hv == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[警告] 无法打开 %ws 查询容量 (GetLastError=%lu)\n",
                path, GetLastError());
        return -1;
    }
    if (!DeviceIoControl(hv, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                         &info, sizeof(info), &ret, NULL)) {
        fprintf(stderr, "[警告] 查询卷容量失败 (GetLastError=%lu)\n", GetLastError());
        CloseHandle(hv);
        return -1;
    }
    CloseHandle(hv);
    *bytes = (ULONG64)info.Length.QuadPart;
    return 0;
}

/* 创建并预分配影子文件（已存在则按需扩充到指定大小） */
static int EnsureShadowFile(const WCHAR *path, ULONG64 bytes)
{
    HANDLE        h;
    LARGE_INTEGER sz;
    LARGE_INTEGER cur;

    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "无法创建影子文件 %ws (GetLastError=%lu)\n", path, GetLastError());
        return -1;
    }
    ZeroMemory(&cur, sizeof(cur));
    if (GetFileSizeEx(h, &cur) && (ULONG64)cur.QuadPart >= bytes) {
        CloseHandle(h);
        return 0;                       /* 已经够大 */
    }
    sz.QuadPart = (LONGLONG)bytes;
    if (!SetFilePointerEx(h, sz, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
        fprintf(stderr, "预分配影子文件失败 (GetLastError=%lu)\n", GetLastError());
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    printf("影子文件已就绪: %ws (%llu MB)\n", path, (unsigned long long)(bytes >> 20));
    return 0;
}

static int CmdEnable(HANDLE h, ULONG volume, ULONG mb, int attachOnly)
{
    TGSHADOW_ENABLE_INPUT in;
    DWORD err;

    ZeroMemory(&in, sizeof(in));   /* ShadowPath 等字段必须清零，否则传栈垃圾给驱动 */
    in.VolumeNumber = volume;
    in.Flags = attachOnly ? TGSHADOW_FLAG_ATTACH_ONLY : 0;
    in.ShadowBytes = (ULONG64)mb * 1024ULL * 1024ULL;
    in.VolumeBytes = 0;
    if (QueryVolumeSize(volume, &in.VolumeBytes) == 0) {
        printf("卷 %lu 容量: %llu MB\n", volume,
               (unsigned long long)(in.VolumeBytes / (1024ULL * 1024ULL)));
    } else {
        fprintf(stderr, "       查询失败，将由驱动按 32GB 兜底容量建图\n");
        in.VolumeBytes = 0;
    }

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
    printf("已启用保护: 卷 %lu, 影子容量 %lu MB%s\n", volume, mb,
           attachOnly ? "  [attach-only：只挂载，不记录写]" : "");

    /* 卷号给 0 时驱动会用 \SystemRoot 自动识别系统卷，但那时只能按 32GB 兜底建图。
       这里取回真实卷号后按真实容量重建一次，避免"超出部分静默不受保护"。 */
    if (volume == 0) {
        TGSHADOW_STATUS st;
        ULONG64 bytes = 0;
        DWORD got = 0;

        ZeroMemory(&st, sizeof(st));
        if (DeviceIoControl(h, (DWORD)IOCTL_TGSHADOW_GET_STATUS, NULL, 0u,
                            &st, (DWORD)sizeof(st), &got, NULL) &&
            st.TargetVolumeNumber != 0 &&
            QueryVolumeSize(st.TargetVolumeNumber, &bytes) == 0 && bytes > 0) {
            printf("自动识别系统卷 = %lu（%llu MB），按真实容量重建映射表...\n",
                   st.TargetVolumeNumber, (unsigned long long)(bytes / (1024ULL * 1024ULL)));
            DoIoctl(h, (DWORD)IOCTL_TGSHADOW_DISABLE, NULL, 0u, NULL, 0u, NULL);
            return CmdEnable(h, st.TargetVolumeNumber, mb, attachOnly);
        }
        printf("提示: 未能取回自动识别的卷号，映射表按 32GB 兜底\n");
    }
    return 0;
}

/**
 * enabledisk <卷号> <MB> <影子文件路径>
 *
 * 用【磁盘影子】启用保护：影子是另一块卷上的预分配文件，容量不受内存限制，
 * 而且重启后依然存在（P5 的崩溃一致性要靠它）。
 * 影子文件所在卷会被驱动自动排除在 COW 之外（否则写影子会递归触发 COW）。
 */
static int CmdEnableDisk(HANDLE h, ULONG volume, ULONG mb, const WCHAR *path)
{
    TGSHADOW_ENABLE_INPUT in;
    WCHAR  ntPath[300];
    DWORD  err;
    ULONG64 shadowBytes = (ULONG64)mb * 1024ULL * 1024ULL;
    ULONG64 volBytes = 0;
    size_t i;

    if (path == NULL || path[0] == 0) {
        fprintf(stderr, "用法: tgshadowctl enabledisk <卷号> <MB> <影子文件路径>\n");
        return 2;
    }
    /* 按影子文件所在卷的可用空间自动缩容（别一上来就 ERROR_DISK_FULL） */
    {
        WCHAR root[4];
        ULARGE_INTEGER avail, total, totalFree;

        root[0] = path[0];
        root[1] = L':';
        root[2] = L'\\';
        root[3] = 0;
        if (GetDiskFreeSpaceExW(root, &avail, &total, &totalFree)) {
            ULONG64 usable = (ULONG64)avail.QuadPart;
            usable = usable / 10 * 9;              /* 留 10% 余量 */
            if (shadowBytes > usable) {
                printf("影子容量 %lu MB 超过 %ws 可用空间，自动调整为 %llu MB\n",
                       mb, root, (unsigned long long)(usable >> 20));
                shadowBytes = usable;
            }
        }
    }
    if (EnsureShadowFile(path, shadowBytes) != 0) {
        return 1;
    }

    ZeroMemory(&in, sizeof(in));
    in.VolumeNumber = volume;
    in.ShadowBytes = shadowBytes;
    if (volume != 0 && QueryVolumeSize(volume, &volBytes) == 0) {
        in.VolumeBytes = volBytes;
    }
    /* DOS 路径 -> NT 路径：S:\a.bin -> \??\S:\a.bin */
    _snwprintf_s(ntPath, _countof(ntPath), _TRUNCATE, L"\\??\\%ws", path);
    for (i = 0; i + 1 < _countof(in.ShadowPath) && ntPath[i] != 0; i++) {
        in.ShadowPath[i] = ntPath[i];
    }
    in.ShadowPath[i] = 0;
    printf("影子路径(NT): %ws\n", in.ShadowPath);

    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_ENABLE, &in, (DWORD)sizeof(in), NULL, 0u, NULL) != 0) {
        err = GetLastError();
        fprintf(stderr, "启用失败 GetLastError=%lu\n", err);
        if (err == ERROR_NOT_READY) {
            fprintf(stderr, "提示: 设备未就绪 = 驱动找不到目标卷或找不到影子卷的过滤设备。\n");
        } else if (err == ERROR_INVALID_PARAMETER) {
            fprintf(stderr, "提示: 影子文件不能放在被保护的卷上。\n");
        } else if (err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_DISK_FULL) {
            fprintf(stderr, "提示: 影子容量不足（检查文件是否真的预分配成功）。\n");
        }
        return 1;
    }
    printf("已启用磁盘影子保护: 卷 %lu, 影子 %lu MB @ %ws\n", volume, mb, path);
    CmdStatus(h);
    return 0;
}

/* 提交：把影子数据写回真实卷（"保留本次修改"）。操作有风险，提示确认。 */
static int CmdCommit(HANDLE h, int assumeYes)
{
    DWORD done = 0;

    if (!assumeYes) {
        printf("提交会把影子里的全部改动写回真实卷，此后重启不再还原。\n");
        printf("输入 yes 继续: ");
        {
            char line[16];
            if (fgets(line, sizeof(line), stdin) == NULL || strncmp(line, "yes", 3) != 0) {
                printf("已取消。\n");
                return 1;
            }
        }
    }
    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_COMMIT, NULL, 0u, NULL, 0u, &done) != 0) {
        fprintf(stderr, "提交失败: %lu\n", GetLastError());
        return 1;
    }
    printf("提交完成（保护已停止）。\n");
    CmdStatus(h);
    return 0;
}

/* 丢弃：放弃本次全部改动，重启即还原 */
static int CmdDiscard(HANDLE h)
{
    DWORD dropped = 0;

    if (DoIoctl(h, (DWORD)IOCTL_TGSHADOW_DISCARD, NULL, 0u, NULL, 0u, &dropped) != 0) {
        fprintf(stderr, "丢弃失败: %lu\n", GetLastError());
        return 1;
    }
    printf("丢弃完成（保护已停止，真实卷未被修改，重启后即为原状）。\n");
    CmdStatus(h);
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
        wprintf(L"用法: tgshadowctl <命令>\n"
                L"  version / status / volumes\n"
                L"  enable <卷号> [MB] [attachonly]   启用内存影子（容量受内存限制）\n"
                L"  enabledisk <卷号> <MB> <路径>     启用磁盘影子（推荐，如 enabledisk 0 4096 S:\\tgshadow.bin）\n"
                L"  disable                           停用保护并卸载过滤\n"
                L"  commit [yes]                      提交改动回真实卷（保留）\n"
                L"  discard                           丢弃全部改动（还原）\n");
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
        int attachOnly = (argc >= 5 &&
                          _wcsicmp(argv[4], L"attachonly") == 0) ? 1 : 0;
        rc = CmdEnable(h, vol, mb, attachOnly);
    } else if (_wcsicmp(argv[1], L"disable") == 0) {
        rc = CmdDisable(h);
    } else if (_wcsicmp(argv[1], L"enabledisk") == 0) {
        ULONG vol = (argc >= 3) ? (ULONG)_wtoi(argv[2]) : 0;
        ULONG mb  = (argc >= 4) ? (ULONG)_wtoi(argv[3]) : 4096;
        rc = CmdEnableDisk(h, vol, mb, (argc >= 5) ? argv[4] : NULL);
    } else if (_wcsicmp(argv[1], L"commit") == 0) {
        rc = CmdCommit(h, argc >= 3 && _wcsicmp(argv[2], L"yes") == 0);
    } else if (_wcsicmp(argv[1], L"discard") == 0) {
        rc = CmdDiscard(h);
    } else {
        wprintf(L"未知命令: %ws\n", argv[1]);
        rc = 2;
    }

    CloseHandle(h);
    return rc;
}

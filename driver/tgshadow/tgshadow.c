/*==============================================================================
  tgshadow.c - TimeGuard 磁盘影子（重启还原）驱动 —— P1 骨架
  ----------------------------------------------------------------------------
  P1 目标（验收：VM 内可加载/卸载/枚举卷/挂载过滤，重启 100 次无异常）：
    * 创建控制设备 \\Device\\TgShadow 与符号链接 \\??\\TgShadow
    * 提供 IOCTL：版本 / 状态 / 卷枚举 / 启用 / 停用 / 提交 / 丢弃
    * 支持把过滤设备挂载到指定卷（当前仅直通 + 计数，不改动任何数据）
    * 干净的卸载路径（detach 全部 → 删除符号链接与设备）

  P2 起才会真正拦截写 I/O 并做写时复制重定向。

  构建：msbuild tgshadow.vcxproj /p:Configuration=Debug /p:Platform=x64
  调试：VM 内 testsigning 开启 + 自签名；双机内核调试见 driver/README.md
==============================================================================*/
#include <ntddk.h>
#include <ntdddisk.h>   /* IOCTL_DISK_GET_LENGTH_INFO / GET_LENGTH_INFORMATION */
#include "tgshadow.h"

#define TGSHADOW_TAG  'hSgT'   /* 'TgSh' 反写，池标记 */

/* ------------------------------------------------------------------ 全局状态 */

typedef struct _TGSHADOW_GLOBAL {
    PDEVICE_OBJECT  ControlDevice;      /* 控制设备（IOCTL 入口） */
    PDEVICE_OBJECT  FilterDevice;       /* 卷过滤设备（挂载在目标卷之上） */
    PDEVICE_OBJECT  LowerDevice;        /* 被挂载卷的设备对象 */
    UNICODE_STRING  LinkName;           /* 符号链接名 */

    ULONG           VersionMajor;
    ULONG           VersionMinor;
    ULONG           Protected;          /* 1 = 已启用影子保护 */
    ULONG           TargetVolumeNumber; /* 受保护卷号 */
    ULONG64         ShadowBytesTotal;
    ULONG64         ShadowBytesUsed;
    ULONG64         ReadCount;
    ULONG64         WriteCount;
    ULONG64         RedirectedBlocks;
    KSPIN_LOCK      Lock;

    /* ---- P2：影子映射元数据（位图记录哪些块被写过） ---- */
    RTL_BITMAP      ShadowBitmap;       /* 已标记块位图 */
    PVOID           ShadowBitmapBuffer; /* 位图缓冲（非分页池） */
    ULONG64         VolumeBytes;        /* 受保护卷容量 */
    ULONG64         BlockCount;         /* 总块数 = VolumeBytes / BLOCK_SIZE */
    ULONG           MarkWrites;         /* 1 = 记录写 I/O（attachonly 模式为 0） */
} TGSHADOW_GLOBAL, *PTGSHADOW_GLOBAL;

static TGSHADOW_GLOBAL g_TgShadow;

/* ------------------------------------------------------------------ 崩溃定位辅助 */

/**
 * 把当前执行到的关键步骤写进注册表：
 *   HKLM\SOFTWARE\TimeGuard\TgShadowLastStep
 *
 * 用途：蓝屏重启后无需 DbgView / 双机调试，一条命令即可看到最后一步：
 *   reg query "HKLM\SOFTWARE\TimeGuard" /v TgShadowLastStep
 *
 * 只能在 PASSIVE_LEVEL 调用（enable/disable 的 IOCTL 路径满足）。
 * 绝不可在读写 I/O 路径调用 —— ZwSetValueKey 在 DISPATCH_LEVEL 会崩溃。
 */
static VOID
TgShadowTrace(_In_ PCWSTR Step)
{
    HANDLE            key = NULL;
    UNICODE_STRING    keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS          status;

    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\SOFTWARE\\TimeGuard");
    InitializeObjectAttributes(&oa, &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL,
                         REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(status)) {
        return;
    }
    RtlInitUnicodeString(&valueName, L"TgShadowLastStep");
    {
        /* 手算长度：不依赖 CRT 的 wcslen（驱动里链接 CRT 函数有隐患） */
        ULONG len = 0;
        while (len < 200 && Step[len] != L'\0') {
            len++;
        }
#pragma warning(push)
#pragma warning(disable: 4996)
        ZwSetValueKey(key, &valueName, 0, REG_SZ, (PVOID)Step,
                      (ULONG)((len + 1) * sizeof(WCHAR)));
#pragma warning(pop)
    }
    ZwClose(key);
}

/* ------------------------------------------------------------------ 影子映射（P2） */

/* 注：卷容量改由用户态查询后经 IOCTL_ENABLE 传入（见 tgshadow.h 说明）。
   内核内用 IoBuildDeviceIoControlRequest 向卷设备发同步 IRP 会因缺少 FileObject
   被卷/磁盘驱动解引用而崩溃(0x3B + 0xC0000005)，实测确认，故移除。 */

/**
 * 为受保护卷分配影子位图。位图 1 位 = 1 个 64KB 块。
 * 例：100GB 卷 → 1.6M 块 → 200KB 非分页池，可接受。
 */
static NTSTATUS
TgShadowAllocMap(_In_ ULONG64 VolumeBytes)
{
    ULONG64 blocks;
    SIZE_T  bitmapBytes;
    PVOID   buf;

    blocks = VolumeBytes / TGSHADOW_BLOCK_SIZE;
    if (blocks == 0) {
        DbgPrint("[TgShadow] volume too small: %llu bytes\n", VolumeBytes);
        return STATUS_INVALID_PARAMETER;
    }
    /* RtlSetBit/RtlTestBit 的索引是 ULONG；上限 4G 块 = 256TB，足够 */
    if (blocks > 0xFFFFFFFFULL) {
        blocks = 0xFFFFFFFFULL;
    }
    bitmapBytes = (SIZE_T)((blocks + 7) / 8);

    /* 用 ExAllocatePoolWithTag 而非 ExAllocatePool2：后者的声明要求
       NTDDI_VERSION >= WIN10_VB，否则编译器按未声明函数处理（返回 int），
       在 x64 上会截断指针 —— 那是必蓝屏的隐患。 */
#pragma warning(push)
#pragma warning(disable: 4996)  /* 新 WDK 将本 API 标记为建议迁移 */
    buf = ExAllocatePoolWithTag(NonPagedPoolNx, bitmapBytes, TGSHADOW_TAG);
#pragma warning(pop)
    if (buf == NULL) {
        DbgPrint("[TgShadow] bitmap alloc failed (%llu bytes)\n",
                 (ULONG64)bitmapBytes);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(buf, bitmapBytes);

    g_TgShadow.ShadowBitmapBuffer = buf;
    RtlInitializeBitMap(&g_TgShadow.ShadowBitmap, (PULONG)buf, (ULONG)blocks);
    g_TgShadow.VolumeBytes = VolumeBytes;
    g_TgShadow.BlockCount = blocks;
    g_TgShadow.ShadowBytesUsed = 0;
    g_TgShadow.RedirectedBlocks = 0;

    DbgPrint("[TgShadow] shadow map ready: volume=%llu MB, blocks=%llu, bitmap=%llu KB\n",
             VolumeBytes / (1024 * 1024), blocks, (ULONG64)bitmapBytes / 1024);
    return STATUS_SUCCESS;
}

static VOID TgShadowFreeMap(VOID);   /* 前向声明（Deferred 需要调用） */

/**
 * 延迟释放位图：detach 之后可能仍有 in-flight 写 IRP 持有并访问位图，
 * 立即 ExFreePool 会造成 use-after-free（PAGE_FAULT_IN_NONPAGED_AREA）。
 * 等一小段时间让在途 I/O 走完再释放。必须在 PASSIVE_LEVEL 调用。
 *
 * 注：生产级实现应改为 I/O 引用计数 + 完成例程等待（P5 完善）；
 *     当前延迟方案足以消除竞态窗口。
 */
static VOID
TgShadowFreeMapDeferred(VOID)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -5000000;   /* 相对时间：500ms（单位 100ns） */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
    TgShadowFreeMap();
}

static VOID
TgShadowFreeMap(VOID)
{
    if (g_TgShadow.ShadowBitmapBuffer != NULL) {
        ExFreePoolWithTag(g_TgShadow.ShadowBitmapBuffer, TGSHADOW_TAG);
        g_TgShadow.ShadowBitmapBuffer = NULL;
    }
    RtlZeroMemory(&g_TgShadow.ShadowBitmap, sizeof(g_TgShadow.ShadowBitmap));
    g_TgShadow.VolumeBytes = 0;
    g_TgShadow.BlockCount = 0;
    g_TgShadow.ShadowBytesUsed = 0;
    g_TgShadow.RedirectedBlocks = 0;
    DbgPrint("[TgShadow] shadow map released\n");
}

/**
 * 标记一段写范围覆盖到的块（P2：只记录，不做数据重定向）。
 * 可在 PASSIVE_LEVEL 或 DISPATCH_LEVEL 调用（KeAcquireSpinLock 自行提升 IRQL）。
 */
static VOID
TgShadowMarkWriteRange(_In_ ULONG64 Offset, _In_ ULONG Length)
{
    ULONG64 startBlock, endBlock, i, marked = 0;
    KIRQL   irql;

    /* 双保险：即便调用方刚检查过 Protected，位图也可能在这一瞬间被释放
       （disable/卸载路径），这里必须再确认结构体本身可用 ——
       对已释放/清零的 RTL_BITMAP 调 RtlTestBit 就是 0x3B + 0xC0000005。 */
    if (g_TgShadow.ShadowBitmapBuffer == NULL ||
        g_TgShadow.ShadowBitmap.Buffer == NULL ||
        g_TgShadow.ShadowBitmap.SizeOfBitMap == 0 ||
        g_TgShadow.BlockCount == 0) {
        return;
    }
    if (Length == 0) {
        return;
    }
    /* 非顺序写（如 ByteOffset = -1）不映射到具体块，跳过 */
    if (Offset == (ULONG64)-1) {
        return;
    }

    startBlock = Offset / TGSHADOW_BLOCK_SIZE;
    if (startBlock >= g_TgShadow.BlockCount) {
        return;
    }
    endBlock = (Offset + (ULONG64)Length - 1) / TGSHADOW_BLOCK_SIZE;
    if (endBlock >= g_TgShadow.BlockCount) {
        endBlock = g_TgShadow.BlockCount - 1;
    }

    KeAcquireSpinLock(&g_TgShadow.Lock, &irql);
    for (i = startBlock; i <= endBlock; i++) {
        if (!RtlTestBit(&g_TgShadow.ShadowBitmap, (ULONG)i)) {
            RtlSetBit(&g_TgShadow.ShadowBitmap, (ULONG)i);
            marked++;
        }
    }
    if (marked > 0) {
        g_TgShadow.RedirectedBlocks += marked;
        g_TgShadow.ShadowBytesUsed += marked * TGSHADOW_BLOCK_SIZE;
    }
    KeReleaseSpinLock(&g_TgShadow.Lock, irql);
}

/* ------------------------------------------------------------------ 工具函数 */

static NTSTATUS
TgShadowAttachToVolume(_In_ ULONG VolumeNumber)
{
    WCHAR            nameBuf[64];
    UNICODE_STRING   volumeName;
    PFILE_OBJECT     fileObj = NULL;
    PDEVICE_OBJECT   targetDev = NULL;
    PDEVICE_OBJECT   filterDev = NULL;
    NTSTATUS         status;
    ULONG            i;
    static const WCHAR prefix[] = L"\\Device\\HarddiskVolume";

    if (g_TgShadow.FilterDevice != NULL) {
        DbgPrint("[TgShadow] already attached\n");
        return STATUS_ALREADY_REGISTERED;
    }

    /* 拼出 \\Device\\HarddiskVolumeN */
    for (i = 0; i < (sizeof(prefix) / sizeof(WCHAR)) - 1; i++) {
        nameBuf[i] = prefix[i];
    }
    {
        WCHAR numBuf[16];
        UNICODE_STRING numStr;
        numStr.Buffer = numBuf;
        numStr.MaximumLength = sizeof(numBuf);
        numStr.Length = 0;
        status = RtlIntegerToUnicodeString(VolumeNumber, 10, &numStr);
        if (!NT_SUCCESS(status)) {
            DbgPrint("[TgShadow] RtlIntegerToUnicodeString failed 0x%08X\n", status);
            return status;
        }
        for (i = 0; i < numStr.Length / sizeof(WCHAR); i++) {
            nameBuf[(sizeof(prefix) / sizeof(WCHAR)) - 1 + i] = numBuf[i];
        }
        nameBuf[(sizeof(prefix) / sizeof(WCHAR)) - 1 + numStr.Length / sizeof(WCHAR)] = L'\0';
    }

    RtlInitUnicodeString(&volumeName, nameBuf);
    DbgPrint("[TgShadow] opening %wZ\n", &volumeName);

    TgShadowTrace(L"attach: opening volume device object");
    status = IoGetDeviceObjectPointer(&volumeName,
                                      FILE_READ_DATA | FILE_WRITE_DATA,
                                      &fileObj, &targetDev);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] IoGetDeviceObjectPointer(%wZ) failed 0x%08X\n",
                 &volumeName, status);
        TgShadowTrace(L"attach: open volume FAILED");
        return status;
    }
    TgShadowTrace(L"attach: volume opened, creating filter device");

    /* 创建过滤设备对象（与目标设备同类型） */
    status = IoCreateDevice(g_TgShadow.ControlDevice->DriverObject,
                            0, NULL,
                            targetDev->DeviceType,
                            targetDev->Characteristics,
                            FALSE,
                            &filterDev);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] IoCreateDevice(filter) failed 0x%08X\n", status);
        ObDereferenceObject(fileObj);
        return status;
    }

    filterDev->Flags |= targetDev->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO |
                                            DO_POWER_PAGABLE);
    filterDev->Flags &= ~DO_DEVICE_INITIALIZING;

    /* ⚠️ 关键（附 dump 证据）：FileObject->DeviceObject 指向本过滤设备
       （因为它在卷栈顶），因此 I/O 管理器/文件系统会把它当"卷设备"来用。
       必须把目标卷的这些字段完整继承过来，否则它们为 0/NULL：
         - Vpb                  : 卷参数块；nt!IopWriteFile 会经它取函数指针，
                                  缺失时崩溃（mov r14,[rcx+18h]，Arg1=c0000005）
         - AlignmentRequirement : 缓冲区对齐要求，影响 I/O 管理器缓冲计算
         - SectorSize           : 磁盘设备扇区大小
       崩溃现场（minidump 反汇编）：
         mov rax,[rdx+8]      ; FileObject->DeviceObject = 本过滤设备
         mov rcx,[rax+50h]    ; 取到无效字段
         mov r14,[rcx+18h]    ; 💥 ACCESS_VIOLATION
       */
    filterDev->Vpb = targetDev->Vpb;
    filterDev->AlignmentRequirement = targetDev->AlignmentRequirement;
    filterDev->SectorSize = targetDev->SectorSize;

    TgShadowTrace(L"attach: created filter device, attaching below NTFS");

    /* ⚠️⚠️ 关键修正（dump 实证）：必须用 IoAttachDevice 而不是
       IoAttachDeviceToDeviceStack。

       卷设备栈原本是：  [NTFS 卷设备对象] -> [卷设备] -> [磁盘设备]
       IoAttachDeviceToDeviceStack 会把过滤设备插到【整个栈的顶部】（NTFS 之上），
       于是 Object Manager 的 IoGetAttachedDeviceReference 取栈顶时拿到我们的设备，
       FileObject->DeviceObject 就指向一个"不是文件系统"的设备；
       nt!IopWriteFile 随后访问它的 Vpb / 快速 I/O 表等字段 → 空指针崩溃：
         BUGCHECK 3b / Arg1 c0000005 / SYMBOL nt!IopWriteFile+e0
         mov rax,[rdx+8]   ; FileObject->DeviceObject = 我们的过滤设备
         mov r14,[rcx+18h] ; 💥 rcx 来自无效字段

       IoAttachDevice 按设备名把过滤设备插到【目标卷设备的直接上层】
       （即 NTFS 之下），这才是扇区级影子该在的位置：NTFS/文件系统正常建立
       FileObject，我们只在扇区 I/O 层面做影子重定向。 */
    status = IoAttachDevice(filterDev, &volumeName, &g_TgShadow.LowerDevice);

    ObDereferenceObject(fileObj);   /* 引用已由 attach 持有 */

    if (!NT_SUCCESS(status) || g_TgShadow.LowerDevice == NULL) {
        DbgPrint("[TgShadow] IoAttachDevice(%wZ) failed 0x%08X\n", &volumeName, status);
        TgShadowTrace(L"attach: IoAttachDevice FAILED");
        g_TgShadow.LowerDevice = NULL;
        IoDeleteDevice(filterDev);
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    }
    TgShadowTrace(L"attach: attached below NTFS, filter is live");

    g_TgShadow.FilterDevice = filterDev;
    g_TgShadow.TargetVolumeNumber = VolumeNumber;
    DbgPrint("[TgShadow] attached to %wZ (volume %lu)\n", &volumeName, VolumeNumber);
    return STATUS_SUCCESS;
}

static VOID
TgShadowDetachFromVolume(VOID)
{
    if (g_TgShadow.FilterDevice == NULL) {
        return;
    }
    if (g_TgShadow.LowerDevice != NULL) {
        IoDetachDevice(g_TgShadow.LowerDevice);
        g_TgShadow.LowerDevice = NULL;
    }
    IoDeleteDevice(g_TgShadow.FilterDevice);
    g_TgShadow.FilterDevice = NULL;
    DbgPrint("[TgShadow] detached\n");
}

/* ------------------------------------------------------------------ 控制设备 IRP */

static NTSTATUS TgShadowPassThrough(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);

/**
 * IRP_MJ_CREATE / CLOSE / CLEANUP 分发。
 *
 * ⚠️ 致命细节（已实测踩坑，附 dump 证据）：
 *   过滤设备挂在卷设备栈上，卷的 CREATE 请求**必须转发**给下层卷设备。
 *   如果在这里直接 IoCompleteRequest 成功返回（像对待自己的控制设备那样），
 *   卷的 FileObject 就不会正确建立，之后任何 WriteFile 都会在
 *   nt!IopWriteFile 里因 FileObject 无效而 0xC0000005 崩溃：
 *     BUGCHECK 3b / Arg1 c0000005 / SYMBOL_NAME nt!IopWriteFile+e0
 *     PROCESS_NAME tgshadowctl.exe
 *   栈里看不到 tgshadow，因为崩在 IRP 构建阶段、在过滤驱动被调用之前。
 */
static NTSTATUS
TgShadowCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    /* 卷栈上的过滤设备：一律转发给下层（绝不能自己完成） */
    if (DeviceObject == g_TgShadow.FilterDevice) {
        return TgShadowPassThrough(DeviceObject, Irp);
    }

    /* 控制设备（\\.\TgShadow）：我们自己处理，直接成功 */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowGetVersion(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    ULONG len = sizeof(TGSHADOW_STATUS);
    PTGSHADOW_STATUS out;

    if (Stack->Parameters.DeviceIoControl.OutputBufferLength < len) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    out = (PTGSHADOW_STATUS)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(out, len);
    out->VersionMajor = TGSHADOW_VERSION_MAJOR;
    out->VersionMinor = TGSHADOW_VERSION_MINOR;
    Irp->IoStatus.Information = len;

    /* 自检埋点放在这里（首次 IOCTL，PASSIVE_LEVEL 安全）：
       一旦用户跑过 version/status，注册表就会出现本条，
       可与"驱动是否真的加载了新构建"互相印证。 */
    TgShadowTrace(L"driver IOCTL ok (version 0.1.0-p2)");
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowGetStatus(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    ULONG len = sizeof(TGSHADOW_STATUS);
    PTGSHADOW_STATUS out;

    if (Stack->Parameters.DeviceIoControl.OutputBufferLength < len) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    out = (PTGSHADOW_STATUS)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(out, len);

    {
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_TgShadow.Lock, &oldIrql);
    out->VersionMajor       = g_TgShadow.VersionMajor;
    out->VersionMinor       = g_TgShadow.VersionMinor;
    out->Protected          = g_TgShadow.Protected;
    out->FilterAttached     = (g_TgShadow.FilterDevice != NULL) ? 1 : 0;
    out->TargetVolumeNumber = g_TgShadow.TargetVolumeNumber;
    out->ShadowBytesTotal   = g_TgShadow.ShadowBytesTotal;
    out->ShadowBytesUsed    = g_TgShadow.ShadowBytesUsed;
    out->ReadCount          = g_TgShadow.ReadCount;
    out->WriteCount         = g_TgShadow.WriteCount;
    out->RedirectedBlocks   = g_TgShadow.RedirectedBlocks;
    KeReleaseSpinLock(&g_TgShadow.Lock, oldIrql);
    }

    Irp->IoStatus.Information = len;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowGetVolumes(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    ULONG maxEntries, i, count = 0;
    PTGSHADOW_VOLUME_INFO out;
    NTSTATUS status;
    WCHAR nameBuf[64];
    UNICODE_STRING volumeName;
    PFILE_OBJECT fileObj = NULL;
    PDEVICE_OBJECT dev = NULL;

    maxEntries = Stack->Parameters.DeviceIoControl.OutputBufferLength /
                 sizeof(TGSHADOW_VOLUME_INFO);
    if (maxEntries == 0) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    out = (PTGSHADOW_VOLUME_INFO)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(out, Stack->Parameters.DeviceIoControl.OutputBufferLength);

    /* 探测 HarddiskVolume0..31 哪些存在 */
    for (i = 0; i < 32 && count < maxEntries; i++) {
        UNICODE_STRING numStr;
        WCHAR numBuf[16];
        ULONG j;
        static const WCHAR prefix[] = L"\\Device\\HarddiskVolume";

        numStr.Buffer = numBuf;
        numStr.MaximumLength = sizeof(numBuf);
        numStr.Length = 0;
        if (!NT_SUCCESS(RtlIntegerToUnicodeString(i, 10, &numStr))) {
            continue;
        }
        for (j = 0; j < (sizeof(prefix) / sizeof(WCHAR)) - 1; j++) {
            nameBuf[j] = prefix[j];
        }
        for (j = 0; j < numStr.Length / sizeof(WCHAR); j++) {
            nameBuf[(sizeof(prefix) / sizeof(WCHAR)) - 1 + j] = numBuf[j];
        }
        nameBuf[(sizeof(prefix) / sizeof(WCHAR)) - 1 + numStr.Length / sizeof(WCHAR)] = L'\0';

        RtlInitUnicodeString(&volumeName, nameBuf);
        status = IoGetDeviceObjectPointer(&volumeName, FILE_READ_DATA,
                                          &fileObj, &dev);
        if (!NT_SUCCESS(status)) {
            continue;
        }
        out[count].VolumeNumber = i;
        out[count].IsTarget = (g_TgShadow.Protected &&
                               g_TgShadow.TargetVolumeNumber == i) ? 1 : 0;
        out[count].TotalBytes = 0;
        count++;
        ObDereferenceObject(fileObj);
    }

    Irp->IoStatus.Information = count * sizeof(TGSHADOW_VOLUME_INFO);
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowEnable(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    PTGSHADOW_ENABLE_INPUT in;
    NTSTATUS status;

    /* 最早埋点：只要 IOCTL_ENABLE 到达驱动就记录。
       若蓝屏后该值仍停留在 "driver loaded"，说明 enable 根本没进驱动。 */
    TgShadowTrace(L"enable: IOCTL received");

    if (Stack->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(TGSHADOW_ENABLE_INPUT)) {
        TgShadowTrace(L"enable: bad input size");
        return STATUS_BUFFER_TOO_SMALL;
    }
    in = (PTGSHADOW_ENABLE_INPUT)Irp->AssociatedIrp.SystemBuffer;
    TgShadowTrace(L"enable: input ok, checking state");

    /* P1：只做卷过滤挂载 + 标记启用；影子存储分配在 P2 实现。
       影子容量先记录，用户态负责在另一卷创建影子文件。 */
    /* 已挂载时的处理：
         同一卷 → 只重建影子位图（先停用保护，避免在途 I/O 访问旧图）；
         不同卷 → 拒绝，要求先 disable 并重启驱动。
       注意：绝不在运行期 IoDetachDevice 并把 LowerDevice 置 NULL ——
       写 IRP 线程可能刚好在检查通过之后调用 IoCallDriver(LowerDevice)，
       拿到 NULL 就是 0x3B + 0xC0000005（已实测踩过）。 */
    if (g_TgShadow.FilterDevice != NULL) {
        if (g_TgShadow.TargetVolumeNumber != in->VolumeNumber) {
            DbgPrint("[TgShadow] enable: already attached to volume %lu, "
                     "cannot switch to %lu without reload\n",
                     g_TgShadow.TargetVolumeNumber, in->VolumeNumber);
            return STATUS_INVALID_PARAMETER;
        }
        if (g_TgShadow.ShadowBitmapBuffer != NULL) {
            KIRQL oldIrql;
            KeAcquireSpinLock(&g_TgShadow.Lock, &oldIrql);
            g_TgShadow.Protected = 0;      /* 先停用，在途 I/O 不再碰位图 */
            KeReleaseSpinLock(&g_TgShadow.Lock, oldIrql);
            TgShadowFreeMapDeferred();     /* 延迟释放旧图 */
        }
    }

    TgShadowTrace(L"enable: attaching volume");
    status = TgShadowAttachToVolume(in->VolumeNumber);
    if (!NT_SUCCESS(status) && status != STATUS_ALREADY_REGISTERED) {
        TgShadowTrace(L"enable: attach FAILED");
        return status;
    }
    TgShadowTrace(L"enable: attach ok, allocating map");

    /* attachonly 模式：只挂载过滤设备，完全不碰位图与写记录（分步定位用） */
    if (in->Flags & TGSHADOW_FLAG_ATTACH_ONLY) {
        g_TgShadow.MarkWrites = 0;
        g_TgShadow.Protected = 1;
        TgShadowTrace(L"enable: DONE (attach-only mode)");
        DbgPrint("[TgShadow] ATTACH-ONLY mode enabled (no write marking)\n");
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }
    g_TgShadow.MarkWrites = 1;

    /* P2：按用户态提供的卷容量建立影子位图（内核不自行查询，见 tgshadow.h 说明） */
    {
        ULONG64 volumeBytes = in->VolumeBytes;

        if (volumeBytes == 0) {
            /* 用户态未提供时的安全兜底：按 32GB 建图（宁可少标记也不越界） */
            volumeBytes = 32ULL * 1024 * 1024 * 1024;
            DbgPrint("[TgShadow] enable: VolumeBytes not supplied, fallback 32GB\n");
        }
        status = TgShadowAllocMap(volumeBytes);
        if (!NT_SUCCESS(status)) {
            TgShadowTrace(L"enable: map alloc FAILED");
            return status;
        }
    }
    TgShadowTrace(L"enable: map ok, turning protection on");

    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_TgShadow.Lock, &oldIrql);
        g_TgShadow.Protected = 1;
        g_TgShadow.ShadowBytesTotal = in->ShadowBytes;
        KeReleaseSpinLock(&g_TgShadow.Lock, oldIrql);
    }
    TgShadowTrace(L"enable: DONE (protection on)");

    DbgPrint("[TgShadow] protection ENABLED on volume %lu (shadow=%llu bytes)\n",
             in->VolumeNumber, in->ShadowBytes);
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowDisable(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    UNREFERENCED_PARAMETER(Stack);
    /* 只停用保护、不 detach：设备栈保持稳定，避免在途 I/O 拿到 NULL 下层设备。
       真正的 detach 只在驱动卸载（TgShadowUnload）时执行。 */
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_TgShadow.Lock, &oldIrql);
        g_TgShadow.Protected = 0;
        KeReleaseSpinLock(&g_TgShadow.Lock, oldIrql);
    }
    TgShadowFreeMapDeferred();
    DbgPrint("[TgShadow] protection DISABLED (filter stays attached)\n");
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    /* 过滤设备上的 DEVICE_CONTROL 是卷/文件系统/安全软件的 IOCTL
       （如卷信息查询、TRIM 等），必须原样转发；只有我们自己的控制设备
       才处理 TGSHADOW_* 私有 IOCTL。否则会破坏卷的正常工作。 */
    if (DeviceObject != g_TgShadow.ControlDevice) {
        return TgShadowPassThrough(DeviceObject, Irp);
    }

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_TGSHADOW_GET_VERSION:
        status = TgShadowGetVersion(Irp, stack);
        break;
    case IOCTL_TGSHADOW_GET_STATUS:
        status = TgShadowGetStatus(Irp, stack);
        break;
    case IOCTL_TGSHADOW_GET_VOLUMES:
        status = TgShadowGetVolumes(Irp, stack);
        break;
    case IOCTL_TGSHADOW_ENABLE:
        status = TgShadowEnable(Irp, stack);
        break;
    case IOCTL_TGSHADOW_DISABLE:
        status = TgShadowDisable(Irp, stack);
        break;
    case IOCTL_TGSHADOW_COMMIT:
    case IOCTL_TGSHADOW_DISCARD:
        /* P3 实现：提交/丢弃影子数据 */
        status = STATUS_NOT_IMPLEMENTED;
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ------------------------------------------------------------------ 通用直通与 PnP */

/**
 * 通用直通：把 IRP 原样转给下层设备。
 *
 * 卷过滤驱动必须转发 PnP / Power / Flush / Shutdown / InternalDeviceControl /
 * SystemControl —— 这些 IRP 由文件系统/卷管理器/电源管理器下发，若被过滤驱动
 * 吞掉（返回 STATUS_INVALID_DEVICE_REQUEST 或干脆无处理），会导致卷卸载、
 * 电源状态转换、刷新缓冲等操作失败，进而 BSOD。
 */
static NTSTATUS
TgShadowPassThrough(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    if (g_TgShadow.LowerDevice == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(g_TgShadow.LowerDevice, Irp);
}

/**
 * PnP 处理：卷被移除（IRP_MN_REMOVE_DEVICE）时必须先把自己从设备栈摘下来，
 * 否则后续 I/O 会打到已删除的设备对象上 —— 典型 BSOD 来源。
 */
static NTSTATUS
TgShadowPnp(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    if (DeviceObject == g_TgShadow.FilterDevice &&
        stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        DbgPrint("[TgShadow] PnP REMOVE_DEVICE: detaching filter\n");
        if (g_TgShadow.LowerDevice != NULL) {
            IoDetachDevice(g_TgShadow.LowerDevice);
            g_TgShadow.LowerDevice = NULL;
        }
        if (g_TgShadow.FilterDevice != NULL) {
            PDEVICE_OBJECT dying = g_TgShadow.FilterDevice;
            g_TgShadow.FilterDevice = NULL;
            IoDeleteDevice(dying);   /* 引用归零时才真正删除，安全 */
        }
    }
    return TgShadowPassThrough(DeviceObject, Irp);
}

/* ------------------------------------------------------------------ 卷过滤 IRP（P2：直通 + 写记录） */

static NTSTATUS
TgShadowFilterReadWrite(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    /* 只处理挂在卷上的过滤设备；控制设备收到读写属非法请求，
       未挂载时也绝不能把 IRP 转发给空的下层设备（否则 BSOD） */
    if (DeviceObject != g_TgShadow.FilterDevice || g_TgShadow.LowerDevice == NULL) {
        DbgPrint("[TgShadow] read/write on non-filter device, rejected\n");
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* P2：写请求记录到影子位图（数据仍直通，不改动 —— 零破坏）；
       P3 起才把写入真正重定向到影子存储 */
    if (stack->MajorFunction == IRP_MJ_WRITE) {
        InterlockedIncrement64((volatile LONG64 *)&g_TgShadow.WriteCount);
        if (g_TgShadow.Protected && g_TgShadow.MarkWrites) {
            LARGE_INTEGER off = stack->Parameters.Write.ByteOffset;
            ULONG         len = stack->Parameters.Write.Length;
            if (off.QuadPart >= 0 && len > 0) {
                TgShadowMarkWriteRange((ULONG64)off.QuadPart, len);
            }
        }
    } else {
        InterlockedIncrement64((volatile LONG64 *)&g_TgShadow.ReadCount);
    }

    UNREFERENCED_PARAMETER(DeviceObject);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(g_TgShadow.LowerDevice, Irp);
}

/* ------------------------------------------------------------------ 驱动入口 */

static VOID
TgShadowUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING linkName;

    DbgPrint("[TgShadow] unload: reads=%llu writes=%llu\n",
             g_TgShadow.ReadCount, g_TgShadow.WriteCount);

    TgShadowDetachFromVolume();
    TgShadowFreeMapDeferred();   /* detach 后等在途 I/O 结束再释放位图 */

    RtlInitUnicodeString(&linkName, L"\\??\\TgShadow");
    IoDeleteSymbolicLink(&linkName);

    if (g_TgShadow.ControlDevice != NULL) {
        IoDeleteDevice(g_TgShadow.ControlDevice);
        g_TgShadow.ControlDevice = NULL;
    }
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("[TgShadow] unloaded\n");
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING deviceName, linkName;
    PDEVICE_OBJECT deviceObject = NULL;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);
    DbgPrint("[TgShadow] DriverEntry (version %S)\n", TGSHADOW_VERSION_STRING);

    RtlZeroMemory(&g_TgShadow, sizeof(g_TgShadow));
    KeInitializeSpinLock(&g_TgShadow.Lock);
    g_TgShadow.VersionMajor = TGSHADOW_VERSION_MAJOR;
    g_TgShadow.VersionMinor = TGSHADOW_VERSION_MINOR;

    RtlInitUnicodeString(&deviceName, TGSHADOW_NT_DEVICE_NAME);
    status = IoCreateDevice(DriverObject, 0, &deviceName,
                            TGSHADOW_DEVICE_TYPE, 0, FALSE, &deviceObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] IoCreateDevice failed 0x%08X\n", status);
        return status;
    }
    g_TgShadow.ControlDevice = deviceObject;

    RtlInitUnicodeString(&linkName, L"\\??\\TgShadow");
    status = IoCreateSymbolicLink(&linkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] IoCreateSymbolicLink failed 0x%08X\n", status);
        IoDeleteDevice(deviceObject);
        g_TgShadow.ControlDevice = NULL;
        return status;
    }

    /* 先把所有 major function 指向通用直通：卷过滤驱动必须转发
       PnP/Power/Flush/Shutdown/InternalDeviceControl/SystemControl 等，
       否则会破坏卷的正常生命周期（BSOD 常见来源）。再覆盖我们真正处理的。 */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = TgShadowPassThrough;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = TgShadowDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_READ]           = TgShadowFilterReadWrite;
    DriverObject->MajorFunction[IRP_MJ_WRITE]          = TgShadowFilterReadWrite;
    DriverObject->MajorFunction[IRP_MJ_PNP]            = TgShadowPnp;
    DriverObject->DriverUnload = TgShadowUnload;

    /* 注意：DriverEntry 处于驱动加载路径，绝不做任何非必要操作
       （注册表写入/内存分配等）——一旦失败整个驱动就加载不起来。
       自检埋点改由首次 IOCTL 时记录。 */

    DbgPrint("[TgShadow] control device ready: %S\n", TGSHADOW_WIN32_DEVICE);
    return STATUS_SUCCESS;
}

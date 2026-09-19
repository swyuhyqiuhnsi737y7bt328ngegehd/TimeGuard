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
} TGSHADOW_GLOBAL, *PTGSHADOW_GLOBAL;

static TGSHADOW_GLOBAL g_TgShadow;

/* ------------------------------------------------------------------ 影子映射（P2） */

/**
 * 查询卷容量。以同步 IRP 下发 IOCTL_DISK_GET_LENGTH_INFO 给下层设备。
 * 必须在 PASSIVE_LEVEL 调用（Enable 的 IOCTL 路径满足）。
 */
static NTSTATUS
TgShadowQueryVolumeSize(_In_ PDEVICE_OBJECT LowerDevice, _Out_ PULONG64 Bytes)
{
    KEVENT                event;
    IO_STATUS_BLOCK       iosb;
    PIRP                  irp;
    GET_LENGTH_INFORMATION info;
    NTSTATUS              status;

    RtlZeroMemory(&info, sizeof(info));
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(IOCTL_DISK_GET_LENGTH_INFO,
                                        LowerDevice,
                                        NULL, 0,
                                        &info, sizeof(info),
                                        FALSE, &event, &iosb);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    if (NT_SUCCESS(status)) {
        *Bytes = (ULONG64)info.Length.QuadPart;
    }
    return status;
}

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

    if (g_TgShadow.ShadowBitmapBuffer == NULL || g_TgShadow.BlockCount == 0) {
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

    status = IoGetDeviceObjectPointer(&volumeName,
                                      FILE_READ_DATA | FILE_WRITE_DATA,
                                      &fileObj, &targetDev);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] IoGetDeviceObjectPointer(%wZ) failed 0x%08X\n",
                 &volumeName, status);
        return status;
    }

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

    g_TgShadow.LowerDevice = IoAttachDeviceToDeviceStack(filterDev, targetDev);

    ObDereferenceObject(fileObj);   /* 引用已由 attach 持有 */

    if (g_TgShadow.LowerDevice == NULL) {
        DbgPrint("[TgShadow] IoAttachDeviceToDeviceStack failed\n");
        IoDeleteDevice(filterDev);
        return STATUS_UNSUCCESSFUL;
    }

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

static NTSTATUS
TgShadowCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
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

    if (Stack->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(TGSHADOW_ENABLE_INPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    in = (PTGSHADOW_ENABLE_INPUT)Irp->AssociatedIrp.SystemBuffer;

    /* P1：只做卷过滤挂载 + 标记启用；影子存储分配在 P2 实现。
       影子容量先记录，用户态负责在另一卷创建影子文件。 */
    status = TgShadowAttachToVolume(in->VolumeNumber);
    if (!NT_SUCCESS(status) && status != STATUS_ALREADY_REGISTERED) {
        return status;
    }

    /* P2：查询受保护卷容量并建立影子位图 */
    {
        ULONG64 volumeBytes = 0;

        TgShadowFreeMap();   /* 重复 enable 时先释放旧图 */
        status = TgShadowQueryVolumeSize(g_TgShadow.LowerDevice, &volumeBytes);
        if (!NT_SUCCESS(status)) {
            DbgPrint("[TgShadow] query volume size failed 0x%08X\n", status);
            return status;
        }
        status = TgShadowAllocMap(volumeBytes);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_TgShadow.Lock, &oldIrql);
        g_TgShadow.Protected = 1;
        g_TgShadow.ShadowBytesTotal = in->ShadowBytes;
        KeReleaseSpinLock(&g_TgShadow.Lock, oldIrql);
    }

    DbgPrint("[TgShadow] protection ENABLED on volume %lu (shadow=%llu bytes)\n",
             in->VolumeNumber, in->ShadowBytes);
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowDisable(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    UNREFERENCED_PARAMETER(Stack);
    TgShadowDetachFromVolume();
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_TgShadow.Lock, &oldIrql);
        g_TgShadow.Protected = 0;
        KeReleaseSpinLock(&g_TgShadow.Lock, oldIrql);
    }
    TgShadowFreeMap();
    DbgPrint("[TgShadow] protection DISABLED\n");
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);

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

/* ------------------------------------------------------------------ 卷过滤 IRP（P1：直通 + 计数） */

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
        if (g_TgShadow.Protected) {
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
    TgShadowFreeMap();

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

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = NULL;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = TgShadowDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_READ]           = TgShadowFilterReadWrite;
    DriverObject->MajorFunction[IRP_MJ_WRITE]          = TgShadowFilterReadWrite;
    DriverObject->DriverUnload = TgShadowUnload;

    DbgPrint("[TgShadow] control device ready: %S\n", TGSHADOW_WIN32_DEVICE);
    return STATUS_SUCCESS;
}

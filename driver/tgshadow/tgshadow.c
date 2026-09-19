/*==============================================================================
  tgshadow.c - TimeGuard 磁盘影子驱动（P2：卷 Upper Filter 版）
  ----------------------------------------------------------------------------
  挂载方式（关键）：
    注册为【卷 class 的 Upper Filter】：
      HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}\UpperFilters
    系统在每次挂载卷设备时调用本驱动的 AddDevice，把过滤设备插到
    【文件系统(NTFS) 之下、卷设备之上】—— 这是扇区级影子的正确位置。

    为什么不动态挂载到已挂载的卷：文件系统在挂载时已缓存下层设备指针，
    之后其 I/O 会绕过动态插入的过滤设备（实测 I/O 计数恒为 0）。

    历史教训：曾用 IoAttachDeviceToDeviceStack 挂到"整个栈顶"（NTFS 之上），
    FileObject->DeviceObject 便指向本驱动这个非文件系统设备，
    nt!IopWriteFile 访问其卷字段时空指针崩溃（0x3B + 0xC0000005）。

  P2：拦截 IRP_MJ_READ/WRITE，用位图记录被写过的 64KB 块（不重定向数据）。
  P3：升级为写时复制重定向到影子存储，重启丢弃 = 还原。
==============================================================================*/
#include <ntddk.h>
#include "tgshadow.h"

#define TGSHADOW_TAG  'hSgT'

typedef struct _TGSHADOW_DEV_EXT {
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  LowerDevice;
    ULONG           VolumeNumber;
    ULONG           IsControl;
    ULONG64         ReadCount;
    ULONG64         WriteCount;
} TGSHADOW_DEV_EXT, *PTGSHADOW_DEV_EXT;

#define TGSHADOW_EXT(d) ((PTGSHADOW_DEV_EXT)((d)->DeviceExtension))

static PDEVICE_OBJECT g_ControlDevice = NULL;
static ULONG          g_TargetVolume = 0;
static ULONG          g_Protected = 0;
static ULONG          g_MarkWrites = 1;
static KSPIN_LOCK     g_Lock;

static RTL_BITMAP     g_Bitmap;
static PVOID          g_BitmapBuffer = NULL;
static ULONG64        g_VolumeBytes = 0;
static ULONG64        g_BlockCount = 0;
static ULONG64        g_MarkedBlocks = 0;
static ULONG64        g_ShadowBytesUsed = 0;

static ULONG64        g_TotalReads = 0;
static ULONG64        g_TotalWrites = 0;
static LONG           g_AttachedDevices = 0;

/* ---------------------------------------------------------------- 崩溃定位埋点 */

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

/* ---------------------------------------------------------------- 影子位图 */

static VOID TgShadowFreeMap(VOID);

static VOID
TgShadowFreeMapDeferred(VOID)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -5000000;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
    TgShadowFreeMap();
}

static VOID
TgShadowFreeMap(VOID)
{
    if (g_BitmapBuffer != NULL) {
        ExFreePoolWithTag(g_BitmapBuffer, TGSHADOW_TAG);
        g_BitmapBuffer = NULL;
    }
    RtlZeroMemory(&g_Bitmap, sizeof(g_Bitmap));
    g_VolumeBytes = 0;
    g_BlockCount = 0;
    g_MarkedBlocks = 0;
    g_ShadowBytesUsed = 0;
    DbgPrint("[TgShadow] shadow map released\n");
}

static NTSTATUS
TgShadowAllocMap(_In_ ULONG64 VolumeBytes)
{
    ULONG64 blocks;
    SIZE_T  bitmapBytes;
    PVOID   buf;

    blocks = VolumeBytes / TGSHADOW_BLOCK_SIZE;
    if (blocks == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (blocks > 0xFFFFFFFFULL) {
        blocks = 0xFFFFFFFFULL;
    }
    bitmapBytes = (SIZE_T)((blocks + 7) / 8);

#pragma warning(push)
#pragma warning(disable: 4996)
    buf = ExAllocatePoolWithTag(NonPagedPoolNx, bitmapBytes, TGSHADOW_TAG);
#pragma warning(pop)
    if (buf == NULL) {
        DbgPrint("[TgShadow] bitmap alloc failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(buf, bitmapBytes);

    g_BitmapBuffer = buf;
    RtlInitializeBitMap(&g_Bitmap, (PULONG)buf, (ULONG)blocks);
    g_VolumeBytes = VolumeBytes;
    g_BlockCount = blocks;
    g_MarkedBlocks = 0;
    g_ShadowBytesUsed = 0;

    DbgPrint("[TgShadow] shadow map ready: vol=%llu MB blocks=%llu\n",
             VolumeBytes / (1024 * 1024), blocks);
    return STATUS_SUCCESS;
}

static VOID
TgShadowMarkWriteRange(_In_ ULONG64 Offset, _In_ ULONG Length)
{
    ULONG64 startBlock, endBlock, i, marked = 0;
    KIRQL   irql;

    if (g_BitmapBuffer == NULL || g_Bitmap.Buffer == NULL ||
        g_Bitmap.SizeOfBitMap == 0 || g_BlockCount == 0) {
        return;
    }
    if (Length == 0 || Offset == (ULONG64)-1) {
        return;
    }

    startBlock = Offset / TGSHADOW_BLOCK_SIZE;
    if (startBlock >= g_BlockCount) {
        return;
    }
    endBlock = (Offset + (ULONG64)Length - 1) / TGSHADOW_BLOCK_SIZE;
    if (endBlock >= g_BlockCount) {
        endBlock = g_BlockCount - 1;
    }

    KeAcquireSpinLock(&g_Lock, &irql);
    for (i = startBlock; i <= endBlock; i++) {
        if (!RtlTestBit(&g_Bitmap, (ULONG)i)) {
            RtlSetBit(&g_Bitmap, (ULONG)i);
            marked++;
        }
    }
    if (marked > 0) {
        g_MarkedBlocks += marked;
        g_ShadowBytesUsed += marked * TGSHADOW_BLOCK_SIZE;
    }
    KeReleaseSpinLock(&g_Lock, irql);
}

/* ---------------------------------------------------------------- 卷号识别 */

static ULONG
TgShadowQueryVolumeNumber(_In_ PDEVICE_OBJECT Pdo)
{
    static const WCHAR prefix[] = L"HarddiskVolume";
    WCHAR    name[256];
    ULONG    len = 0;
    NTSTATUS status;
    ULONG    i, j, num;

    status = IoGetDeviceProperty(Pdo, DevicePropertyPhysicalDeviceObjectName,
                                 sizeof(name), name, &len);
    if (!NT_SUCCESS(status)) {
        return 0;
    }
    for (i = 0; name[i] != L'\0'; i++) {
        if (name[i] != L'H') {
            continue;
        }
        for (j = 0; prefix[j] != L'\0' && name[i + j] == prefix[j]; j++) {
            ;
        }
        if (prefix[j] != L'\0') {
            continue;
        }
        i += j;
        num = 0;
        while (name[i] >= L'0' && name[i] <= L'9') {
            num = num * 10 + (ULONG)(name[i] - L'0');
            i++;
        }
        return num;
    }
    return 0;
}

/* ---------------------------------------------------------------- 过滤分发 */

static NTSTATUS
TgShadowPassThrough(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PTGSHADOW_DEV_EXT ext;

    if (DeviceObject == g_ControlDevice) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ext = TGSHADOW_EXT(DeviceObject);
    if (ext == NULL || ext->LowerDevice == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

static NTSTATUS
TgShadowCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    if (DeviceObject != g_ControlDevice) {
        return TgShadowPassThrough(DeviceObject, Irp);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowReadWrite(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PTGSHADOW_DEV_EXT  ext;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    if (DeviceObject == g_ControlDevice) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ext = TGSHADOW_EXT(DeviceObject);
    if (ext == NULL || ext->LowerDevice == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (stack->MajorFunction == IRP_MJ_WRITE) {
        InterlockedIncrement64((volatile LONG64 *)&ext->WriteCount);
        InterlockedIncrement64((volatile LONG64 *)&g_TotalWrites);
        if (g_Protected && g_MarkWrites && ext->VolumeNumber == g_TargetVolume) {
            LARGE_INTEGER off = stack->Parameters.Write.ByteOffset;
            ULONG         len = stack->Parameters.Write.Length;
            if (off.QuadPart >= 0 && len > 0) {
                TgShadowMarkWriteRange((ULONG64)off.QuadPart, len);
            }
        }
    } else {
        InterlockedIncrement64((volatile LONG64 *)&ext->ReadCount);
        InterlockedIncrement64((volatile LONG64 *)&g_TotalReads);
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

static NTSTATUS
TgShadowPnp(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PTGSHADOW_DEV_EXT  ext;

    if (DeviceObject == g_ControlDevice) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    ext = TGSHADOW_EXT(DeviceObject);
    if (ext == NULL || ext->LowerDevice == NULL) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        NTSTATUS status;
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        IoDetachDevice(ext->LowerDevice);
        ext->LowerDevice = NULL;
        InterlockedDecrement(&g_AttachedDevices);
        IoDeleteDevice(DeviceObject);
        return status;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->LowerDevice, Irp);
}

static NTSTATUS
TgShadowPower(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PTGSHADOW_DEV_EXT ext;

    if (DeviceObject == g_ControlDevice) {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    ext = TGSHADOW_EXT(DeviceObject);
    PoStartNextPowerIrp(Irp);
    if (ext == NULL || ext->LowerDevice == NULL) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->LowerDevice, Irp);
}

/* ---------------------------------------------------------------- IOCTL */

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

    out->VersionMajor       = TGSHADOW_VERSION_MAJOR;
    out->VersionMinor       = TGSHADOW_VERSION_MINOR;
    out->Protected          = g_Protected;
    out->FilterAttached     = (g_AttachedDevices > 0) ? 1 : 0;
    out->TargetVolumeNumber = g_TargetVolume;
    out->ReadCount          = g_TotalReads;
    out->WriteCount         = g_TotalWrites;
    out->RedirectedBlocks   = g_MarkedBlocks;
    out->ShadowBytesUsed    = g_ShadowBytesUsed;
    out->ShadowBytesTotal   = (ULONG64)g_VolumeBytes;
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
        status = IoGetDeviceObjectPointer(&volumeName, FILE_READ_DATA, &fileObj, &dev);
        if (!NT_SUCCESS(status)) {
            continue;
        }
        out[count].VolumeNumber = i;
        out[count].IsTarget = (g_Protected && g_TargetVolume == i) ? 1 : 0;
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

    TgShadowTrace(L"enable: IOCTL received");
    if (Stack->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(TGSHADOW_ENABLE_INPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    in = (PTGSHADOW_ENABLE_INPUT)Irp->AssociatedIrp.SystemBuffer;

    g_MarkWrites = (in->Flags & TGSHADOW_FLAG_ATTACH_ONLY) ? 0 : 1;
    if (g_BitmapBuffer != NULL) {
        g_Protected = 0;
        TgShadowFreeMapDeferred();
    }
    if (g_MarkWrites) {
        ULONG64 volumeBytes = in->VolumeBytes;
        if (volumeBytes == 0) {
            volumeBytes = 32ULL * 1024 * 1024 * 1024;
        }
        status = TgShadowAllocMap(volumeBytes);
        if (!NT_SUCCESS(status)) {
            TgShadowTrace(L"enable: map alloc FAILED");
            return status;
        }
    }
    g_TargetVolume = in->VolumeNumber;
    g_Protected = 1;
    TgShadowTrace(L"enable: DONE");

    DbgPrint("[TgShadow] protection ON: vol=%lu markWrites=%lu filters=%ld\n",
             g_TargetVolume, g_MarkWrites, g_AttachedDevices);
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowDisable(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    UNREFERENCED_PARAMETER(Stack);
    g_Protected = 0;
    TgShadowFreeMapDeferred();
    TgShadowTrace(L"disable: DONE");
    DbgPrint("[TgShadow] protection OFF\n");
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    if (DeviceObject != g_ControlDevice) {
        return TgShadowPassThrough(DeviceObject, Irp);
    }
    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_TGSHADOW_GET_VERSION: status = TgShadowGetVersion(Irp, stack); break;
    case IOCTL_TGSHADOW_GET_STATUS:  status = TgShadowGetStatus(Irp, stack);  break;
    case IOCTL_TGSHADOW_GET_VOLUMES: status = TgShadowGetVolumes(Irp, stack); break;
    case IOCTL_TGSHADOW_ENABLE:      status = TgShadowEnable(Irp, stack);     break;
    case IOCTL_TGSHADOW_DISABLE:     status = TgShadowDisable(Irp, stack);    break;
    case IOCTL_TGSHADOW_COMMIT:
    case IOCTL_TGSHADOW_DISCARD:     status = STATUS_NOT_IMPLEMENTED;         break;
    default:                         status = STATUS_INVALID_DEVICE_REQUEST;  break;
    }
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ---------------------------------------------------------------- AddDevice */

static NTSTATUS
TgShadowAddDevice(_In_ PDRIVER_OBJECT DriverObject,
                  _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT    filterDev = NULL;
    PTGSHADOW_DEV_EXT ext;
    NTSTATUS          status;

    status = IoCreateDevice(DriverObject, sizeof(TGSHADOW_DEV_EXT), NULL,
                            PhysicalDeviceObject->DeviceType,
                            PhysicalDeviceObject->Characteristics,
                            FALSE, &filterDev);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] AddDevice: IoCreateDevice failed 0x%08X\n", status);
        return status;
    }
    ext = TGSHADOW_EXT(filterDev);
    RtlZeroMemory(ext, sizeof(TGSHADOW_DEV_EXT));
    ext->Self = filterDev;
    ext->VolumeNumber = TgShadowQueryVolumeNumber(PhysicalDeviceObject);

    filterDev->Flags |= PhysicalDeviceObject->Flags &
                        (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    filterDev->Flags &= ~DO_DEVICE_INITIALIZING;

    ext->LowerDevice = IoAttachDeviceToDeviceStack(filterDev, PhysicalDeviceObject);
    if (ext->LowerDevice == NULL) {
        IoDeleteDevice(filterDev);
        return STATUS_UNSUCCESSFUL;
    }

    filterDev->Vpb = PhysicalDeviceObject->Vpb;
    filterDev->AlignmentRequirement = PhysicalDeviceObject->AlignmentRequirement;
    filterDev->SectorSize = PhysicalDeviceObject->SectorSize;

    InterlockedIncrement(&g_AttachedDevices);
    DbgPrint("[TgShadow] AddDevice: attached, volume=%lu type=0x%04lX\n",
             ext->VolumeNumber, PhysicalDeviceObject->DeviceType);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- 入口/卸载 */

static VOID
TgShadowUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING linkName;

    DbgPrint("[TgShadow] unload: reads=%llu writes=%llu\n", g_TotalReads, g_TotalWrites);
    g_Protected = 0;
    TgShadowFreeMap();
    RtlInitUnicodeString(&linkName, L"\\??\\TgShadow");
    IoDeleteSymbolicLink(&linkName);
    if (g_ControlDevice != NULL) {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
    }
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("[TgShadow] unloaded\n");
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS          status;
    UNICODE_STRING    deviceName, linkName;
    PDEVICE_OBJECT    deviceObject = NULL;
    ULONG             i;
    PTGSHADOW_DEV_EXT ext;

    UNREFERENCED_PARAMETER(RegistryPath);
    DbgPrint("[TgShadow] DriverEntry (upper-filter build)\n");

    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(&g_Bitmap, sizeof(g_Bitmap));

    RtlInitUnicodeString(&deviceName, TGSHADOW_NT_DEVICE_NAME);
    status = IoCreateDevice(DriverObject, sizeof(TGSHADOW_DEV_EXT), &deviceName,
                            TGSHADOW_DEVICE_TYPE, 0, FALSE, &deviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    g_ControlDevice = deviceObject;
    ext = TGSHADOW_EXT(deviceObject);
    RtlZeroMemory(ext, sizeof(TGSHADOW_DEV_EXT));
    ext->Self = deviceObject;
    ext->IsControl = 1;

    RtlInitUnicodeString(&linkName, L"\\??\\TgShadow");
    status = IoCreateSymbolicLink(&linkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        g_ControlDevice = NULL;
        return status;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = TgShadowPassThrough;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE]          = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]           = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]         = TgShadowCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]  = TgShadowDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_READ]            = TgShadowReadWrite;
    DriverObject->MajorFunction[IRP_MJ_WRITE]           = TgShadowReadWrite;
    DriverObject->MajorFunction[IRP_MJ_PNP]             = TgShadowPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]           = TgShadowPower;

    DriverObject->DriverExtension->AddDevice = TgShadowAddDevice;
    DriverObject->DriverUnload = TgShadowUnload;

    DbgPrint("[TgShadow] control device ready: %S\n", TGSHADOW_WIN32_DEVICE);
    return STATUS_SUCCESS;
}

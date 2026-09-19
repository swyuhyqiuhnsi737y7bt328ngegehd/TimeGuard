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

/* P3 诊断计数器（定位 COW 分支未触发的原因） */
static ULONG64        g_WritesPassive = 0;
static ULONG64        g_WritesHighIrql = 0;
static ULONG64        g_CowEntered = 0;
static ULONG64        g_CowNoBuffer = 0;
static ULONG64        g_CowWriteOk = 0;    /* 写成功重定向到影子的次数 */
static ULONG64        g_CowReadOk = 0;     /* 读成功命中影子的次数 */
static ULONG64        g_CowFailed = 0;     /* COW 失败并落回透传的次数 */
static ULONG          g_CowLastStatus = 0; /* 最近一次 COW 失败的状态码 */

/* ---- P3：影子存储与块映射表 ---- */
static PFILE_OBJECT   g_ShadowFile = NULL;
static PDEVICE_OBJECT g_ShadowDevice = NULL;
static HANDLE         g_ShadowHandle = NULL;
static PULONG         g_BlockMap = NULL;      /* 原块号 -> 影子块号+1（0 = 未映射） */
static ULONG64        g_ShadowBlocks = 0;     /* 已分配影子块数 */
static ULONG64        g_ShadowCapBlocks = 0;  /* 影子容量（块） */
static PVOID          g_ShadowMemory = NULL;  /* 内存影子缓冲（P3） */
static ULONG64        g_ShadowMemSize = 0;

/* 1 = 对所有卷启用 COW（P3 测试期）。
   ⚠️ 生产环境必须精确匹配目标卷——分区 PDO 名可能是 \Device\Harddisk3\Partition3
   而不是 \Device\HarddiskVolume3，TgShadowQueryVolumeNumber 会解析出 0，导致
   "卷号不匹配、COW 从不触发"（实测症状：enable 成功但重定向块数恒为 0）。 */
static ULONG          g_MatchAllVolumes = 1;

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
static VOID TgShadowCloseStorage(VOID);

/**
 * 延迟释放全部影子资源（位图 + 块映射表 + 影子文件句柄）。
 * detach/停用后可能仍有 in-flight I/O 在使用它们，必须等一会儿再释放。
 * 调用方必须已把 g_Protected 置 0（保证不再有新的 COW 进入）。
 */
static VOID
TgShadowFreeMapDeferred(VOID)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -5000000;   /* 500ms */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    TgShadowFreeMap();              /* P2 位图 */

    if (g_ShadowMemory != NULL) {   /* P3 内存影子 */
        ExFreePoolWithTag(g_ShadowMemory, TGSHADOW_TAG);
        g_ShadowMemory = NULL;
        g_ShadowMemSize = 0;
    }
    if (g_BlockMap != NULL) {       /* P3 块映射表 */
        ExFreePoolWithTag(g_BlockMap, TGSHADOW_TAG);
        g_BlockMap = NULL;
        g_BlockCount = 0;
        g_ShadowBlocks = 0;
        g_ShadowCapBlocks = 0;
        g_MarkedBlocks = 0;
        g_ShadowBytesUsed = 0;
    }
    TgShadowCloseStorage();
    DbgPrint("[TgShadow] shadow resources released\n");
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

/* ================================================================ P3 影子存储

   架构：
     真实卷（受保护）                影子文件（另一卷上，预分配）
     ┌──────────────┐               ┌────────────────────┐
     │ 块 N 原始数据 │ ──首次写时──▶ │ 影子块 1（保存原数据）│
     └──────────────┘               ├────────────────────┤
                                    │ 影子块 2 ...        │
                                    └────────────────────┘

   映射表 g_BlockMap[原块号] = 影子块号+1（0 = 未映射 = 直接用真实卷数据）。
   写：块未映射 -> 先读真实卷原始块存入影子，再把新数据写进影子；真实卷不再改动。
   读：块已映射 -> 从影子读；否则透传真实卷。

   ⚠️ 影子文件所在卷绝不能是被保护的卷（会递归重定向）。
   ⚠️ 所有同步等待（KeWaitForSingleObject）要求 PASSIVE_LEVEL，
      因此重定向只在 PASSIVE_LEVEL 生效；更高 IRQL（如分页 I/O）直接透传。
   ================================================================ */

static VOID
TgShadowCloseStorage(VOID)
{
    if (g_ShadowFile != NULL) {
        ObDereferenceObject(g_ShadowFile);
        g_ShadowFile = NULL;
    }
    if (g_ShadowHandle != NULL) {
        ZwClose(g_ShadowHandle);
        g_ShadowHandle = NULL;
    }
    g_ShadowDevice = NULL;
}

/** 打开影子存储文件（PASSIVE_LEVEL）。路径为 NT 形式，如 \??\D:\tgshadow.bin */
static NTSTATUS
TgShadowOpenStorage(_In_ PCWSTR Path)
{
    UNICODE_STRING    name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   iosb;
    NTSTATUS          status;

    TgShadowCloseStorage();

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

#pragma warning(push)
#pragma warning(disable: 4996)
    status = ZwCreateFile(&g_ShadowHandle,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                          FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
#pragma warning(pop)
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] open shadow storage '%ws' failed 0x%08X\n", Path, status);
        g_ShadowHandle = NULL;
        return status;
    }

    status = ObReferenceObjectByHandle(g_ShadowHandle,
                                       FILE_READ_DATA | FILE_WRITE_DATA,
                                       *IoFileObjectType, KernelMode,
                                       (PVOID *)&g_ShadowFile, NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(g_ShadowHandle);
        g_ShadowHandle = NULL;
        g_ShadowFile = NULL;
        DbgPrint("[TgShadow] reference shadow file failed 0x%08X\n", status);
        return status;
    }
    g_ShadowDevice = IoGetRelatedDeviceObject(g_ShadowFile);
    DbgPrint("[TgShadow] shadow storage opened: %ws\n", Path);
    return STATUS_SUCCESS;
}

/** 分配块映射表（原块号 -> 影子块号） */
static NTSTATUS
TgShadowAllocBlockMap(_In_ ULONG64 VolumeBytes, _In_ ULONG64 ShadowBytes)
{
    ULONG64 blocks;
    SIZE_T  mapBytes;
    PVOID   map;

    if (g_BlockMap != NULL) {
        ExFreePoolWithTag(g_BlockMap, TGSHADOW_TAG);
        g_BlockMap = NULL;
    }
    blocks = VolumeBytes / TGSHADOW_BLOCK_SIZE;
    if (blocks == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (blocks > 0xFFFFFFFFULL) {
        blocks = 0xFFFFFFFFULL;
    }
    mapBytes = (SIZE_T)(blocks * sizeof(ULONG));

#pragma warning(push)
#pragma warning(disable: 4996)
    map = ExAllocatePoolWithTag(NonPagedPoolNx, mapBytes, TGSHADOW_TAG);
#pragma warning(pop)
    if (map == NULL) {
        DbgPrint("[TgShadow] block map alloc failed (%llu bytes)\n", (ULONG64)mapBytes);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(map, mapBytes);

    g_BlockMap = (PULONG)map;
    g_BlockCount = blocks;
    g_VolumeBytes = VolumeBytes;
    g_ShadowBlocks = 0;
    g_ShadowCapBlocks = ShadowBytes / TGSHADOW_BLOCK_SIZE;

    /* 分配内存影子缓冲（非分页，容量由用户态指定；测试建议 64~512MB） */
    if (g_ShadowMemory != NULL) {
        ExFreePoolWithTag(g_ShadowMemory, TGSHADOW_TAG);
        g_ShadowMemory = NULL;
        g_ShadowMemSize = 0;
    }
    if (ShadowBytes > 0) {
#pragma warning(push)
#pragma warning(disable: 4996)
        g_ShadowMemory = ExAllocatePoolWithTag(NonPagedPoolNx, (SIZE_T)ShadowBytes,
                                               TGSHADOW_TAG);
#pragma warning(pop)
        if (g_ShadowMemory == NULL) {
            DbgPrint("[TgShadow] shadow memory alloc failed (%llu MB)\n",
                     ShadowBytes / (1024 * 1024));
            ExFreePoolWithTag(map, TGSHADOW_TAG);
            g_BlockMap = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_ShadowMemory, (SIZE_T)ShadowBytes);
        g_ShadowMemSize = ShadowBytes;
    }

    DbgPrint("[TgShadow] block map ready: %llu blocks, shadow %llu MB (memory)\n",
             blocks, ShadowBytes / (1024 * 1024));
    return STATUS_SUCCESS;
}

/**
 * 影子存储读写 —— P3 采用【内存影子】。
 *
 * 为什么先用内存：
 *   1) VM 里只有一个可写卷（C:），影子文件放同卷会经本过滤设备再触发一次
 *      COW -> 无限递归；放内存完全没有这个问题。
 *   2) 内存复制不受 IRQL 限制（文件/IRP 方案必须在 PASSIVE_LEVEL 同步等待）。
 *   3) 重启即丢，天然符合"重启还原"语义，便于验证 P3 逻辑。
 * 生产环境（P4/P5）改为磁盘文件或专用分区，接口保持不变。
 */
static NTSTATUS
TgShadowStorageIo(_In_ BOOLEAN Write, _In_ ULONG64 Offset,
                  _In_ PVOID Buffer, _In_ ULONG Length)
{
    if (g_ShadowMemory == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Offset + Length > g_ShadowMemSize || Offset + Length < Offset) {
        return STATUS_DISK_FULL;
    }
    if (Write) {
        RtlCopyMemory((PUCHAR)g_ShadowMemory + Offset, Buffer, Length);
    } else {
        RtlCopyMemory(Buffer, (PUCHAR)g_ShadowMemory + Offset, Length);
    }
    return STATUS_SUCCESS;
}

/** 从受保护卷（下层设备）读取一段原始数据（PASSIVE_LEVEL） */
static NTSTATUS
TgShadowReadLower(_In_ PDEVICE_OBJECT LowerDev, _In_ ULONG64 Offset,
                  _In_ PVOID Buffer, _In_ ULONG Length)
{
    KEVENT          event;
    IO_STATUS_BLOCK iosb;
    PIRP            irp;
    LARGE_INTEGER   off;
    NTSTATUS        status;

    if (LowerDev == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    off.QuadPart = (LONGLONG)Offset;

#pragma warning(push)
#pragma warning(disable: 4996)
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, LowerDev, Buffer, Length,
                                       &off, &event, &iosb);
#pragma warning(pop)
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = IoCallDriver(LowerDev, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

/**
 * 写时复制：把 [Offset, Offset+Length) 的数据写入影子存储，真实卷保持不变。
 * 仅可在 PASSIVE_LEVEL 调用（内部有同步等待）。
 */
static NTSTATUS
TgShadowCowWrite(_In_ PDEVICE_OBJECT LowerDev, _In_ ULONG64 Offset,
                 _In_ PVOID Buffer, _In_ ULONG Length)
{
    PUCHAR   buf = (PUCHAR)Buffer;
    PUCHAR   tmp = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    /* ⚠️ P3 用内存影子时 g_ShadowFile 恒为 NULL，早期版本在这里直接返回
       STATUS_INVALID_DEVICE_STATE，导致 COW 静默失效（实测：进入 COW 判定 66 次、
       重定向块数恒为 0）。判据必须是"存储后端至少有一个可用"。 */
    if (g_BlockMap == NULL || (g_ShadowMemory == NULL && g_ShadowFile == NULL)) {
        return STATUS_INVALID_DEVICE_STATE;
    }

#pragma warning(push)
#pragma warning(disable: 4996)
    tmp = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, TGSHADOW_BLOCK_SIZE, TGSHADOW_TAG);
#pragma warning(pop)
    if (tmp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (Length > 0) {
        ULONG64 block = Offset / TGSHADOW_BLOCK_SIZE;
        ULONG   inBlock = (ULONG)(Offset % TGSHADOW_BLOCK_SIZE);
        ULONG   chunk = TGSHADOW_BLOCK_SIZE - inBlock;
        ULONG   shadowBlock;
        BOOLEAN newlyAllocated = FALSE;
        KIRQL   irql;

        if (chunk > Length) {
            chunk = Length;
        }
        if (block >= g_BlockCount) {
            break;
        }

        KeAcquireSpinLock(&g_Lock, &irql);
        shadowBlock = g_BlockMap[block];
        if (shadowBlock == 0) {
            /* 预留影子块号（1-based；0 表示未映射） */
            if (g_ShadowBlocks >= g_ShadowCapBlocks) {
                KeReleaseSpinLock(&g_Lock, irql);
                status = STATUS_DISK_FULL;
                DbgPrint("[TgShadow] shadow storage FULL\n");
                break;
            }
            g_ShadowBlocks++;
            shadowBlock = (ULONG)g_ShadowBlocks;
            g_BlockMap[block] = shadowBlock;
            g_MarkedBlocks++;
            g_ShadowBytesUsed += TGSHADOW_BLOCK_SIZE;
            newlyAllocated = TRUE;
        }
        KeReleaseSpinLock(&g_Lock, irql);

        /* 新分配的影子块：先把真实卷的原始数据搬进影子（COW 的关键一步），
           之后真实卷这块就再也不会被写。 */
        if (newlyAllocated) {
            status = TgShadowReadLower(LowerDev, block * TGSHADOW_BLOCK_SIZE,
                                       tmp, TGSHADOW_BLOCK_SIZE);
            if (!NT_SUCCESS(status)) {
                DbgPrint("[TgShadow] COW read original block %llu failed 0x%08X\n",
                         block, status);
                break;
            }
            status = TgShadowStorageIo(TRUE,
                                       (ULONG64)(shadowBlock - 1) * TGSHADOW_BLOCK_SIZE,
                                       tmp, TGSHADOW_BLOCK_SIZE);
            if (!NT_SUCCESS(status)) {
                DbgPrint("[TgShadow] COW save original block failed 0x%08X\n", status);
                break;
            }
        }

        /* 写新数据到影子块内对应偏移 */
        status = TgShadowStorageIo(TRUE,
                                   (ULONG64)(shadowBlock - 1) * TGSHADOW_BLOCK_SIZE + inBlock,
                                   buf, chunk);
        if (!NT_SUCCESS(status)) {
            DbgPrint("[TgShadow] COW write data failed 0x%08X\n", status);
            break;
        }

        Offset += chunk;
        buf += chunk;
        Length -= chunk;
    }

    ExFreePoolWithTag(tmp, TGSHADOW_TAG);
    return status;
}

/** 检查 [Offset, Offset+Length) 范围内是否存在已映射块（读路径的性能闸门） */
static BOOLEAN
TgShadowRangeHasMapping(_In_ ULONG64 Offset, _In_ ULONG Length)
{
    ULONG64 startBlock, endBlock, i;
    BOOLEAN found = FALSE;
    KIRQL   irql;

    if (g_BlockMap == NULL || g_BlockCount == 0) {
        return FALSE;
    }
    startBlock = Offset / TGSHADOW_BLOCK_SIZE;
    if (startBlock >= g_BlockCount) {
        return FALSE;
    }
    endBlock = (Offset + (ULONG64)Length - 1) / TGSHADOW_BLOCK_SIZE;
    if (endBlock >= g_BlockCount) {
        endBlock = g_BlockCount - 1;
    }
    KeAcquireSpinLock(&g_Lock, &irql);
    for (i = startBlock; i <= endBlock; i++) {
        if (g_BlockMap[i] != 0) {
            found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&g_Lock, irql);
    return found;
}

/**
 * COW 读：逐块判断——已映射从影子读，未映射从真实卷读。
 * 仅可在 PASSIVE_LEVEL 调用。
 */
static NTSTATUS
TgShadowCowRead(_In_ PDEVICE_OBJECT LowerDev, _In_ ULONG64 Offset,
                _In_ PVOID Buffer, _In_ ULONG Length)
{
    PUCHAR   buf = (PUCHAR)Buffer;
    NTSTATUS status = STATUS_SUCCESS;

    if (g_BlockMap == NULL || (g_ShadowMemory == NULL && g_ShadowFile == NULL)) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    while (Length > 0) {
        ULONG64 block = Offset / TGSHADOW_BLOCK_SIZE;
        ULONG   inBlock = (ULONG)(Offset % TGSHADOW_BLOCK_SIZE);
        ULONG   chunk = TGSHADOW_BLOCK_SIZE - inBlock;
        ULONG   shadowBlock;
        KIRQL   irql;

        if (chunk > Length) {
            chunk = Length;
        }
        if (block >= g_BlockCount) {
            break;
        }

        KeAcquireSpinLock(&g_Lock, &irql);
        shadowBlock = g_BlockMap[block];
        KeReleaseSpinLock(&g_Lock, irql);

        if (shadowBlock != 0) {
            status = TgShadowStorageIo(FALSE,
                                       (ULONG64)(shadowBlock - 1) * TGSHADOW_BLOCK_SIZE + inBlock,
                                       buf, chunk);
        } else {
            status = TgShadowReadLower(LowerDev, Offset, buf, chunk);
        }
        if (!NT_SUCCESS(status)) {
            DbgPrint("[TgShadow] COW read failed at %llu len %lu: 0x%08X\n",
                     Offset, chunk, status);
            break;
        }

        Offset += chunk;
        buf += chunk;
        Length -= chunk;
    }
    return status;
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
    BOOLEAN            isWrite;
    LARGE_INTEGER      off;
    ULONG              len;
    PVOID              buffer = NULL;

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

    isWrite = (stack->MajorFunction == IRP_MJ_WRITE);
    if (isWrite) {
        off = stack->Parameters.Write.ByteOffset;
        len = stack->Parameters.Write.Length;
        InterlockedIncrement64((volatile LONG64 *)&ext->WriteCount);
        InterlockedIncrement64((volatile LONG64 *)&g_TotalWrites);
        if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
            InterlockedIncrement64((volatile LONG64 *)&g_WritesPassive);
        } else {
            InterlockedIncrement64((volatile LONG64 *)&g_WritesHighIrql);
        }
    } else {
        off = stack->Parameters.Read.ByteOffset;
        len = stack->Parameters.Read.Length;
        InterlockedIncrement64((volatile LONG64 *)&ext->ReadCount);
        InterlockedIncrement64((volatile LONG64 *)&g_TotalReads);
    }

    /* ---- P3：写时复制重定向 ----
       条件：已启用保护 + 块映射表就绪 + 是这个受保护卷 + PASSIVE_LEVEL
       （同步等待要求 PASSIVE；更高 IRQL 如分页 I/O 直接透传）。
       写：一律重定向到影子，真实卷保持不变。
       读：只有范围内存在已映射块才绕行（否则透传，避免性能损失）。 */
    if (g_Protected && g_MarkWrites && g_BlockMap != NULL &&
        (g_MatchAllVolumes || ext->VolumeNumber == g_TargetVolume) &&
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
        off.QuadPart >= 0 && len > 0) {

        BOOLEAN needCow = isWrite ? TRUE
                                  : TgShadowRangeHasMapping((ULONG64)off.QuadPart, len);

        if (needCow) {
            InterlockedIncrement64((volatile LONG64 *)&g_CowEntered);
            if (Irp->MdlAddress != NULL) {
                buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
            } else if (Irp->AssociatedIrp.SystemBuffer != NULL) {
                buffer = Irp->AssociatedIrp.SystemBuffer;
            } else {
                InterlockedIncrement64((volatile LONG64 *)&g_CowNoBuffer);
            }

            if (buffer != NULL) {
                NTSTATUS cowStatus = isWrite
                    ? TgShadowCowWrite(ext->LowerDevice, (ULONG64)off.QuadPart, buffer, len)
                    : TgShadowCowRead(ext->LowerDevice, (ULONG64)off.QuadPart, buffer, len);

                if (NT_SUCCESS(cowStatus)) {
                    /* 影子处理成功：真实卷不再被写 / 读直接返回影子数据 */
                    if (isWrite) {
                        InterlockedIncrement64((volatile LONG64 *)&g_CowWriteOk);
                    } else {
                        InterlockedIncrement64((volatile LONG64 *)&g_CowReadOk);
                    }
                    Irp->IoStatus.Status = STATUS_SUCCESS;
                    Irp->IoStatus.Information = len;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return STATUS_SUCCESS;
                }
                /* 失败则落回透传（保守：宁可这次不被保护，也不能丢数据） */
                InterlockedIncrement64((volatile LONG64 *)&g_CowFailed);
                g_CowLastStatus = (ULONG)cowStatus;
                DbgPrint("[TgShadow] COW %s failed 0x%08X -> passthrough\n",
                         isWrite ? "write" : "read", cowStatus);
            }
        }
    } else if (isWrite && g_Protected &&
               (g_MatchAllVolumes || ext->VolumeNumber == g_TargetVolume) &&
               off.QuadPart >= 0 && len > 0) {
        /* 高 IRQL 等场景：退化为 P2 的位图记录（如果位图存在） */
        TgShadowMarkWriteRange((ULONG64)off.QuadPart, len);
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
    out->WritesPassive      = g_WritesPassive;
    out->WritesHighIrql     = g_WritesHighIrql;
    out->CowEntered         = g_CowEntered;
    out->CowNoBuffer        = g_CowNoBuffer;
    out->CowWriteOk         = g_CowWriteOk;
    out->CowReadOk          = g_CowReadOk;
    out->CowFailed          = g_CowFailed;
    out->CowLastStatus      = g_CowLastStatus;
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

    /* 清掉上一轮的保护与影子资源 */
    g_Protected = 0;
    TgShadowFreeMapDeferred();

    if (g_MarkWrites) {
        ULONG64 volumeBytes = in->VolumeBytes;
        ULONG64 shadowBytes = in->ShadowBytes;

        if (volumeBytes == 0) {
            volumeBytes = 32ULL * 1024 * 1024 * 1024;
        }
        if (shadowBytes == 0) {
            shadowBytes = volumeBytes;
        }

        /* 打开影子存储文件（用户态已预分配好容量） */
        if (in->ShadowPath[0] != L'\0') {
            TgShadowTrace(L"enable: opening shadow storage");
            status = TgShadowOpenStorage(in->ShadowPath);
            if (!NT_SUCCESS(status)) {
                TgShadowTrace(L"enable: open shadow storage FAILED");
                return status;
            }
        } else {
            /* 未提供影子路径：只建立映射表（无存储时 COW 会失败并透传） */
            TgShadowTrace(L"enable: no shadow path supplied");
        }

        /* 块映射表：原块号 -> 影子块号 */
        status = TgShadowAllocBlockMap(volumeBytes, shadowBytes);
        if (!NT_SUCCESS(status)) {
            TgShadowTrace(L"enable: block map alloc FAILED");
            return status;
        }
    }

    g_TargetVolume = in->VolumeNumber;
    g_Protected = 1;
    TgShadowTrace(L"enable: DONE (COW armed)");

    DbgPrint("[TgShadow] protection ON: vol=%lu markWrites=%lu filters=%ld\n",
             g_TargetVolume, g_MarkWrites, g_AttachedDevices);
    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
TgShadowDisable(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    UNREFERENCED_PARAMETER(Stack);
    g_Protected = 0;                 /* 先停用：不再有新的 COW 进入 */
    TgShadowFreeMapDeferred();       /* 再延迟释放位图/映射表/影子句柄 */
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

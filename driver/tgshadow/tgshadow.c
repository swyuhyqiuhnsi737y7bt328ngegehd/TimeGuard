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
    ULONG           IsBootVolume;   /* 1 = 承载当前运行系统的卷 */
    PDEVICE_OBJECT  Pdo;            /* 本过滤设备所挂载的卷 PDO（精确匹配用） */
    ULONG64         ReadCount;
    ULONG64         WriteCount;
} TGSHADOW_DEV_EXT, *PTGSHADOW_DEV_EXT;

#define TGSHADOW_EXT(d) ((PTGSHADOW_DEV_EXT)((d)->DeviceExtension))

static PDEVICE_OBJECT g_ControlDevice = NULL;
static PDEVICE_OBJECT g_TargetDevice = NULL;      /* 受保护卷对应的过滤设备（精确匹配） */
static PDEVICE_OBJECT g_SystemVolumeDevice = NULL;/* C: 对应的卷设备对象 */
static PFILE_OBJECT   g_SystemVolumeFile = NULL;  /* 持有引用，防止设备对象消失 */
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
static ULONG64        g_PagingIo = 0;      /* 分页 I/O（一律透传，见下） */
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
static ULONG64        g_ShadowFileBytes = 0;  /* 影子文件容量（由用户态传入并校验） */
static PDEVICE_OBJECT g_ShadowVolumeFilter = NULL; /* 影子所在卷的过滤设备（必须排除 COW） */

/* P4：过滤设备索引。Upper Filter 会给【每个】卷都建一个过滤设备，
   提交/丢弃时必须找到"受保护卷"对应的那个，否则会写错卷（灾难性）。
   按卷号索引，AddDevice 填充、REMOVE_DEVICE 清空。 */
#define TGSHADOW_MAX_VOLUMES 32
static PDEVICE_OBJECT g_VolumeDevices[TGSHADOW_MAX_VOLUMES];
static ULONG          g_VolumeNumbers[TGSHADOW_MAX_VOLUMES];
static KSPIN_LOCK     g_VolumeLock;

/* P4 统计 */
static ULONG64        g_CommittedBlocks = 0;   /* 提交回真实卷的块数 */
static ULONG64        g_DiscardedBlocks = 0;   /* 丢弃的块数 */
static ULONG64        g_LastCommitFailed = 0;  /* 上次提交失败的块数 */

/* 1 = 对所有卷启用 COW（P3 测试期）。
   ⚠️ 生产环境必须精确匹配目标卷——分区 PDO 名可能是 \Device\Harddisk3\Partition3
   而不是 \Device\HarddiskVolume3，TgShadowQueryVolumeNumber 会解析出 0，导致
   "卷号不匹配、COW 从不触发"（实测症状：enable 成功但重定向块数恒为 0）。 */
static ULONG          g_MatchAllVolumes = 0;

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
static VOID TgShadowResolveSystemVolume(VOID);
static BOOLEAN TgShadowIsSystemVolume(_In_ PDEVICE_OBJECT Pdo);
static PDEVICE_OBJECT TgShadowFindFilterByVpb(_In_ PVPB Vpb);
static VOID TgShadowTraceValue(_In_ PCWSTR Name, _In_ PCWSTR Value);
static BOOLEAN TgShadowIsSpecialIo(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack);
static PDEVICE_OBJECT TgShadowFindFilterByDosName(_In_ PCWSTR Want);
static PDEVICE_OBJECT TgShadowFindFilterByVolumeNumber(_In_ ULONG VolumeNumber);
static ULONG TgShadowSystemVolumeNumber(VOID);
static VOID TgShadowDumpVolumes(VOID);

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
    /* 后端一：内存影子（容量受物理内存限制，只适合短时间验证） */
    if (g_ShadowMemory != NULL) {
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

    /* 后端二：磁盘影子文件（推荐）。
       文件必须放在【不受保护的卷】上，且该卷必须被排除在 COW 之外，
       否则每次写影子都会再触发一次 COW -> 无限递归。
       文件是带缓存打开的，所以这里允许任意偏移/长度（不必按扇区对齐），
       真正落盘由文件系统的延迟写线程在别处完成——那条路径走的是影子卷，
       已被排除，不会回到本函数。 */
    if (g_ShadowFile != NULL && g_ShadowHandle != NULL) {
        LARGE_INTEGER   off;
        IO_STATUS_BLOCK iosb;
        NTSTATUS        status;

        if (Offset + Length > g_ShadowFileBytes || Offset + Length < Offset) {
            return STATUS_DISK_FULL;
        }
        off.QuadPart = (LONGLONG)Offset;
        if (Write) {
            status = ZwWriteFile(g_ShadowHandle, NULL, NULL, NULL, &iosb,
                                 Buffer, Length, &off, NULL);
        } else {
            status = ZwReadFile(g_ShadowHandle, NULL, NULL, NULL, &iosb,
                                Buffer, Length, &off, NULL);
        }
        if (NT_SUCCESS(status) && iosb.Information != Length) {
            /* 影子文件被截断/预分配不足：当作容量不足处理，避免静默丢数据 */
            return STATUS_DISK_FULL;
        }
        return status;
    }

    return STATUS_INVALID_DEVICE_STATE;
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

/* -------------------------------------------------- 特殊文件（不参与保护） */

/* ntddk.h 里没有导出这个原型，自己声明；不声明会被当成返回 int（x64 下隐患） */
NTKERNELAPI
NTSTATUS
NTAPI
ObQueryNameString(
    _In_ PFILE_OBJECT FileObject,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );


static PFILE_OBJECT g_SpecialFiles[8] = { 0 };   /* 页面文件等：只缓存指针，比较极快 */
/* 0 = 不做特殊文件排除（默认，与已验证版本一致）；1 = 启用（实验性，会取对象锁） */
static ULONG g_ExcludeSpecialIo = 0;

static BOOLEAN
TgShadowNameHasSpecial(_In_ PUNICODE_STRING Name)
{
    static const WCHAR *keys[] = { L"pagefile.sys", L"hiberfil.sys", L"swapfile.sys" };
    ULONG k, i, j;
    ULONG chars = Name->Length / sizeof(WCHAR);

    for (k = 0; k < 3; k++) {
        ULONG keyLen = 0;
        while (keys[k][keyLen] != L'\0') {
            keyLen++;
        }
        if (chars < keyLen) {
            continue;
        }
        for (i = 0; i + keyLen <= chars; i++) {
            for (j = 0; j < keyLen; j++) {
                if (RtlUpcaseUnicodeChar(Name->Buffer[i + j]) !=
                    RtlUpcaseUnicodeChar(keys[k][j])) {
                    break;
                }
            }
            if (j == keyLen) {
                return TRUE;      /* 命中 pagefile.sys / hiberfil.sys / swapfile.sys */
            }
        }
    }
    return FALSE;
}

/**
 * 判断这次 I/O 是否属于"绝不能进影子"的特殊文件。
 * 返回 TRUE 时调用方必须原样透传。
 *
 * 为什么必须排除页面文件：它的内容不需要还原语义，但写入量极大且发生在内存
 * 压力下；把它纳入 COW 会迅速吃光影子、并让内存回收路径卡在自己的 I/O 上。
 * 命中过的文件对象会被记下来，后续比较就是几次指针比较。
 */
static BOOLEAN
TgShadowIsSpecialIo(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    PFILE_OBJECT fo;
    ULONG        i;
    UCHAR        nameBuf[512];
    POBJECT_NAME_INFORMATION info = (POBJECT_NAME_INFORMATION)nameBuf;
    NTSTATUS     st;

    if ((Irp->Flags & IRP_PAGING_IO) == 0) {
        return FALSE;               /* 非分页 I/O 一律正常处理 */
    }
    fo = Stack->FileObject;
    if (fo == NULL) {
        fo = Irp->Tail.Overlay.OriginalFileObject;
    }
    if (fo == NULL) {
        return TRUE;                /* 拿不到文件对象，无法判断 -> 保守透传 */
    }
    for (i = 0; i < ARRAYSIZE(g_SpecialFiles); i++) {
        if (g_SpecialFiles[i] == fo) {
            return TRUE;            /* 之前已确认是页面文件 */
        }
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return TRUE;                /* 高 IRQL 查不了名字 -> 保守透传 */
    }
    st = ObQueryNameString(fo, info, sizeof(nameBuf), NULL);
    if (NT_SUCCESS(st) && info->Name.Buffer != NULL &&
        TgShadowNameHasSpecial(&info->Name)) {
        for (i = 0; i < ARRAYSIZE(g_SpecialFiles); i++) {
            if (g_SpecialFiles[i] == NULL) {
                g_SpecialFiles[i] = fo;
                DbgPrint("[TgShadow] 记录特殊文件（不参与影子保护）：%wZ\n", &info->Name);
                break;
            }
        }
        return TRUE;
    }
    return FALSE;
}

/** 把一段数据写回受保护卷（同步，PASSIVE_LEVEL）——仅提交时使用 */
static NTSTATUS
TgShadowWriteLower(_In_ PDEVICE_OBJECT LowerDev, _In_ ULONG64 Offset,
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
    irp = IoBuildSynchronousFsdRequest(IRP_MJ_WRITE, LowerDev, Buffer, Length,
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
 * 找出承载当前运行系统的卷号。
 *
 * 用户态查不到：服务运行在会话 0，QueryDosDevice(L"C:") 在那里拿不到映射。
 * 内核侧也不用 IoGetBootDiskVolume（WDK 里没有导出，链接会失败），
 * 而是解析 \SystemRoot 符号链接——它总是指向 \Device\HarddiskVolumeN。
 * 必须在 PASSIVE_LEVEL 调用（IOCTL 分发满足）。
 */
static ULONG
TgShadowBootVolumeNumber(VOID)
{
    UNICODE_STRING    name, target;
    OBJECT_ATTRIBUTES oa;
    HANDLE            hLink = NULL;
    NTSTATUS          status;
    WCHAR             buf[512];
    ULONG             i, n = 0;
    BOOLEAN           inDigits = FALSE;

    RtlInitUnicodeString(&name, L"\\SystemRoot");
    InitializeObjectAttributes(&oa, &name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenSymbolicLinkObject(&hLink, GENERIC_READ, &oa);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] 打开 \\SystemRoot 失败 0x%08X\n", status);
        return 0;
    }
    target.Buffer = buf;
    target.Length = 0;
    target.MaximumLength = sizeof(buf) - sizeof(WCHAR);
    status = ZwQuerySymbolicLinkObject(hLink, &target, NULL);
    ZwClose(hLink);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[TgShadow] 查询 \\SystemRoot 失败 0x%08X\n", status);
        return 0;
    }
    buf[target.Length / sizeof(WCHAR)] = L'\0';

    /* 形如 \Device\HarddiskVolume3：取 "HarddiskVolume" 之后的十进制数字 */
    for (i = 0; i + 13 <= target.Length / sizeof(WCHAR); i++) {
        if (wcsncmp(&buf[i], L"HarddiskVolume", 13) == 0) {
            i += 13;
            while (i < target.Length / sizeof(WCHAR) &&
                   buf[i] >= L'0' && buf[i] <= L'9') {
                n = n * 10 + (ULONG)(buf[i] - L'0');
                i++;
                inDigits = TRUE;
            }
            break;
        }
    }
    DbgPrint("[TgShadow] \\SystemRoot -> %ws (卷号 %lu, parsed=%d)\n",
             buf, n, inDigits);
    return inDigits ? n : 0;
}

/** 按 VPB 找过滤设备：影子文件所在卷与各卷 PDO 共享同一个 VPB */
static PDEVICE_OBJECT
TgShadowFindFilterByVpb(_In_ PVPB Vpb)
{
    ULONG i;
    PDEVICE_OBJECT found = NULL;
    KIRQL irql;

    if (Vpb == NULL) {
        return NULL;
    }
    KeAcquireSpinLock(&g_VolumeLock, &irql);
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        if (g_VolumeDevices[i] != NULL &&
            TGSHADOW_EXT(g_VolumeDevices[i])->Pdo != NULL &&
            TGSHADOW_EXT(g_VolumeDevices[i])->Pdo->Vpb == Vpb) {
            found = g_VolumeDevices[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_VolumeLock, irql);
    return found;
}

/** 按卷号找过滤设备（PDO 名解析出的卷号，实测可靠） */
static PDEVICE_OBJECT
TgShadowFindFilterByVolumeNumber(_In_ ULONG VolumeNumber)
{
    ULONG i;
    PDEVICE_OBJECT found = NULL;
    KIRQL irql;

    if (VolumeNumber == 0) {
        return NULL;
    }
    KeAcquireSpinLock(&g_VolumeLock, &irql);
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        if (g_VolumeDevices[i] != NULL &&
            TGSHADOW_EXT(g_VolumeDevices[i])->VolumeNumber == VolumeNumber) {
            found = g_VolumeDevices[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_VolumeLock, irql);
    return found;
}

/** 找出 DOS 名为 C: 的那个卷的卷号（"C:" 与 "\??\C:" 都认） */
static ULONG
TgShadowSystemVolumeNumber(VOID)
{
    PDEVICE_OBJECT dev;
    ULONG          num = 0;

    dev = TgShadowFindFilterByDosName(L"C:");
    if (dev == NULL) {
        dev = TgShadowFindFilterByDosName(L"\\??\\C:");
    }
    if (dev != NULL) {
        num = TGSHADOW_EXT(dev)->VolumeNumber;
    }
    return num;
}

/** 按卷设备对象查找对应的过滤设备 */
static PDEVICE_OBJECT
TgShadowFindFilterByPdo(_In_ PDEVICE_OBJECT Pdo)
{
    ULONG i;
    PDEVICE_OBJECT found = NULL;
    KIRQL irql;

    KeAcquireSpinLock(&g_VolumeLock, &irql);
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        if (g_VolumeDevices[i] != NULL &&
            TGSHADOW_EXT(g_VolumeDevices[i])->Pdo == Pdo) {
            found = g_VolumeDevices[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_VolumeLock, irql);
    return found;
}

/** 找出承载当前运行系统的过滤设备（AddDevice 已按设备对象比对标记） */
static PDEVICE_OBJECT
TgShadowFindBootFilter(VOID)
{
    ULONG i;
    PDEVICE_OBJECT found = NULL;
    KIRQL irql;

    KeAcquireSpinLock(&g_VolumeLock, &irql);
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        if (g_VolumeDevices[i] != NULL &&
            TGSHADOW_EXT(g_VolumeDevices[i])->IsBootVolume) {
            found = g_VolumeDevices[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_VolumeLock, irql);
    return found;
}

/** 按卷号查找过滤设备（Upper Filter 对每个卷都会建一个） */
static PTGSHADOW_DEV_EXT
TgShadowFindVolumeExt(_In_ ULONG VolumeNumber)
{
    ULONG i;
    PTGSHADOW_DEV_EXT found = NULL;
    KIRQL irql;

    KeAcquireSpinLock(&g_VolumeLock, &irql);
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        if (g_VolumeDevices[i] != NULL && g_VolumeNumbers[i] == VolumeNumber) {
            found = TGSHADOW_EXT(g_VolumeDevices[i]);
            break;
        }
    }
    KeReleaseSpinLock(&g_VolumeLock, irql);
    return found;
}

/**
 * 提交：把影子里的数据写回真实卷（家长确认"保留本次修改"）。
 *
 * 顺序至关重要：
 *   1) 先停用保护 —— 之后的写直接落真实卷（保证系统最终状态一致）；
 *   2) 等 500ms 让 in-flight COW 落定（否则可能漏掉最后几个块）；
 *   3) 逐块把影子数据写回真实卷；
 *   4) 清空映射表（数据已合并，影子不再需要）。
 * 任一块失败都继续处理其余块，最后返回失败块数，避免"提交一半就放弃"。
 */
static NTSTATUS
TgShadowCommit(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    PUCHAR   tmp;
    ULONG64  i, committed = 0, failed = 0;
    NTSTATUS status = STATUS_SUCCESS;
    PTGSHADOW_DEV_EXT ext;
    LARGE_INTEGER interval;
    KIRQL    irql;
    ULONG    vol = g_TargetVolume;

    UNREFERENCED_PARAMETER(Stack);

    if (g_BlockMap == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (g_TargetDevice != NULL) {
        ext = TGSHADOW_EXT(g_TargetDevice);
    } else {
        ext = TgShadowFindVolumeExt(vol);
    }
    if (ext == NULL || ext->LowerDevice == NULL) {
        DbgPrint("[TgShadow] commit: 找不到受保护卷的设备（卷 %lu）\n", vol);
        return STATUS_DEVICE_NOT_READY;
    }

    /* 1) 停用保护，2) 等待 in-flight COW 结束 */
    g_Protected = 0;
    interval.QuadPart = -5000000;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    DbgPrint("[TgShadow] commit start: %llu mapped blocks\n", g_ShadowBlocks);

#pragma warning(push)
#pragma warning(disable: 4996)
    tmp = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, TGSHADOW_BLOCK_SIZE, TGSHADOW_TAG);
#pragma warning(pop)
    if (tmp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < g_BlockCount; i++) {
        ULONG shadowBlock;
        BOOLEAN mapped;

        KeAcquireSpinLock(&g_Lock, &irql);
        shadowBlock = g_BlockMap[i];
        KeReleaseSpinLock(&g_Lock, irql);
        if (shadowBlock == 0) {
            continue;
        }

        status = TgShadowStorageIo(FALSE,
                                   (ULONG64)(shadowBlock - 1) * TGSHADOW_BLOCK_SIZE,
                                   tmp, TGSHADOW_BLOCK_SIZE);
        if (NT_SUCCESS(status)) {
            status = TgShadowWriteLower(ext->LowerDevice, i * TGSHADOW_BLOCK_SIZE,
                                        tmp, TGSHADOW_BLOCK_SIZE);
        }
        if (!NT_SUCCESS(status)) {
            failed++;
            DbgPrint("[TgShadow] commit block %llu failed 0x%08X\n", i, status);
            continue;
        }
        committed++;

        /* 写回成功即解除映射 */
        KeAcquireSpinLock(&g_Lock, &irql);
        mapped = (g_BlockMap[i] != 0);
        if (mapped) {
            g_BlockMap[i] = 0;
        }
        KeReleaseSpinLock(&g_Lock, irql);
    }

    ExFreePoolWithTag(tmp, TGSHADOW_TAG);

    g_CommittedBlocks += committed;
    g_LastCommitFailed = failed;
    /* ⚠️ METHOD_BUFFERED 下 Information 是"回拷到用户缓冲区的字节数"，
       这里没有输出缓冲区，必须置 0；块数通过 GET_STATUS 暴露。 */
    Irp->IoStatus.Information = 0;
    DbgPrint("[TgShadow] commit done: %llu ok, %llu failed\n", committed, failed);

    return failed == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/**
 * 丢弃：放弃本次全部改动（家长未确认 / 超时 / 主动还原）。
 * 停用保护并释放影子资源；真实卷从未被写，因此重启后自然回到原状。
 */
static NTSTATUS
TgShadowDiscard(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    ULONG64 discarded;

    UNREFERENCED_PARAMETER(Stack);

    g_Protected = 0;
    discarded = g_ShadowBlocks;
    g_DiscardedBlocks += discarded;

    TgShadowFreeMapDeferred();      /* 内部先等 500ms 再释放 */

    Irp->IoStatus.Information = 0;  /* 同上：不能用 Information 传块数 */
    DbgPrint("[TgShadow] discard done: %llu blocks dropped\n", discarded);
    return STATUS_SUCCESS;
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

    /* ---- P4 安全阀：只排除页面文件/休眠文件，不是"所有分页 I/O" ----
       ⚠️ 这里踩过一个很深的坑：一度写成"IRP_PAGING_IO 一律透传"，结果
       【普通文件写入全部不再被保护】——因为 NTFS 的缓存写入正是由延迟写线程以
       分页 I/O 下发的（实测：文件写入 2MB，重定向块数恒为 0，"分页 I/O 透传"
       计数却暴涨）。正确做法是看【文件是谁】：只放行页面文件/休眠文件/
       交换文件，其余分页 I/O（包括用户文件）照常走 COW。 */
    /* ⚠️ 实验开关：TgShadowIsSpecialIo 内部用 ObQueryNameString 在 I/O 路径里查
       文件名，实测会把卷 I/O 堵死（guest 只剩下 RPC 有响应，任何读盘操作挂起）。
       暂时关闭，让所有 I/O 都走下面的正常 COW 分支——这与 P3/P4 已验证通过的
       版本语义一致。页面文件排除改由 P5 用不取锁的方式实现。 */
    if (g_ExcludeSpecialIo && TgShadowIsSpecialIo(Irp, stack)) {
        InterlockedIncrement64((volatile LONG64 *)&g_PagingIo);
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }

    /* 影子文件所在卷必须完全排除，否则写影子会再次触发 COW -> 无限递归 */
    if (ext->Self == g_ShadowVolumeFilter) {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }

    /* ---- P3：写时复制重定向 ----
       条件：已启用保护 + 块映射表就绪 + 是这个受保护卷 + PASSIVE_LEVEL
       （同步等待要求 PASSIVE；更高 IRQL 直接透传）。
       写：一律重定向到影子，真实卷保持不变。
       读：只有范围内存在已映射块才绕行（否则透传，避免性能损失）。 */
    if (g_Protected && g_MarkWrites && g_BlockMap != NULL &&
        (g_MatchAllVolumes || ext->Self == g_TargetDevice) &&
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
               (g_MatchAllVolumes || ext->Self == g_TargetDevice) &&
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
        KIRQL    irql;
        ULONG    k;

        /* 先摘掉索引，避免提交/丢弃拿到正在删除的设备 */
        KeAcquireSpinLock(&g_VolumeLock, &irql);
        for (k = 0; k < TGSHADOW_MAX_VOLUMES; k++) {
            if (g_VolumeDevices[k] == DeviceObject) {
                g_VolumeDevices[k] = NULL;
                g_VolumeNumbers[k] = 0;
                break;
            }
        }
        KeReleaseSpinLock(&g_VolumeLock, irql);

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
    out->CommittedBlocks    = (ULONG)g_CommittedBlocks;
    out->DiscardedBlocks    = (ULONG)g_DiscardedBlocks;
    out->LastCommitFailed   = (ULONG)g_LastCommitFailed;
    out->PagingIo           = (ULONG)g_PagingIo;
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
    PDEVICE_OBJECT filter = NULL;      /* 受保护卷对应的过滤设备 */

    TgShadowTrace(L"enable: IOCTL received");
    if (Stack->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(TGSHADOW_ENABLE_INPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    in = (PTGSHADOW_ENABLE_INPUT)Irp->AssociatedIrp.SystemBuffer;

    g_MarkWrites = (in->Flags & TGSHADOW_FLAG_ATTACH_ONLY) ? 0 : 1;
    g_MatchAllVolumes = (in->Flags & TGSHADOW_FLAG_ALL_VOLUMES) ? 1 : 0;

    /* 确定受保护卷对应的过滤设备。
       卷号 0 = 自动识别系统卷（用户态在 SYSTEM 会话里算不出 C: 的卷号）。 */
    {
        if (in->VolumeNumber == 0) {
            /* enable 一定发生在系统启动完成后，此时 DOS 名才有；先取证再匹配 */
            TgShadowDumpVolumes();
            /* IoVolumeDeviceToDosName 对带盘符的卷返回的是 "C:"（不带 \??\ 前缀），
               实测：vol=3 -> dos=C:。两种形式都试一遍，避免白跑一轮部署。 */
            filter = TgShadowFindFilterByDosName(L"C:");
            if (filter == NULL) {
                filter = TgShadowFindFilterByDosName(L"\\??\\C:");
            }
            if (filter == NULL) {
                /* 再退一步：用 PDO 名解析出的卷号 + 驱动自己的卷清单（实测可靠） */
                filter = TgShadowFindFilterByVolumeNumber(TgShadowSystemVolumeNumber());
            }
            if (filter == NULL && g_SystemVolumeDevice != NULL) {
                TgShadowResolveSystemVolume();
                filter = TgShadowFindFilterByPdo(g_SystemVolumeDevice);
            }
            if (filter == NULL) {
                filter = TgShadowFindBootFilter();   /* AddDevice 时已标记的兜底 */
            }
            if (filter == NULL) {
                DbgPrint("[TgShadow] enable: 找不到系统卷的过滤设备"
                         "（系统卷设备=%p，已挂载=%ld）\n",
                         g_SystemVolumeDevice, g_AttachedDevices);
                TgShadowTrace(L"enable: boot filter NOT FOUND");
                TgShadowTraceValue(L"TgShadowEnableErr", L"系统卷过滤设备未找到");
                return STATUS_DEVICE_NOT_READY;
            }
            in->VolumeNumber = TGSHADOW_EXT(filter)->VolumeNumber;
            DbgPrint("[TgShadow] enable: 自动识别系统卷 = %lu\n", in->VolumeNumber);
        } else {
            /* 显式指定卷号：用 AddDevice 时解析出的卷号匹配。
               实测每个卷的 PDO 名都是 \Device\HarddiskVolumeN，解析可靠
               （注册表取证：vol=1 / vol=2 / vol=3 全部正确）。
               注意【不能】用 IoGetDeviceObjectPointer("\\Device\\HarddiskVolumeN")
               再和 PDO 比指针：那个 API 返回的是文件系统设备对象（栈顶），
               和卷 PDO 不是同一个对象，永远比不中。 */
            filter = TgShadowFindFilterByVolumeNumber(in->VolumeNumber);
            if (filter == NULL) {
                DbgPrint("[TgShadow] enable: 卷 %lu 没有对应的过滤设备"
                         "（已挂载 %ld 个）\n", in->VolumeNumber, g_AttachedDevices);
                TgShadowTrace(L"enable: volume filter NOT FOUND");
                return STATUS_DEVICE_NOT_READY;
            }
        }
        g_TargetDevice = filter;
        DbgPrint("[TgShadow] enable: 目标过滤设备=%p 卷号=%lu\n",
                 filter, in->VolumeNumber);
    }

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
            FILE_STANDARD_INFORMATION fsi;
            IO_STATUS_BLOCK         iosb;

            TgShadowTrace(L"enable: opening shadow storage");
            status = TgShadowOpenStorage(in->ShadowPath);
            if (!NT_SUCCESS(status)) {
                TgShadowTrace(L"enable: open shadow storage FAILED");
                return status;
            }
            /* 影子容量取【文件实际大小】与用户声明值的较小者：
               宁可早一点报"影子满"，也不能写到文件外面去。 */
            RtlZeroMemory(&fsi, sizeof(fsi));
            status = ZwQueryInformationFile(g_ShadowHandle, &iosb, &fsi,
                                            sizeof(fsi), FileStandardInformation);
            if (!NT_SUCCESS(status)) {
                DbgPrint("[TgShadow] enable: 查询影子文件大小失败 0x%08X\n", status);
                TgShadowCloseStorage();
                return status;
            }
            g_ShadowFileBytes = (ULONG64)fsi.AllocationSize.QuadPart;
            if (shadowBytes == 0 || shadowBytes > g_ShadowFileBytes) {
                shadowBytes = g_ShadowFileBytes;
            }
            /* 关键：影子卷必须排除在 COW 之外，否则写影子 -> 再 COW -> 无限递归。
               三种识别方式依次尝试（实测 VPB 在文件系统设备上不一定有值）。 */
            {
                PVPB vpb = (g_ShadowDevice != NULL) ? g_ShadowDevice->Vpb : NULL;

                if (vpb != NULL && vpb->RealDevice != NULL) {
                    g_ShadowVolumeFilter = TgShadowFindFilterByPdo(vpb->RealDevice);
                }
                if (g_ShadowVolumeFilter == NULL && vpb != NULL) {
                    g_ShadowVolumeFilter = TgShadowFindFilterByVpb(vpb);
                }
                /* 兜底：路径形如 \??\S:\xxx，取盘符直接按 DOS 名匹配 */
                if (g_ShadowVolumeFilter == NULL && in->ShadowPath[4] != L'\0') {
                    WCHAR drive[3];
                    drive[0] = in->ShadowPath[4];      /* 'S' */
                    drive[1] = L':';
                    drive[2] = L'\0';
                    g_ShadowVolumeFilter = TgShadowFindFilterByDosName(drive);
                }
            }
            if (g_ShadowVolumeFilter == NULL) {
                DbgPrint("[TgShadow] enable: 无法识别影子卷的过滤设备，"
                         "拒绝启用（否则会递归）\n");
                TgShadowTrace(L"enable: shadow volume filter NOT FOUND");
                TgShadowTraceValue(L"TgShadowEnableErr", L"影子卷过滤设备未找到");
                TgShadowCloseStorage();
                return STATUS_DEVICE_NOT_READY;
            }
            TgShadowTraceValue(L"TgShadowEnableErr", L"(无：影子卷已识别)");
            if (g_ShadowVolumeFilter == filter) {
                DbgPrint("[TgShadow] enable: 影子文件与受保护卷是同一个卷，拒绝启用\n");
                TgShadowTrace(L"enable: shadow on protected volume - REFUSED");
                TgShadowCloseStorage();
                return STATUS_INVALID_PARAMETER;
            }
            DbgPrint("[TgShadow] enable: 磁盘影子 %llu MB（卷已排除递归）\n",
                     g_ShadowFileBytes / (1024 * 1024));
        } else {
            /* 没给路径 = 内存影子（仅用于短时间验证，容量受物理内存限制） */
            TgShadowTrace(L"enable: no shadow path supplied (memory backend)");
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
    case IOCTL_TGSHADOW_COMMIT:      status = TgShadowCommit(Irp, stack);     break;
    case IOCTL_TGSHADOW_DISCARD:     status = TgShadowDiscard(Irp, stack);    break;
    default:                         status = STATUS_INVALID_DEVICE_REQUEST;  break;
    }
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ------------------------------------------------------------ 系统卷设备识别 */

/** 往 HKLM\SOFTWARE\TimeGuard 写一个字符串值（取证用，enable 时调用） */
static VOID
TgShadowTraceValue(_In_ PCWSTR Name, _In_ PCWSTR Value)
{
    HANDLE            key = NULL;
    UNICODE_STRING    keyName, valueName;
    OBJECT_ATTRIBUTES oa;
    ULONG             len = 0;

    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\SOFTWARE\\TimeGuard");
    InitializeObjectAttributes(&oa, &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL,
                                REG_OPTION_NON_VOLATILE, NULL))) {
        return;
    }
    while (len < 900 && Value[len] != L'\0') {
        len++;
    }
    RtlInitUnicodeString(&valueName, Name);
#pragma warning(push)
#pragma warning(disable: 4996)
    ZwSetValueKey(key, &valueName, 0, REG_SZ, (PVOID)Value,
                  (ULONG)((len + 1) * sizeof(WCHAR)));
#pragma warning(pop)
    ZwClose(key);
}

/**
 * 把当前已挂载的每个卷的【PDO 名 / DOS 名 / 解析出的卷号】写进注册表。
 * 卷的 DOS 名（\??\C:）在 BOOT_START 阶段还不存在，只有系统起来后才有，
 * 所以这个诊断必须在 enable 时（系统早已启动完）调用。
 */
/* 内核里没有 CRT 的字符串函数，自己拼（只在诊断路径上用，不追求效率） */
static ULONG
TgAppendStr(_Out_ PWCHAR Dst, _In_ ULONG Cap, _In_ ULONG Pos, _In_ PCWSTR Src)
{
    while (*Src != L'\0' && Pos + 1 < Cap) {
        Dst[Pos++] = *Src++;
    }
    Dst[Pos] = L'\0';
    return Pos;
}

static ULONG
TgAppendNum(_Out_ PWCHAR Dst, _In_ ULONG Cap, _In_ ULONG Pos, _In_ ULONG Value)
{
    WCHAR         tmp[16];
    UNICODE_STRING numStr;
    ULONG          i;

    numStr.Buffer = tmp;
    numStr.MaximumLength = sizeof(tmp);
    numStr.Length = 0;
    if (!NT_SUCCESS(RtlIntegerToUnicodeString(Value, 10, &numStr))) {
        return TgAppendStr(Dst, Cap, Pos, L"?");
    }
    for (i = 0; i < numStr.Length / sizeof(WCHAR); i++) {
        if (Pos + 1 >= Cap) break;
        Dst[Pos++] = tmp[i];
    }
    Dst[Pos] = L'\0';
    return Pos;
}

static VOID
TgShadowDumpVolumes(VOID)
{
    WCHAR          buf[1024];
    ULONG          pos = 0, i;
    (void)pos;
    PDEVICE_OBJECT snapshot[TGSHADOW_MAX_VOLUMES];
    KIRQL          irql;

    KeAcquireSpinLock(&g_VolumeLock, &irql);
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        snapshot[i] = g_VolumeDevices[i];
    }
    KeReleaseSpinLock(&g_VolumeLock, irql);

    buf[0] = L'\0';
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        PTGSHADOW_DEV_EXT ext;
        WCHAR   pdoName[128];
        WCHAR   dosName[128];
        UNICODE_STRING dos;
        ULONG   pdoLen = 0, k;

        if (snapshot[i] == NULL) {
            continue;
        }
        ext = TGSHADOW_EXT(snapshot[i]);

        pdoName[0] = L'\0';
        if (!NT_SUCCESS(IoGetDeviceProperty(ext->Pdo, DevicePropertyPhysicalDeviceObjectName,
                                            sizeof(pdoName), pdoName, &pdoLen))) {
            RtlInitUnicodeString(&dos, L"(unknown)");
            for (k = 0; k < dos.Length / sizeof(WCHAR) && k < 100; k++) {
                pdoName[k] = dos.Buffer[k];
            }
            pdoName[k] = L'\0';
        }
        dosName[0] = L'\0';
        if (NT_SUCCESS(IoVolumeDeviceToDosName(ext->Pdo, &dos))) {
            for (k = 0; k < dos.Length / sizeof(WCHAR) && k < 100; k++) {
                dosName[k] = dos.Buffer[k];
            }
            dosName[k] = L'\0';
            ExFreePool(dos.Buffer);
        } else {
            RtlInitUnicodeString(&dos, L"(none)");
            for (k = 0; k < dos.Length / sizeof(WCHAR) && k < 100; k++) {
                dosName[k] = dos.Buffer[k];
            }
            dosName[k] = L'\0';
        }

        /* 追加一行：[idx] vol=N boot=X pdo=... dos=... */
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, L"[");
        pos = TgAppendNum(buf, ARRAYSIZE(buf), pos, i);
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, L"] vol=");
        pos = TgAppendNum(buf, ARRAYSIZE(buf), pos, ext->VolumeNumber);
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, L" boot=");
        pos = TgAppendNum(buf, ARRAYSIZE(buf), pos, ext->IsBootVolume);
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, L" pdo=");
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, pdoName);
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, L" dos=");
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, dosName);
        pos = TgAppendStr(buf, ARRAYSIZE(buf), pos, L"\r\n");
    }
    if (pos == 0) {
        pos = TgAppendStr(buf, ARRAYSIZE(buf), 0, L"(没有已挂载的卷)\r\n");
    }
    TgShadowTraceValue(L"TgShadowVolumes", buf);
    DbgPrint("[TgShadow] 卷清单：\n%ws\n", buf);
}

/**
 * 在【已挂载的过滤设备】里按 DOS 名找目标卷。
 * 必须在 PASSIVE_LEVEL 调用，且只能在系统启动完成后（DOS 名才存在）。
 */
static PDEVICE_OBJECT
TgShadowFindFilterByDosName(_In_ PCWSTR Want)
{
    PDEVICE_OBJECT snapshot[TGSHADOW_MAX_VOLUMES];
    PDEVICE_OBJECT found = NULL;
    KIRQL          irql;
    ULONG          i, k;
    USHORT         wantChars = (USHORT)wcslen(Want);

    KeAcquireSpinLock(&g_VolumeLock, &irql);
    for (i = 0; i < TGSHADOW_MAX_VOLUMES; i++) {
        snapshot[i] = g_VolumeDevices[i];
    }
    KeReleaseSpinLock(&g_VolumeLock, irql);

    for (i = 0; i < TGSHADOW_MAX_VOLUMES && found == NULL; i++) {
        UNICODE_STRING dos;
        BOOLEAN        match;

        if (snapshot[i] == NULL) {
            continue;
        }
        if (!NT_SUCCESS(IoVolumeDeviceToDosName(TGSHADOW_EXT(snapshot[i])->Pdo, &dos))) {
            continue;
        }
        match = (dos.Length == wantChars * sizeof(WCHAR));
        for (k = 0; match && k < wantChars; k++) {
            if (RtlUpcaseUnicodeChar(dos.Buffer[k]) != RtlUpcaseUnicodeChar(Want[k])) {
                match = FALSE;
            }
        }
        if (match) {
            found = snapshot[i];
        }
        ExFreePool(dos.Buffer);
    }
    return found;
}

/**
 * 判断某个卷设备是不是系统卷（C:）。
 *
 * 用 IoVolumeDeviceToDosName 直接问系统"这个卷的 DOS 名是什么"——这是文档化 API，
 * 比解析 PDO 名字（可能是 \Device\Harddisk3\Partition3）和解析符号链接都可靠。
 *
 * 走过的弯路（留作记录）：
 *   - 解析 PDO 名里的 "HarddiskVolumeN"：分区 PDO 名字形式不同，解析得 0；
 *   - IoGetBootDiskVolume：WDK 没导出，链接失败；
 *   - ZwOpenSymbolicLinkObject(L"\\SystemRoot")：系统上不一定是符号链接；
 *   - IoGetDeviceObjectPointer(L"\\??\\C:") 再和 PDO 比指针：返回的是【文件系统
 *     设备对象】（栈顶），和卷 PDO 根本不是同一个对象，永远比不中。
 * AddDevice 在 PASSIVE_LEVEL 执行，正好满足 IoVolumeDeviceToDosName 的要求。
 */
static BOOLEAN
TgShadowIsSystemVolume(_In_ PDEVICE_OBJECT Pdo)
{
    UNICODE_STRING dos;
    static const WCHAR want[] = L"\\??\\C:";
    BOOLEAN match = FALSE;
    ULONG   i;

    if (!NT_SUCCESS(IoVolumeDeviceToDosName(Pdo, &dos))) {
        return FALSE;
    }
    if (dos.Buffer != NULL) {
        DbgPrint("[TgShadow] 卷 %p 的 DOS 名 = %wZ\n", Pdo, &dos);
        if (dos.Length == (sizeof(want) - sizeof(WCHAR)) &&
            dos.Buffer[0] == L'\\') {
            match = TRUE;
            for (i = 0; i < dos.Length / sizeof(WCHAR); i++) {
                if (RtlUpcaseUnicodeChar(dos.Buffer[i]) !=
                    RtlUpcaseUnicodeChar(want[i])) {
                    match = FALSE;
                    break;
                }
            }
        }
        ExFreePool(dos.Buffer);
    }
    return match;
}

/**
 * 解析 C: 对应的卷设备对象，用于精确识别"系统卷"。
 *
 * 为什么不在 DriverEntry 一次搞定：本驱动是 BOOT_START，加载时 DOS 设备名
 * （\??\C:）通常还没被挂载管理器建立，只有等到系统起来后才可用。所以这里
 * 设计成可重入：DriverEntry 先尽力试一次，enable（必然在系统启动完成后）再试。
 * 必须在 PASSIVE_LEVEL 调用。
 */
static VOID
TgShadowResolveSystemVolume(VOID)
{
    static const WCHAR *cands[] = { L"\\??\\C:", L"\\SystemRoot", L"\\Device\\HarddiskVolume1" };
    ULONG c;

    if (g_SystemVolumeDevice != NULL) {
        return;
    }
    for (c = 0; c < sizeof(cands) / sizeof(cands[0]); c++) {
        UNICODE_STRING    name;
        OBJECT_ATTRIBUTES oa;
        NTSTATUS          st;
        PFILE_OBJECT      fileObj = NULL;
        PDEVICE_OBJECT    devObj = NULL;

        RtlInitUnicodeString(&name, cands[c]);
        InitializeObjectAttributes(&oa, &name,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        st = IoGetDeviceObjectPointer(&name, FILE_READ_DATA, &fileObj, &devObj);
        if (!NT_SUCCESS(st)) {
            DbgPrint("[TgShadow] 解析 %ws 失败 0x%08X\n", cands[c], st);
            continue;
        }
        g_SystemVolumeFile = fileObj;
        g_SystemVolumeDevice = devObj;
        DbgPrint("[TgShadow] 系统卷设备已解析：%ws -> %p\n", cands[c], devObj);
        return;
    }
    DbgPrint("[TgShadow] 警告：无法解析系统卷设备（DOS 名可能尚未建立）\n");
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
    ext->Pdo = PhysicalDeviceObject;
    /* 系统卷判定：优先用 DOS 名（最可靠），再退回设备指针比对 */
    ext->IsBootVolume = TgShadowIsSystemVolume(PhysicalDeviceObject) ? 1 : 0;
    if (!ext->IsBootVolume && g_SystemVolumeDevice != NULL &&
        g_SystemVolumeDevice == PhysicalDeviceObject) {
        ext->IsBootVolume = 1;
    }

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

    /* 登记到卷索引表：提交/丢弃时按卷号找到正确的下层设备 */
    {
        KIRQL irql;
        ULONG k;
        KeAcquireSpinLock(&g_VolumeLock, &irql);
        for (k = 0; k < TGSHADOW_MAX_VOLUMES; k++) {
            if (g_VolumeDevices[k] == NULL) {
                g_VolumeDevices[k] = filterDev;
                g_VolumeNumbers[k] = ext->VolumeNumber;
                break;
            }
        }
        KeReleaseSpinLock(&g_VolumeLock, irql);
    }

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
    }
    if (g_SystemVolumeFile != NULL) {
        /* 释放 DriverEntry 里 IoGetDeviceObjectPointer 拿到的引用 */
        ObDereferenceObject(g_SystemVolumeFile);
        g_SystemVolumeFile = NULL;
        g_SystemVolumeDevice = NULL;
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
    KeInitializeSpinLock(&g_VolumeLock);

    /* 尽力尝试解析系统卷设备；失败也没关系——enable 时（系统早已启动完）
       会再试一次，见 TgShadowResolveSystemVolume。 */
    TgShadowResolveSystemVolume();
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

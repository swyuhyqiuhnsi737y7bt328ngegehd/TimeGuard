/*==============================================================================
  tgshadow.h - TimeGuard 磁盘影子（重启还原）驱动公共定义
  ----------------------------------------------------------------------------
  用户态（tgshadow_svc.exe / admin.exe）与内核驱动共享的接口定义。

  设计：作为卷设备的 Upper Filter 挂载到受保护卷（默认系统卷）之上，
  在扇区级拦截 IRP_MJ_READ / IRP_MJ_WRITE，用写时复制（COW）把改动重定向到
  外部影子存储（另一卷上的预分配文件），重启默认丢弃 = 还原；家长确认"保留"
  时把影子数据提交回真实卷。

  注意：本文件同时被内核与用户态包含，不能使用内核专有类型。
==============================================================================*/
#ifndef TGSHADOW_H
#define TGSHADOW_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef ULONG  TG_U32;
typedef ULONG64 TG_U64;
typedef USHORT TG_U16;
typedef UCHAR  TG_U8;
#else
#include <windows.h>
typedef unsigned int   TG_U32;
typedef unsigned __int64 TG_U64;
typedef unsigned short TG_U16;
typedef unsigned char  TG_U8;
#endif

#define TGSHADOW_VERSION_MAJOR   0
#define TGSHADOW_VERSION_MINOR   1
#define TGSHADOW_VERSION_STRING  L"0.1.0-p3.1"

/* 设备与符号链接：用户态通过 \\.\\TgShadow 打开 */
#define TGSHADOW_NT_DEVICE_NAME   L"\\Device\\TgShadow"
#define TGSHADOW_WIN32_DEVICE     L"\\\\.\\TgShadow"

/* 影子块粒度：64KB（128 个 512B 扇区）——单块越小映射表越大，越大写放大越高 */
#define TGSHADOW_BLOCK_SIZE       (64 * 1024)
#define TGSHADOW_SECTOR_SIZE      512

/* 驱动状态（IOCTL_GET_STATUS 返回） */
typedef struct _TGSHADOW_STATUS {
    TG_U32  VersionMajor;
    TG_U32  VersionMinor;
    TG_U32  Protected;          /* 1 = 影子保护已启用 */
    TG_U32  FilterAttached;     /* 1 = 卷过滤已挂载 */
    TG_U32  TargetVolumeNumber; /* 受保护卷号（HarddiskVolumeN 的 N） */
    TG_U32  Reserved0;
    TG_U64  ShadowBytesTotal;   /* 影子存储总容量 */
    TG_U64  ShadowBytesUsed;    /* 已用（已重定向块占用） */
    TG_U64  ReadCount;          /* 拦截到的读 I/O 数 */
    TG_U64  WriteCount;         /* 拦截到的写 I/O 数 */
    TG_U64  RedirectedBlocks;   /* 已重定向的块数 */

    /* ---- P3 诊断：COW 分支为何没进 ---- */
    TG_U64  WritesPassive;      /* PASSIVE_LEVEL 的写 I/O 次数（COW 的前提） */
    TG_U64  WritesHighIrql;     /* 高 IRQL 的写 I/O 次数（只能透传） */
    TG_U64  CowEntered;         /* 进入 COW 判定的次数 */
    TG_U64  CowNoBuffer;        /* 因取不到缓冲区而放弃 COW 的次数 */
    TG_U64  CowWriteOk;         /* 写成功重定向到影子的次数 */
    TG_U64  CowReadOk;          /* 读成功命中影子的次数 */
    TG_U64  CowFailed;          /* COW 失败并落回透传的次数（>0 说明保护未生效） */
    TG_U32  CowLastStatus;      /* 最近一次 COW 失败的 NTSTATUS */
} TGSHADOW_STATUS, *PTGSHADOW_STATUS;

/* 卷信息（IOCTL_GET_VOLUMES 返回数组元素） */
typedef struct _TGSHADOW_VOLUME_INFO {
    TG_U32  VolumeNumber;       /* HarddiskVolumeN 的 N */
    TG_U32  IsTarget;           /* 1 = 当前受保护卷 */
    TG_U64  TotalBytes;         /* 卷容量（P1 暂填 0，P2 起用 IOCTL 查询） */
} TGSHADOW_VOLUME_INFO, *PTGSHADOW_VOLUME_INFO;

/* enable 的 Flags 位 */
#define TGSHADOW_FLAG_ATTACH_ONLY  0x0001  /* 只挂载卷过滤、不记录写（分步定位用） */

/* 启用参数（IOCTL_ENABLE） */
typedef struct _TGSHADOW_ENABLE_INPUT {
    TG_U32  VolumeNumber;       /* 要保护的卷号 */
    TG_U32  Flags;              /* 保留 */
    TG_U64  ShadowBytes;        /* 影子存储容量（0 = 使用驱动默认） */
    /* 受保护卷的容量（字节），由用户态查询后传入（IOCTL_DISK_GET_LENGTH_INFO）。
       必须由用户态提供：内核里用 IoBuildDeviceIoControlRequest 向卷设备发同步 IRP
       时 IRP 没有 FileObject，卷/磁盘驱动解引用它会空指针崩溃(0x3B/0xC0000005)。 */
    TG_U64  VolumeBytes;

    /* P3：影子存储文件路径（NT 形式，如 \??\D:\tgshadow.bin）。
       由用户态预先创建并预分配好容量，驱动在 enable 时打开它。
       影子文件所在卷绝不能是被保护的卷（否则递归重定向）。 */
    TG_U16  ShadowPath[260];
} TGSHADOW_ENABLE_INPUT, *PTGSHADOW_ENABLE_INPUT;

/* IOCTL 定义（设备类型 0x8331 = 自定义） */
#define TGSHADOW_DEVICE_TYPE  0x8331

#define IOCTL_TGSHADOW_GET_VERSION \
    CTL_CODE(TGSHADOW_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TGSHADOW_GET_STATUS \
    CTL_CODE(TGSHADOW_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TGSHADOW_GET_VOLUMES \
    CTL_CODE(TGSHADOW_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TGSHADOW_ENABLE \
    CTL_CODE(TGSHADOW_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_TGSHADOW_DISABLE \
    CTL_CODE(TGSHADOW_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_TGSHADOW_COMMIT \
    CTL_CODE(TGSHADOW_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_TGSHADOW_DISCARD \
    CTL_CODE(TGSHADOW_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#endif /* TGSHADOW_H */

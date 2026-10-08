/*
 * device.h - partunit.device internal structures.
 *
 * The device presents each Amiga-typed partition of a configured underlying
 * block device as a unit of itself. See docs/proposal.md.
 */

#ifndef PARTUNIT_DEVICE_H
#define PARTUNIT_DEVICE_H

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <devices/trackdisk.h>
#include <dos/dos.h>

#include "ptparse.h"
#include "unitmap.h"

#define DEVICE_NAME     "partunit.device"
#define DEVICE_VERSION  0
#define DEVICE_REVISION 1
/*
 * Disk-loaded only, so the RomTag priority is irrelevant to boot order; it
 * exists because a resident structure needs one.
 */
#define DEVICE_PRIORITY 0

#define IDSTRING        DEVICE_NAME " 0.1 (8.10.2026)"

/* Bounded by UM_MAX_DISKS / UM_MAX_PARTS_PER_DISK in unitmap.h. */
#define MAX_DISKS       UM_MAX_DISKS
#define MAX_PARTS       UM_MAX_PARTS_PER_DISK

struct PUDisk;

/*
 * One unit = one partition. Begins with a MinNode so the node pointer IS the
 * io_Unit pointer, as lide does.
 */
struct PUUnit {
    struct MinNode   pu_Node;
    struct PUDisk   *pu_Disk;
    um_unit          pu_Map;        /* start, length, block size, flags     */
    pt_partition     pu_Part;       /* what the parser found                */
    ULONG            pu_UnitNum;    /* disk * 100 + partition               */
    ULONG            pu_ChangeNum;  /* OUR change count; starts at 1        */
    ULONG            pu_OpenCount;
    struct MinList   pu_ChangeInts; /* TD_ADDCHANGEINT queue                */
    APTR             pu_ChangeInt;  /* the single legacy TD_REMOVE slot     */
    UBYTE            pu_Present;    /* slot is populated                    */
    UBYTE            pu_Pad;
};

/*
 * One underlying device/unit pair from the config, with the task that owns
 * access to it. Units on the same disk share the task, which serialises our
 * use of the child - which is what the child expects anyway.
 */
struct PUDisk {
    struct MinNode      pd_Node;
    struct DeviceBase  *pd_Dev;
    struct Task        *pd_Task;
    struct Task        *pd_Parent;      /* for the startup handshake        */
    struct MsgPort     *pd_Port;        /* where BeginIO queues requests    */

    /* Our own request to the underlying device. */
    struct MsgPort     *pd_ChildPort;
    struct IOExtTD     *pd_ChildIO;
    struct IOStdReq    *pd_ChildStd;    /* same memory, STDIO view          */

    char                pd_Name[32];    /* e.g. "scsi.device"               */
    ULONG               pd_ChildUnit;
    ULONG               pd_DiskIndex;   /* config order                     */

    ULONG               pd_BlockSize;
    pt_u64               pd_TotalBlocks;
    ULONG               pd_ChildChangeNum;

    UBYTE               pd_ChildOpen;
    UBYTE               pd_Has64;       /* child advertised TD64/NSD64      */
    UBYTE               pd_HasNSD;      /* child answered NSCMD_DEVICEQUERY */
    UBYTE               pd_Removable;
    UBYTE               pd_Active;      /* task is alive                    */
    UBYTE               pd_Force;       /* parse even if an RDB is sniffed  */
    UBYTE               pd_BackedOff;   /* RDB found: the OS owns this disk */
    UBYTE               pd_Pad;

    /* Scratch for the parser. Allocated, not stacked: pt_scratch is ~4KB
     * and a device init path has no business putting that on a stack. */
    pt_scratch         *pd_Scratch;
};

struct DeviceBase {
    struct Library      db_Lib;
    struct ExecBase    *db_SysBase;
    struct Library      *db_DOSBase;
    BPTR                db_SegList;
    struct MinList      db_Units;
    struct MinList      db_Disks;
    struct SignalSemaphore db_UnitSem;
    ULONG               db_NumUnits;
    ULONG               db_NumDisks;
    UBYTE               db_IsOpen;
    UBYTE               db_NoMount;
    UBYTE               db_Pad[2];
};

/* Our private commands, deliberately not advertised. */
#define PU_CMD_DIE      0x1000

/* iotask.c */
void  pu_io_task(void);
LONG  pu_disk_start(struct DeviceBase *dev, struct PUDisk *pd);
void  pu_disk_stop(struct PUDisk *pd);

/* unitio.c - the request handler, running on the disk's task */
void  pu_process_ioreq(struct PUDisk *pd, struct IOStdReq *ior);

/* config.c - the DISK lines of ENV:partunit/config */
void  pu_config_load(struct DeviceBase *dev);

/* child.c - talking to the underlying device */
LONG  pu_child_open(struct PUDisk *pd);
void  pu_child_close(struct PUDisk *pd);
LONG  pu_child_probe(struct PUDisk *pd);
LONG  pu_child_rw(struct PUDisk *pd, int is_write, pt_u64 byte_off,
                  ULONG length, APTR data, ULONG *actual, int needs64);
int   pu_child_read_block(void *user, pt_u64 lba, void *buf);

#endif /* PARTUNIT_DEVICE_H */

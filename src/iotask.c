/*
 * iotask.c - one IO task per underlying disk.
 *
 * Per disk, not per unit: all units on a disk share one child IORequest, and
 * one task owning it serialises our use of the child, which is what the child
 * expects anyway. lide uses the same shape, one task per ATA channel.
 */

#include "device.h"

#include <exec/errors.h>
#include <exec/memory.h>

#include <proto/exec.h>

#include <string.h>

/*
 * SysBase is the global that proto/exec.h declares and device.c defines and
 * sets during init. A device has no startup code to initialise it, so if
 * device.c ever stops setting it every Exec call in the driver dereferences
 * NULL - which is why it is set as the very first thing init_device does.
 */

/* Not const: tc_Node.ln_Name is a plain char *, and casting const away
 * for something Exec may read at any time is a lie worth not telling. */
static char task_name[] = DEVICE_NAME;

/* Discover the partitions on this disk and populate its unit slots. */
static void scan_disk(struct PUDisk *pd)
{
    struct DeviceBase *dev = pd->pd_Dev;
    pt_device    ptdev;
    pt_table     tbl;
    pt_partition parts[MAX_PARTS];
    pt_result    r;
    int          found = 0;
    pt_u32       at = 0;
    unsigned int i;

    ptdev.read         = pu_child_read_block;
    ptdev.user         = pd;
    ptdev.block_size   = pd->pd_BlockSize;
    ptdev.total_blocks = pd->pd_TotalBlocks;

    /*
     * Whole-drive back-off. A disk that is entirely Amiga can carry an RDB
     * behind an MBR placed at block 0 only so Windows does not offer to
     * format it; that disk already belongs to the OS and we must not present
     * units for it. FORCE overrides.
     */
    if (!pd->pd_Force) {
        if (pt_sniff_rdb(&ptdev, pd->pd_Scratch, &found, &at) == PT_OK &&
            found) {
            pd->pd_BackedOff = 1;
            return;
        }
    }

    r = pt_parse(&ptdev, pd->pd_Scratch, parts, MAX_PARTS, &tbl);
    if (r != PT_OK) {
        return;
    }

    for (i = 0; i < tbl.count; i++) {
        struct PUUnit *pu;

        if (!parts[i].is_amiga) {
            continue;       /* a FAT or Linux partition is never a unit */
        }
        pu = AllocMem(sizeof(struct PUUnit), MEMF_PUBLIC | MEMF_CLEAR);
        if (pu == NULL) {
            return;
        }

        pu->pu_Disk    = pd;
        pu->pu_Part    = parts[i];
        pu->pu_UnitNum = um_unit_number(pd->pd_DiskIndex, i);

        pu->pu_Map.start_lba     = parts[i].start_lba;
        pu->pu_Map.block_count   = parts[i].block_count;
        pu->pu_Map.block_size    = pd->pd_BlockSize;
        pu->pu_Map.writable      = 1;
        pu->pu_Map.media_present = 1;
        if (um_unit_init(&pu->pu_Map) != UM_OK) {
            FreeMem(pu, sizeof(struct PUUnit));
            continue;
        }

        /*
         * Starts at 1, not 0. The spec does not mandate a starting value -
         * it says only that the counter is incremented on insert or remove -
         * but devtest requires iotd_Count == 0 to be rejected, which is only
         * true if the counter starts non-zero. lide starts at 1. This is a
         * devtest-driven convention rather than a spec requirement.
         */
        pu->pu_ChangeNum = 1;
        NewMinList(&pu->pu_ChangeInts);
        pu->pu_Present = 1;

        ObtainSemaphore(&dev->db_UnitSem);
        AddTail((struct List *)&dev->db_Units, (struct Node *)&pu->pu_Node);
        dev->db_NumUnits++;
        ReleaseSemaphore(&dev->db_UnitSem);
    }
}

static void unscan_disk(struct PUDisk *pd)
{
    struct DeviceBase *dev = pd->pd_Dev;
    struct MinNode    *n, *next;

    ObtainSemaphore(&dev->db_UnitSem);
    n = dev->db_Units.mlh_Head;
    while (n->mln_Succ != NULL) {
        struct PUUnit *pu = (struct PUUnit *)n;
        next = n->mln_Succ;
        if (pu->pu_Disk == pd) {
            Remove((struct Node *)n);
            dev->db_NumUnits--;
            FreeMem(pu, sizeof(struct PUUnit));
        }
        n = next;
    }
    ReleaseSemaphore(&dev->db_UnitSem);
}

void pu_io_task(void)
{
    struct Task   *me = FindTask(NULL);
    struct PUDisk *pd = (struct PUDisk *)me->tc_UserData;
    ULONG          mask;
    int            running = 1;

    pd->pd_Port = CreateMsgPort();
    if (pd->pd_Port == NULL) {
        goto fail;
    }
    if (pu_child_open(pd) != 0) {
        goto fail;
    }
    if (pu_child_probe(pd) != 0) {
        goto fail;
    }

    scan_disk(pd);

    pd->pd_Active = 1;
    Signal(pd->pd_Parent, SIGF_SINGLE);

    mask = 1UL << pd->pd_Port->mp_SigBit;

    while (running) {
        struct IOStdReq *ior;

        Wait(mask);
        while ((ior = (struct IOStdReq *)GetMsg(pd->pd_Port)) != NULL) {
            if (ior->io_Command == PU_CMD_DIE) {
                running = 0;
                ior->io_Error = 0;
                ReplyMsg(&ior->io_Message);
                continue;
            }
            pu_process_ioreq(pd, ior);
        }
    }

    /* Teardown, then tell the caller of pu_disk_stop that we are gone. */
    unscan_disk(pd);
    pu_child_close(pd);
    if (pd->pd_Port != NULL) {
        DeleteMsgPort(pd->pd_Port);
        pd->pd_Port = NULL;
    }
    pd->pd_Active = 0;
    pd->pd_Task   = NULL;
    Signal(pd->pd_Parent, SIGF_SINGLE);
    RemTask(NULL);
    Wait(0);
    return;

fail:
    pu_child_close(pd);
    if (pd->pd_Port != NULL) {
        DeleteMsgPort(pd->pd_Port);
        pd->pd_Port = NULL;
    }
    pd->pd_Active = 0;
    pd->pd_Task   = NULL;
    /* The handshake must happen on the failure path too, or init blocks. */
    Signal(pd->pd_Parent, SIGF_SINGLE);
    RemTask(NULL);
    Wait(0);
}

/*
 * Start a disk's task and wait for it to report. The handshake exists so
 * init_device knows whether the child opened and how many units appeared
 * before it returns - lide does the same with Wait(SIGF_SINGLE).
 */
LONG pu_disk_start(struct DeviceBase *dev, struct PUDisk *pd)
{
    struct Task     *task;
    APTR             stack;
    const ULONG      stacksize = 8192;

    pd->pd_Dev    = dev;
    pd->pd_Parent = FindTask(NULL);

    pd->pd_Scratch = AllocMem(sizeof(pt_scratch), MEMF_PUBLIC | MEMF_CLEAR);
    if (pd->pd_Scratch == NULL) {
        return TDERR_NoMem;
    }

    task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    if (task == NULL) {
        FreeMem(pd->pd_Scratch, sizeof(pt_scratch));
        pd->pd_Scratch = NULL;
        return TDERR_NoMem;
    }
    stack = AllocMem(stacksize, MEMF_PUBLIC);
    if (stack == NULL) {
        FreeMem(task, sizeof(struct Task));
        FreeMem(pd->pd_Scratch, sizeof(pt_scratch));
        pd->pd_Scratch = NULL;
        return TDERR_NoMem;
    }

    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri  = 5;
    task->tc_Node.ln_Name = task_name;
    task->tc_SPLower      = stack;
    task->tc_SPUpper      = (APTR)((ULONG)stack + stacksize);
    task->tc_SPReg        = task->tc_SPUpper;
    /*
     * Set before AddTask so the task can find its PUDisk the moment it runs;
     * this is why lide has its own L_CreateTask rather than using
     * CreateTask() from amiga.lib.
     */
    task->tc_UserData     = pd;

    SetSignal(0, SIGF_SINGLE);
    pd->pd_Task = task;

    if (AddTask(task, (APTR)pu_io_task, NULL) == NULL) {
        FreeMem(stack, stacksize);
        FreeMem(task, sizeof(struct Task));
        FreeMem(pd->pd_Scratch, sizeof(pt_scratch));
        pd->pd_Scratch = NULL;
        pd->pd_Task = NULL;
        return TDERR_NoMem;
    }

    Wait(SIGF_SINGLE);

    if (!pd->pd_Active) {
        /* The task freed nothing but itself; its stack and Task are ours. */
        FreeMem(stack, stacksize);
        FreeMem(task, sizeof(struct Task));
        FreeMem(pd->pd_Scratch, sizeof(pt_scratch));
        pd->pd_Scratch = NULL;
        return IOERR_OPENFAIL;
    }
    return 0;
}

void pu_disk_stop(struct PUDisk *pd)
{
    struct MsgPort  *port;
    struct IOStdReq *ior;

    if (!pd->pd_Active || pd->pd_Port == NULL) {
        return;
    }

    port = CreateMsgPort();
    if (port == NULL) {
        return;
    }
    ior = CreateIORequest(port, sizeof(struct IOStdReq));
    if (ior == NULL) {
        DeleteMsgPort(port);
        return;
    }

    pd->pd_Parent = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);

    ior->io_Command = PU_CMD_DIE;
    ior->io_Message.mn_ReplyPort = port;
    PutMsg(pd->pd_Port, &ior->io_Message);
    WaitPort(port);
    GetMsg(port);

    Wait(SIGF_SINGLE);      /* the task signals again once torn down */

    DeleteIORequest((struct IORequest *)ior);
    DeleteMsgPort(port);

    if (pd->pd_Scratch != NULL) {
        FreeMem(pd->pd_Scratch, sizeof(pt_scratch));
        pd->pd_Scratch = NULL;
    }
}

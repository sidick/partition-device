/*
 * device.c - partunit.device: RomTag, library vectors, and BeginIO.
 *
 * Structure follows lide.device, which follows jbilander's SimpleDevice
 * skeleton: a Resident structure whose rt_Init builds the library, a
 * register-annotated open/close/expunge/beginio/abortio set, and a unit list
 * rather than a unit array.
 */

#include "device.h"

#include <devices/newstyle.h>
#include <devices/scsidisk.h>
#include <exec/errors.h>
#include <exec/initializers.h>
#include <exec/memory.h>
#include <exec/resident.h>

#include <proto/exec.h>

#include <string.h>

#ifndef NSCMD_ETD_READ64
# define NSCMD_ETD_READ64   0xe000
# define NSCMD_ETD_WRITE64  0xe001
# define NSCMD_ETD_SEEK64   0xe002
# define NSCMD_ETD_FORMAT64 0xe003
#endif

/*
 * A device has no startup code, so nothing initialises the SysBase that
 * proto/exec.h declares. We define it here and set it as the very first
 * thing init_device does; every Exec call in the whole driver goes through
 * it.
 */
struct ExecBase *SysBase;

/* Not const: Exec stores these in lib_Node.ln_Name and lib_IdString, which
 * are plain char *, and casting const away for something the system may read
 * at any time is a lie worth not telling. */
static char device_name[] = DEVICE_NAME;
static char device_id[]   = IDSTRING;

/* Forward declarations, all register-annotated as Exec requires. */
static struct Library *init_device(struct ExecBase *sysbase asm("a6"),
                                   BPTR seglist asm("a0"),
                                   struct DeviceBase *dev asm("d0"));
static ULONG dev_open(struct DeviceBase *dev asm("a6"),
                      struct IOStdReq *ior asm("a1"),
                      ULONG unitnum asm("d0"), ULONG flags asm("d1"));
static BPTR  dev_close(struct DeviceBase *dev asm("a6"),
                       struct IOStdReq *ior asm("a1"));
static BPTR  dev_expunge(struct DeviceBase *dev asm("a6"));
static ULONG dev_null(void);
static void  dev_beginio(struct DeviceBase *dev asm("a6"),
                         struct IOStdReq *ior asm("a1"));
static ULONG dev_abortio(struct DeviceBase *dev asm("a6"),
                         struct IOStdReq *ior asm("a1"));

/*
 * "A library or device with a romtag should start with moveq #-1,d0 (to
 * safely return an error if a user tries to execute the file), followed by a
 * Resident structure."
 */
int _start(void);
int __attribute__((no_reorder)) _start(void)
{
    return -1;
}

extern const char endskip;

static const APTR dev_vectors[] = {
    (APTR)dev_open,
    (APTR)dev_close,
    (APTR)dev_expunge,
    (APTR)dev_null,          /* extFunc, not used */
    (APTR)dev_beginio,
    (APTR)dev_abortio,
    (APTR)-1
};

/* {LibBase size, function vectors, structure-init table, init function} */
static const ULONG dev_inittab[] = {
    (ULONG)sizeof(struct DeviceBase),
    (ULONG)dev_vectors,
    (ULONG)0,
    (ULONG)init_device
};

static const struct Resident romtag __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&romtag,
    (APTR)&endskip,
    RTF_AUTOINIT,
    DEVICE_VERSION,
    NT_DEVICE,
    DEVICE_PRIORITY,
    device_name,
    device_id,
    /*
     * With RTF_AUTOINIT, rt_Init points at the init TABLE, not at a function:
     * exec reads {size, vectors, structure-init, init-function} from it and
     * does the MakeLibrary/AddDevice itself. Pointing it at a function here
     * compiles fine and then fails at load time, which is a miserable thing
     * to debug.
     */
    (APTR)&dev_inittab
};

/*
 * Commands we advertise through NSCMD_DEVICEQUERY.
 *
 * TODO (Phase 2): build this per-unit from what each child actually
 * advertised. One global list is what lide does, and it is why lide's ATAPI
 * units claim TD64 support they do not have. We front heterogeneous children
 * by design, so the shortcut is worse for us than it is for lide.
 */
static const UWORD supported_commands[] = {
    CMD_CLEAR,
    CMD_UPDATE,
    CMD_READ,
    CMD_WRITE,
    TD_ADDCHANGEINT,
    TD_REMCHANGEINT,
    TD_REMOVE,
    TD_CHANGENUM,
    TD_CHANGESTATE,
    TD_GETDRIVETYPE,
    TD_GETGEOMETRY,
    TD_MOTOR,
    TD_PROTSTATUS,
    TD_FORMAT,
    TD_READ64,
    TD_WRITE64,
    TD_FORMAT64,
    ETD_READ,
    ETD_WRITE,
    ETD_FORMAT,
    ETD_UPDATE,
    ETD_CLEAR,
    NSCMD_DEVICEQUERY,
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_TD_FORMAT64,
    NSCMD_ETD_READ64,
    NSCMD_ETD_WRITE64,
    NSCMD_ETD_FORMAT64,
    HD_SCSICMD,
    0
};

/* ------------------------------------------------------------------ *
 * Unit lookup
 * ------------------------------------------------------------------ */

static struct PUUnit *find_unit(struct DeviceBase *dev, ULONG unitnum)
{
    struct MinNode *n;
    struct PUUnit  *found = NULL;

    /*
     * ObtainSemaphoreShared does not exist before V36, so branch on the
     * version rather than assuming - the same idiom lide uses.
     */
    if (SysBase->LibNode.lib_Version >= 36) {
        ObtainSemaphoreShared(&dev->db_UnitSem);
    } else {
        ObtainSemaphore(&dev->db_UnitSem);
    }
    for (n = dev->db_Units.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ) {
        struct PUUnit *pu = (struct PUUnit *)n;
        if (pu->pu_UnitNum == unitnum && pu->pu_Present) {
            found = pu;
            break;
        }
    }
    ReleaseSemaphore(&dev->db_UnitSem);
    return found;
}

/* Is this request one of ours, and still valid? Checked on every entry
 * point, as lide does - both unit membership and io_Device must match. */
static int ioreq_is_valid(struct DeviceBase *dev, struct IOStdReq *ior)
{
    struct MinNode *n;
    int             ok = 0;

    if (ior == NULL || ior->io_Unit == NULL) {
        return 0;
    }
    if (SysBase->LibNode.lib_Version >= 36) {
        ObtainSemaphoreShared(&dev->db_UnitSem);
    } else {
        ObtainSemaphore(&dev->db_UnitSem);
    }
    for (n = dev->db_Units.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ) {
        if ((struct PUUnit *)n == (struct PUUnit *)ior->io_Unit &&
            (struct Device *)dev == ior->io_Device) {
            ok = 1;
            break;
        }
    }
    ReleaseSemaphore(&dev->db_UnitSem);
    return ok;
}

/* ------------------------------------------------------------------ *
 * init
 * ------------------------------------------------------------------ */

static struct Library *init_device(struct ExecBase *sysbase asm("a6"),
                                   BPTR seglist asm("a0"),
                                   struct DeviceBase *dev asm("d0"))
{
    /* First, before any Exec call anywhere in the driver. */
    SysBase = sysbase;

    dev->db_SysBase = sysbase;
    dev->db_SegList = seglist;

    dev->db_Lib.lib_Node.ln_Type = NT_DEVICE;
    dev->db_Lib.lib_Node.ln_Name = device_name;
    dev->db_Lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    dev->db_Lib.lib_Version      = DEVICE_VERSION;
    dev->db_Lib.lib_Revision     = DEVICE_REVISION;
    dev->db_Lib.lib_IdString     = (APTR)device_id;

    NewMinList(&dev->db_Units);
    NewMinList(&dev->db_Disks);
    InitSemaphore(&dev->db_UnitSem);
    dev->db_NumUnits = 0;
    dev->db_NumDisks = 0;

    /*
     * Config is deliberately NOT read here.
     *
     * init_device runs in a forbidden state (Exec guarantees it is
     * single-threaded, but we are inside Forbid()), and reading the config
     * means dos.library Open(), which can Wait(). Waiting inside a Forbid is
     * a bug, so the config read and the disk-task startup happen at first
     * open instead - see ensure_configured().
     */
    return (struct Library *)dev;
}

/*
 * Read the config and start the disk tasks, once, on the first open.
 *
 * Gated on the opener being a real Process, because reading config means
 * touching dos.library and a bare Task has no Process structure for DOS to
 * work against. A Task-only opener therefore sees no units at all, which is
 * honest: we have no way to learn which disks to wrap.
 *
 * Open is guaranteed single-threaded by Exec, so no lock is needed around
 * db_Configured, and Wait()ing here (which pu_disk_start does, for the task
 * handshake) is legal where it would not have been at init.
 */
static void ensure_configured(struct DeviceBase *dev)
{
    if (dev->db_Configured) {
        return;
    }
    dev->db_Configured = 1;

    if (SysBase->ThisTask->tc_Node.ln_Type != NT_PROCESS) {
        return;
    }
    pu_config_load(dev);
}

/* ------------------------------------------------------------------ *
 * open / close / expunge
 * ------------------------------------------------------------------ */

static ULONG dev_open(struct DeviceBase *dev asm("a6"),
                      struct IOStdReq *ior asm("a1"),
                      ULONG unitnum asm("d0"), ULONG flags asm("d1"))
{
    struct PUUnit *pu;


    BYTE           error = 0;

    (void)flags;

    /* Guard against being expunged during open. */
    dev->db_Lib.lib_OpenCnt++;

    ensure_configured(dev);

    pu = find_unit(dev, unitnum);
    if (pu == NULL) {
        /*
         * TDERR_BadUnitNum, NEVER IOERR_OPENFAIL.
         *
         * lide's comment is emphatic: "HDToolbox scans each LUN of a unit and
         * stops searching if it sees an error other than TDERR_BadUnitNum. So
         * if this is not returned, only one drive will ever be detected."
         *
         * Our numbering is sparse - disk 1 starts at unit 100 - so a scanner
         * walking upwards must see TDERR_BadUnitNum for every gap or it stops
         * at the first one. lide itself is inconsistent here, returning
         * IOERR_OPENFAIL for unitnum > highestUnit; we deliberately do not
         * copy that.
         */
        error = TDERR_BadUnitNum;
    } else if (pu->pu_Disk == NULL || !pu->pu_Disk->pd_Active) {
        error = IOERR_OPENFAIL;
    }

    if (error == 0) {
        ior->io_Unit = (struct Unit *)pu;
        /*
         * Mark the request complete so CheckIO does not report a
         * never-used request as busy and WaitIO on one cannot hang.
         * Olaf Barthel's trackfile.device rule; BeginIO flips it back to
         * NT_MESSAGE. Both halves are required.
         */
        ior->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        pu->pu_OpenCount++;
        dev->db_Lib.lib_OpenCnt++;
        dev->db_Lib.lib_Flags &= ~LIBF_DELEXP;
        dev->db_IsOpen = 1;
    } else {
        /* Invalidate both on failure. */
        ior->io_Unit   = NULL;
        ior->io_Device = NULL;
    }

    dev->db_Lib.lib_OpenCnt--;
    ior->io_Error = error;
    return (ULONG)error;
}

static BPTR dev_close(struct DeviceBase *dev asm("a6"),
                      struct IOStdReq *ior asm("a1"))
{
    if (ioreq_is_valid(dev, ior)) {
        struct PUUnit *pu = (struct PUUnit *)ior->io_Unit;
        if (pu->pu_OpenCount > 0) {
            pu->pu_OpenCount--;
        }
        if (dev->db_Lib.lib_OpenCnt > 0) {
            dev->db_Lib.lib_OpenCnt--;
        }
    }

    /* Unconditionally, valid or not. */
    ior->io_Unit   = NULL;
    ior->io_Device = NULL;

    if (dev->db_Lib.lib_OpenCnt == 0 &&
        (dev->db_Lib.lib_Flags & LIBF_DELEXP)) {
        return dev_expunge(dev);
    }
    return 0;
}

/*
 * Never actually expunge.
 *
 * "IMPORTANT: because Expunge is called from the memory allocator, it may
 * NEVER Wait() or otherwise take long time to complete." Tearing down the
 * disk tasks requires waiting on them, so it cannot be done here - and as
 * lide puts it, "If expunged the driver would be gone until reboot".
 */
static BPTR dev_expunge(struct DeviceBase *dev asm("a6"))
{
    dev->db_Lib.lib_Flags |= LIBF_DELEXP;
    return 0;
}

static ULONG dev_null(void)
{
    return 0;
}

/* ------------------------------------------------------------------ *
 * BeginIO
 * ------------------------------------------------------------------ */

static BYTE do_getgeometry(struct PUUnit *pu, struct IOStdReq *ior)
{
    struct DriveGeometry *dg;
    um_geometry           g;

    if (ior->io_Data == NULL || ((ULONG)ior->io_Data & 1)) {
        return IOERR_BADADDRESS;
    }
    if (ior->io_Length < sizeof(struct DriveGeometry)) {
        return IOERR_BADLENGTH;
    }

    dg = (struct DriveGeometry *)ior->io_Data;
    /* Zero first so reserved fields are clean, as lide does. */
    memset(dg, 0, sizeof(struct DriveGeometry));

    um_geometry_fill(&pu->pu_Map, pu->pu_Disk->pd_Removable, &g);

    dg->dg_SectorSize   = g.sector_size;
    dg->dg_TotalSectors = g.total_sectors;
    dg->dg_Cylinders    = g.cylinders;
    dg->dg_CylSectors   = g.cyl_sectors;
    dg->dg_Heads        = g.heads;
    dg->dg_TrackSectors = g.track_sectors;
    dg->dg_BufMemType   = MEMF_PUBLIC;
    dg->dg_DeviceType   = DG_DIRECT_ACCESS;
    dg->dg_Flags        = (UBYTE)(g.removable ? DGF_REMOVABLE : 0);

    ior->io_Actual = sizeof(struct DriveGeometry);
    return 0;
}

static BYTE do_devicequery(struct IOStdReq *ior)
{
    struct NSDeviceQueryResult *r;

    if (ior->io_Data == NULL || ((ULONG)ior->io_Data & 1)) {
        return IOERR_BADADDRESS;
    }
    if (ior->io_Length < (LONG)sizeof(struct NSDeviceQueryResult)) {
        return IOERR_BADLENGTH;
    }

    r = (struct NSDeviceQueryResult *)ior->io_Data;
    r->nsdqr_DevQueryFormat   = 0;
    /*
     * The bytes we actually wrote, not io_Length and not a hardcoded size.
     * devtest never reads this field, so getting it wrong is invisible to
     * the test but misleading to real callers - and devtest's own struct is
     * larger than ours, so a caller passing a bigger buffer is normal.
     */
    r->nsdqr_SizeAvailable    = sizeof(struct NSDeviceQueryResult);
    r->nsdqr_DeviceType       = NSDEVTYPE_TRACKDISK;
    r->nsdqr_DeviceSubType    = 0;
    r->nsdqr_SupportedCommands = (APTR)supported_commands;

    ior->io_Actual = sizeof(struct NSDeviceQueryResult);
    return 0;
}

static void dev_beginio(struct DeviceBase *dev asm("a6"),
                        struct IOStdReq *ior asm("a1"))
{
    struct PUUnit *pu;
    BYTE           error = TDERR_NotSpecified;
    int            queue = 0;

    if (!ioreq_is_valid(dev, ior)) {
        ior->io_Error = error;
        if (!(ior->io_Flags & IOF_QUICK)) {
            ReplyMsg(&ior->io_Message);
        }
        return;
    }

    /*
     * The other half of the trackfile.device rule: mark the request pending
     * so WaitIO on a genuinely outstanding request works.
     */
    ior->io_Message.mn_Node.ln_Type = NT_MESSAGE;

    pu = (struct PUUnit *)ior->io_Unit;

    if (pu->pu_Disk == NULL || !pu->pu_Disk->pd_Active) {
        /* Nothing can be served if the owning task is gone. */
        ior->io_Error = IOERR_OPENFAIL;
        if (!(ior->io_Flags & IOF_QUICK)) {
            ReplyMsg(&ior->io_Message);
        }
        return;
    }

    switch (ior->io_Command) {

    /* ---- immediate: no child I/O needed ---- */
    case TD_GETGEOMETRY:
        error = do_getgeometry(pu, ior);
        break;

    case NSCMD_DEVICEQUERY:
        error = do_devicequery(ior);
        break;

    case TD_CHANGENUM:
        ior->io_Actual = pu->pu_ChangeNum;
        error = 0;
        break;

    case TD_CHANGESTATE:
        /* Media state lives in io_Actual; io_Error is always 0. */
        ior->io_Actual = pu->pu_Map.media_present ? 0 : 1;
        error = 0;
        break;

    case TD_PROTSTATUS:
        /* Write protection is a status, not an error. */
        ior->io_Actual = pu->pu_Map.writable ? 0 : 1;
        error = 0;
        break;

    case TD_GETDRIVETYPE:
        ior->io_Actual = DG_DIRECT_ACCESS;
        error = 0;
        break;

    case TD_MOTOR:
    case CMD_UPDATE:
    case CMD_CLEAR:
        /*
         * Honest no-ops: nothing is cached here and the motor is the child's
         * business. They must still answer io_Actual = 0 with no error. If a
         * buffering layer is ever added, UPDATE and CLEAR become real.
         */
        ior->io_Actual = 0;
        error = 0;
        break;

    case TD_REMOVE:
        /* The legacy single-slot interrupt. A second TD_REMOVE overwrites
         * the first, which is what trackdisk does. */
        pu->pu_ChangeInt = ior->io_Data;
        error = 0;
        break;

    case TD_ADDCHANGEINT:
        /*
         * IOF_QUICK so the common tail never replies: a change-interrupt
         * request must stay outstanding until it is removed. Disable()
         * rather than Forbid() because the list is walked from Cause()
         * context.
         */
        ior->io_Flags |= IOF_QUICK;
        Disable();
        AddHead((struct List *)&pu->pu_ChangeInts,
                (struct Node *)&ior->io_Message.mn_Node);
        Enable();
        return;

    case TD_REMCHANGEINT: {
        struct MinNode *n;
        Disable();
        for (n = pu->pu_ChangeInts.mlh_Head; n->mln_Succ != NULL;
             n = n->mln_Succ) {
            if ((struct IOStdReq *)n == ior) {
                Remove((struct Node *)n);
                break;
            }
        }
        Enable();
        /* Succeeds whether or not it was found, and does not reply it. */
        error = 0;
        break;
    }

    /* ---- queued: needs the child, so it needs the task ---- */
    case CMD_READ:
    case ETD_READ:
    case CMD_WRITE:
    case ETD_WRITE:
        /*
         * Zero the high offset for the 32-bit commands. The task reads
         * io_Actual as the high half of a 64-bit byte offset unconditionally,
         * and callers DO leave it dirty - lide's own mounter sets
         * io_Command/io_Offset/io_Data/io_Length for CMD_READ and never
         * touches io_Actual. Omitting this produces wild mis-addressing.
         */
        ior->io_Actual = 0;
        queue = 1;
        break;

    case TD_FORMAT:
    case ETD_FORMAT:
    case ETD_UPDATE:
    case ETD_CLEAR:
    case TD_READ64:
    case TD_WRITE64:
    case TD_FORMAT64:
    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_FORMAT64:
    case HD_SCSICMD:
        queue = 1;
        break;

    default:
        ior->io_Actual = 0;   /* may still hold a caller's high offset */
        error = IOERR_NOCMD;
        break;
    }

    if (queue) {
        ior->io_Flags &= ~IOF_QUICK;
        PutMsg(pu->pu_Disk->pd_Port, &ior->io_Message);
        return;
    }

    ior->io_Error = error;
    if (!(ior->io_Flags & IOF_QUICK)) {
        ReplyMsg(&ior->io_Message);
    }
}

/*
 * AbortIO: remove a request that is still queued. In-flight requests cannot
 * be aborted. Returns 0 when nothing was aborted - "0 indicates that the IO
 * was *NOT* aborted".
 */
static ULONG dev_abortio(struct DeviceBase *dev asm("a6"),
                         struct IOStdReq *ior asm("a1"))
{
    struct PUUnit *pu;
    struct Node   *n;
    ULONG          result = 0;

    if (!ioreq_is_valid(dev, ior)) {
        return 0;
    }
    pu = (struct PUUnit *)ior->io_Unit;
    if (pu->pu_Disk == NULL || pu->pu_Disk->pd_Port == NULL) {
        return 0;
    }

    /* MUST be done inside a Disable(). */
    Disable();
    for (n = pu->pu_Disk->pd_Port->mp_MsgList.lh_Head;
         n->ln_Succ != NULL; n = n->ln_Succ) {
        if ((struct IOStdReq *)n == ior) {
            Remove(n);
            ior->io_Error = IOERR_ABORTED;
            ReplyMsg(&ior->io_Message);
            result = IOERR_ABORTED;
            break;
        }
    }
    Enable();
    return result;
}

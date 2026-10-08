/*
 * config.c - the DISK lines of ENV:partunit/config.
 *
 * Phase 1 scope deliberately: only "DISK <device> <unit> [FORCE]". The PART,
 * POLL, MAXTRANSFER and NOMOUNT directives in docs/proposal.md belong to
 * Phase 3 along with the mounter, and the parser here is shaped so they can
 * be added without restructuring.
 *
 * ENV: is read live; ENVARC: is never read directly, per the house
 * ENV/ENVARC rule.
 *
 * Note on timing: this runs at FIRST OPEN, not at init, and only when the
 * opener is a real Process - see ensure_configured() in device.c. init_device
 * runs in a forbidden state, and Open() can Wait(), so reading config there
 * would be waiting inside a Forbid. It is also the reason a ROM-resident form
 * is out of scope: a ROM module has no DOS to read config with and would need
 * a global scan instead.
 */

#include "device.h"

#include <exec/errors.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

/*
 * ENV: first, then S: - the same fallback sana2loop uses, and for the same
 * two reasons. ENV: is where a configured system keeps this; S: is reachable
 * in a minimal boot that has no ENV: assign and no C: commands to make one
 * with, which is exactly the Copperline smoke-test environment.
 *
 * Cast at the use site: Open() takes STRPTR (UBYTE *), not char *.
 */
static const char *const config_paths[] = {
    "ENV:partunit/config",
    "S:partunit/config",
    NULL
};

#define CONFIG_MAX  2048

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Returns the token's length and advances *p past it. */
static int next_token(char **p, char *out, int outmax)
{
    char *s = *p;
    int   n = 0;

    while (*s != '\0' && is_space(*s)) {
        s++;
    }
    while (*s != '\0' && !is_space(*s)) {
        if (n < outmax - 1) {
            out[n] = *s;
        }
        n++;
        s++;
    }
    out[n < outmax ? n : outmax - 1] = '\0';
    *p = s;
    return n;
}

static int str_eq_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char cb = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static ULONG parse_ulong(const char *s)
{
    ULONG v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s != '\0') {
            char c = *s++;
            if (c >= '0' && c <= '9')      v = v * 16 + (ULONG)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (ULONG)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (ULONG)(c - 'A' + 10);
            else break;
        }
        return v;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (ULONG)(*s - '0');
        s++;
    }
    return v;
}

/* One DISK line. Starts the disk's task, which probes the child and scans
 * for partitions before reporting back. */
static void add_disk(struct DeviceBase *dev, const char *name, ULONG unit,
                     int force)
{
    struct PUDisk *pd;

    if (dev->db_NumDisks >= MAX_DISKS) {
        return;
    }
    pd = AllocMem(sizeof(struct PUDisk), MEMF_PUBLIC | MEMF_CLEAR);
    if (pd == NULL) {
        return;
    }

    strncpy(pd->pd_Name, name, sizeof(pd->pd_Name) - 1);
    pd->pd_Name[sizeof(pd->pd_Name) - 1] = '\0';
    pd->pd_ChildUnit = unit;
    pd->pd_DiskIndex = dev->db_NumDisks;
    pd->pd_Force     = (UBYTE)(force ? 1 : 0);

    DBG("PU: disk_start\n");
    if (pu_disk_start(dev, pd) != 0) {
        DBG("PU: disk_start failed\n");
        FreeMem(pd, sizeof(struct PUDisk));
        return;
    }

    AddTail((struct List *)&dev->db_Disks, (struct Node *)&pd->pd_Node);
    dev->db_NumDisks++;
}

static void parse_config(struct DeviceBase *dev, char *text)
{
    char *line = text;

    while (*line != '\0') {
        char *eol = line;
        char *p;
        char  tok[64];

        while (*eol != '\0' && *eol != '\n') {
            eol++;
        }
        if (*eol == '\n') {
            *eol++ = '\0';
        }

        /* Comments run to end of line. */
        for (p = line; *p != '\0'; p++) {
            if (*p == ';' || *p == '#') {
                *p = '\0';
                break;
            }
        }

        p = line;
        if (next_token(&p, tok, sizeof(tok)) > 0) {
            if (str_eq_ci(tok, "DISK")) {
                char devname[32];
                char unitstr[16];

                if (next_token(&p, devname, sizeof(devname)) > 0 &&
                    next_token(&p, unitstr, sizeof(unitstr)) > 0) {
                    ULONG unit  = parse_ulong(unitstr);
                    int   force = 0;
                    char  opt[16];

                    while (next_token(&p, opt, sizeof(opt)) > 0) {
                        if (str_eq_ci(opt, "FORCE")) {
                            force = 1;
                        }
                        /* POLL and MAXTRANSFER are Phase 2/3; ignored for
                         * now rather than treated as an error, so an
                         * existing config does not break. */
                    }
                    add_disk(dev, devname, unit, force);
                }
            } else if (str_eq_ci(tok, "NOMOUNT")) {
                dev->db_NoMount = 1;
            }
        }

        line = eol;
    }
}

/*
 * A plain global that this toolchain's <proto/dos.h> inline stubs reference
 * by this exact name. dos.library has no fixed low-memory pointer the way
 * Exec does, so it must be OpenLibrary()'d first; <proto/dos.h> declares
 * `extern struct DosLibrary *DOSBase`, and matching that exact type here is
 * what actually supplies its storage.
 *
 * Opened, used and closed within this one function, every time - the device
 * must not hold dos.library open, and a KS1.3 target has no business
 * assuming it is even available.
 */
struct DosLibrary *DOSBase;

void pu_config_load(struct DeviceBase *dev)
{
    BPTR  fh;
    char *buf;
    LONG  got;

    DBG("PU: config_load\n");
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (DOSBase == NULL) {
        return;
    }
    dev->db_DOSBase = (struct Library *)DOSBase;

    /*
     * First path that opens wins; a missing file is not an error, it just
     * means no disks are configured and so no units appear.
     *
     * CRITICAL: pr_WindowPtr must be -1 across these Opens.
     *
     * Naming a volume that is not mounted - "ENV:" on a system with no ENV:
     * assign, which is an ordinary configuration and not an exotic one -
     * makes DOS put up a "Please insert volume ENV:" requester and wait for
     * it. In a headless or Workbench-less boot there is nothing to display
     * it on and nobody to click it, so the Open never returns and the
     * caller's OpenDevice hangs forever. Setting pr_WindowPtr to -1 tells
     * DOS to fail the call instead of asking, which is the only acceptable
     * behaviour for a device: a missing config file must never be able to
     * wedge the machine.
     *
     * Found exactly this way - the first on-target run hung here, which no
     * host test could have shown.
     */
    {
        struct Process *me = (struct Process *)SysBase->ThisTask;
        APTR            saved = me->pr_WindowPtr;
        int             i;

        me->pr_WindowPtr = (APTR)-1;
        fh = 0;
        for (i = 0; config_paths[i] != NULL; i++) {
            fh = Open((STRPTR)config_paths[i], MODE_OLDFILE);
            if (fh != 0) {
                break;
            }
        }
        me->pr_WindowPtr = saved;
    }
    if (fh == 0) {
        CloseLibrary((struct Library *)DOSBase);
        dev->db_DOSBase = NULL;
        return;
    }

    buf = AllocMem(CONFIG_MAX + 1, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL) {
        Close(fh);
        CloseLibrary((struct Library *)DOSBase);
        dev->db_DOSBase = NULL;
        return;
    }

    got = Read(fh, buf, CONFIG_MAX);
    Close(fh);
    if (got > 0) {
        buf[got] = '\0';
        parse_config(dev, buf);
    }

    FreeMem(buf, CONFIG_MAX + 1);
    CloseLibrary((struct Library *)DOSBase);
    dev->db_DOSBase = NULL;
}

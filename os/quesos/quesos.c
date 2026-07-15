/* ==================================================================================
 * quesos.c -- quesOS kernel core, guest-C portion (card maize-24, Piece 3).
 *
 * quesOS is the first Phase-2 build (North Star doc #12): a directly-loaded guest
 * image that acts as init, owns the cause-7 (SYS) trap vector, dispatches the
 * Linux-ABI syscall subset in guest code, and execs a program image, reaps its
 * exit status, and runs the next. First increment is SINGLE-TASKING: one child at
 * a time, flat v1.0 memory, no protection (design-of-record: workbench doc #13).
 *
 * This C file is the portable half. The metal (the cause-7 handler entry/exit, the
 * SETSYSG/CLRSYSG provider toggle, the control transfer to a child, the private-
 * stack switch) lives in quesos_boot.mazm. The two halves share these globals and
 * the extern function boundary below.
 *
 * Provider-flag invariant (D9 Shape B): quesOS's OWN code always runs with the
 * syscall-guest flag CLEAR, so quesOS's own SYS stubs (sys_open/sys_read/sys_close)
 * reach the native sys::call provider directly and cannot recurse into the handler.
 * The flag is SET only while a child executes; quesos_enter_child sets it just
 * before jumping to the child, and the cause-7 handler clears it again on the exit
 * path before returning control here. See quesos_boot.mazm for the handler.
 * ================================================================================== */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* --- Raw syscall stubs (toolchain/rt/syscall.mazm), reached natively because
 *     quesOS runs flag-clear. Declared here; cproc is strict C11 and needs the
 *     prototype, the symbol resolves through mzld's global table. ----------------- */
long sys_open(const char *path, long flags, long mode);
long sys_read(long fd, void *buf, long count);
long sys_close(long fd);
long sys_write(long fd, const void *buf, long count);

/* --- The metal half (quesos_boot.mazm). ---------------------------------------- */
void quesos_enter_child(u64 entry, u64 rs); /* SETSYSG; RS=rs; JMP entry (noreturn) */
void quesos_poweroff(void);                 /* CLRSYSG; SYS $3C (native VM halt)     */

/* quesOS's private stack (quesos_boot.mazm switches RS here at _start and resets to
 * its top on every child-exit trap). 64 KiB; the boot asm hardcodes the same size
 * as a +0x10000 offset from this base, so keep the two in lockstep. */
#define QUESOS_STACK_SIZE 0x10000u
u8 quesos_stack[QUESOS_STACK_SIZE];

/* Handoff globals for quesos_enter_child, filled by quesos_load_image. */
u64 g_child_entry;
u64 g_child_rs;

/* Single-slot-per-child process table (single-tasking). The worklist is quesOS's
 * own argv[1..] (decision D7): each entry is a child .mzx guest path to exec+reap. */
#define QUESOS_MAX_CHILDREN 16
#define QUESOS_PATH_CAP     256
static char  g_pathbuf[QUESOS_MAX_CHILDREN][QUESOS_PATH_CAP];
static char *g_paths[QUESOS_MAX_CHILDREN];
static long  g_status[QUESOS_MAX_CHILDREN];
static u8    g_reaped[QUESOS_MAX_CHILDREN];
static long  g_count;
static long  g_next;

/* Whole child image is slurped here before segment placement. hello.mzx is ~31 KiB;
 * 256 KiB is generous headroom for a full-RT-linked static child. */
#define QUESOS_FILEBUF_CAP (256u * 1024u)
static u8 g_filebuf[QUESOS_FILEBUF_CAP];

/* Top of guest RAM, where the child's argc/argv/envp start block is built, matching
 * src/maize.cpp build_process_start_block exactly (decision D6, no auxv). */
#define GUEST_RAM_TOP 0xFFFFFFFFFFFFFFF8ul

/* .mzx on-disk constants (src/maize_obj.h / src/maize.cpp load_mzx). */
#define MZX_HEADER_SIZE 24u
#define MZX_SEGMENT_SIZE 40u

/* --- Freestanding mem primitives. Defined non-static so any implicit memcpy/memset
 *     the compiler emits also resolves here; quesOS links no libc. -------------- */
void *memcpy(void *dst, const void *src, unsigned long n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) { *d++ = *s++; }
    return dst;
}

void *memset(void *dst, int c, unsigned long n) {
    u8 *d = (u8 *)dst;
    while (n--) { *d++ = (u8)c; }
    return dst;
}

static unsigned long qos_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) { ++n; }
    return n;
}

/* Little-endian field reads out of the slurped file buffer (endian-explicit so the
 * parse is correct regardless of the buffer's alignment). */
static u16 rd_u16(const u8 *b, unsigned long off) {
    return (u16)((u32)b[off] | ((u32)b[off + 1] << 8));
}

static u64 rd_u64(const u8 *b, unsigned long off) {
    u64 v = 0;
    int i;
    for (i = 7; i >= 0; --i) { v = (v << 8) | (u64)b[off + (unsigned)i]; }
    return v;
}

/* --- Minimal console output over native write (flag-clear passthrough). --------- */
static void qos_puts(const char *s) {
    sys_write(1, s, (long)qos_strlen(s));
}

static void qos_put_u64(u64 v) {
    char tmp[24];
    int i = 0;
    if (v == 0) { sys_write(1, "0", 1); return; }
    while (v > 0 && i < 24) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    /* digits landed least-significant-first; emit reversed. */
    while (i > 0) { char c = tmp[--i]; sys_write(1, &c, 1); }
}

/* --- execve: resolve, load, build the child stack (AC2). ----------------------- */
/* Reads the whole child .mzx into g_filebuf; returns the byte count, or -1. */
static long quesos_slurp(const char *path) {
    long fd = sys_open(path, 0 /* O_RDONLY */, 0);
    long total = 0;
    if (fd < 0) { return -1; }
    for (;;) {
        long n = sys_read(fd, g_filebuf + total, (long)(QUESOS_FILEBUF_CAP - (u64)total));
        if (n < 0) { sys_close(fd); return -1; }
        if (n == 0) { break; }
        total += n;
        if ((u64)total >= QUESOS_FILEBUF_CAP) { break; }
    }
    sys_close(fd);
    return total;
}

/* Build the child's initial stack at the top of RAM, byte-for-byte in the shape of
 * src/maize.cpp build_process_start_block: [argc][argv..][NULL][envp..][NULL] with
 * the NUL-terminated strings packed down from GUEST_RAM_TOP. One arg (argv[0] = the
 * child path), empty envp, no auxv (decision D6). Sets g_child_rs. */
static void quesos_build_child_stack(const char *path) {
    u64 plen = (u64)qos_strlen(path) + 1;
    u64 string_base = GUEST_RAM_TOP - plen;
    u64 argv0 = string_base;
    u64 cur = string_base;
    u64 k;
    u64 ptr_block_size;
    u64 rs;
    u64 p;

    for (k = 0; k < plen - 1; ++k) { *(u8 *)(cur++) = (u8)path[k]; }
    *(u8 *)(cur++) = 0;

    /* argc + (argv[0] + NULL) + (envp NULL) = 1 + 2 + 1 = 4 words. */
    ptr_block_size = 8u * (1u + 2u + 1u);
    rs = (string_base - ptr_block_size) & ~(u64)7;

    p = rs;
    *(u64 *)p = 1;      p += 8;   /* argc                */
    *(u64 *)p = argv0;  p += 8;   /* argv[0]             */
    *(u64 *)p = 0;      p += 8;   /* argv NULL terminator */
    *(u64 *)p = 0;      p += 8;   /* envp NULL terminator */

    g_child_rs = rs;
}

/* Parse the .mzx header + segment table and place each segment at its vaddr in the
 * shared flat address space, zero-filling the BSS tail. Returns 0 on success. */
static int quesos_load_image(const char *path) {
    long size = quesos_slurp(path);
    const u8 *b = g_filebuf;
    u16 seg_count;
    u64 entry;
    u64 shoff;
    u16 i;

    if (size < (long)MZX_HEADER_SIZE) {
        qos_puts("[quesos] cannot read image: ");
        qos_puts(path);
        qos_puts("\n");
        return -1;
    }
    if (b[0] != 'M' || b[1] != 'Z' || b[2] != 'X' || b[3] != 0x01) {
        qos_puts("[quesos] not a .mzx image: ");
        qos_puts(path);
        qos_puts("\n");
        return -1;
    }

    seg_count = rd_u16(b, 6);
    entry     = rd_u64(b, 8);
    shoff     = rd_u64(b, 16);

    for (i = 0; i < seg_count; ++i) {
        u64 so        = shoff + (u64)i * MZX_SEGMENT_SIZE;
        u64 vaddr     = rd_u64(b, so + 8);
        u64 file_off  = rd_u64(b, so + 16);
        u64 mem_size  = rd_u64(b, so + 24);
        u64 file_size = rd_u64(b, so + 32);
        u64 j;

        for (j = 0; j < file_size; ++j) { *(u8 *)(vaddr + j) = b[file_off + j]; }
        for (j = file_size; j < mem_size; ++j) { *(u8 *)(vaddr + j) = 0; }
    }

    quesos_build_child_stack(path);
    g_child_entry = entry;
    return 0;
}

/* --- The reap loop. -------------------------------------------------------------- */
/* Load and enter the next worklist child; power the VM off when the list is drained. */
static void quesos_run_next(void) {
    for (;;) {
        if (g_next >= g_count) {
            quesos_poweroff();   /* noreturn: CLRSYSG; native SYS $3C */
        }
        if (quesos_load_image(g_paths[g_next]) != 0) {
            /* Skip an unloadable entry rather than wedge the machine. */
            g_status[g_next] = -1;
            g_reaped[g_next] = 1;
            ++g_next;
            continue;
        }
        quesos_enter_child(g_child_entry, g_child_rs);  /* noreturn: SETSYSG; jump */
    }
}

/* Called from the cause-7 handler's exit path (quesos_boot.mazm) after it has
 * cleared the provider flag and reset RS to quesOS's private stack. Records the
 * child's status, emits the reap evidence, and runs the next worklist entry. */
void quesos_reap(long code) {
    g_status[g_next] = code;
    g_reaped[g_next] = 1;

    qos_puts("[quesos] reaped ");
    qos_puts(g_paths[g_next]);
    qos_puts(" status=");
    qos_put_u64((u64)(code & 0xFF));
    qos_puts("\n");

    ++g_next;
    quesos_run_next();  /* noreturn */
}

/* Entry from _start (quesos_boot.mazm), after the handler is installed at vector 7
 * and RS has been switched to quesOS's private stack. argc/argv are quesOS's own,
 * marshalled by _start off the host process-start block; argv[1..] is the exec
 * worklist (decision D7). The argv strings live at the top of RAM, which child
 * stacks overwrite, so copy them into g_pathbuf BEFORE loading any child. */
void quesos_main(long argc, char **argv) {
    long i;

    g_count = 0;
    g_next = 0;

    for (i = 1; i < argc && g_count < QUESOS_MAX_CHILDREN; ++i) {
        const char *src = argv[i];
        long j = 0;
        while (src[j] && j < QUESOS_PATH_CAP - 1) {
            g_pathbuf[g_count][j] = src[j];
            ++j;
        }
        g_pathbuf[g_count][j] = 0;
        g_paths[g_count] = g_pathbuf[g_count];
        ++g_count;
    }

    if (g_count == 0) {
        qos_puts("[quesos] no programs on the exec worklist; powering off\n");
        quesos_poweroff();
    }

    qos_puts("[quesos] init: cause-7 handler resident; running ");
    qos_put_u64((u64)g_count);
    qos_puts(" program(s)\n");

    quesos_run_next();  /* noreturn */
}

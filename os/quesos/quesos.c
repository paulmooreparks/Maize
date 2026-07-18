/* ==================================================================================
 * quesos.c -- quesOS kernel core, guest-C portion.
 *
 * quesOS is a directly-loaded guest image that acts as init: it owns the cause-7
 * (SYS) trap vector, dispatches the Linux-ABI syscall subset in GUEST code, and runs
 * user programs. The maize-24 keystone ran ONE program at a time in a flat, bare-mode
 * address space. maize-93 grows quesOS to a MULTI-PROCESS kernel running on the Sv48
 * paging MMU (maize-180/194): every process gets its own page table (address-space
 * isolation), fork copies the parent's pages eagerly, and a round-robin scheduler over
 * the instruction-tick timer bounds compute-bound processes. Process calls stay
 * GUEST-ONLY, in this dispatcher, never added to src/sys.cpp (the maize-24 rule).
 *
 * Memory model (physical addresses; quesOS builds per-process Sv48 tables mapping VAs
 * to these). The kernel region is mapped identically (U=0, supervisor) into EVERY
 * process table so a trap handler keeps executing across a MOVTCR address-space swap.
 *
 *   0x00001000  trap vector table page             kernel (U=0), one 4 KiB leaf
 *   0x00002000 .. 0x000FFFFF  user VA region        per-process user pages (U=1), region 0
 *   0x00100000 .. 0x001FFFFF  quesOS image (code+data)   kernel (U=0), 4 KiB leaves
 *   0x00200000 .. 0x003FFFFF  fb-mmap window (maize-238)  per-process, lazy L0 (region 1)
 *   0x00400000 .. 0x043FFFFF  frame + page-table pool     kernel (U=0), 2 MiB superpages
 *
 * A user process's own pages are DISTINCT physical frames drawn from the pool and
 * mapped U=1 in the process's own table; that separation (not the U bit) is what makes
 * fork's two address spaces independent. quesOS runs supervisor; a process runs user
 * (privilege clear, syscall-guest set so its SYS traps here, interrupts enabled so the
 * timer preempts it). The trap trampoline and the context switch live in
 * quesos_boot.mazm; the two halves share the globals and the extern boundary below.
 * ================================================================================== */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* --- Raw syscall stubs (toolchain/rt/syscall.mazm), reached natively because the
 *     trap trampoline runs the C dispatcher with the syscall-guest flag CLEAR. ------ */
long sys_open(const char *path, long flags, long mode);
long sys_read(long fd, void *buf, long count);
long sys_close(long fd);
long sys_write(long fd, const void *buf, long count);
/* maize-94 decision 8941: the remaining native hostfs subset a quesOS process forwards
 * through this dispatcher (each mirrors the frozen native number, SYSCALL-ABI.md). */
long sys_fstat(long fd, void *statbuf);
long sys_lseek(long fd, long offset, long whence);
long sys_getdents64(long fd, void *dirp, long count);
long sys_unlink(const char *path);
long sys_mkdir(const char *path, long mode);
long sys_rename(const char *oldp, const char *newp);
long sys_ftruncate(long fd, long length);
/* maize-94 (OQ 8951 operator ruling): the native console termios calls ($F1/$F2). A
 * quesOS process's SYS $F1/$F2 traps here and is forwarded to these native stubs, so a
 * raw-mode shell (oksh emacs line editing) can drive the window console's line
 * discipline. NO new syscall numbers: the existing native $F1/$F2 are reused. */
long sys_tcgetattr(long fd, void *termios_p);
long sys_tcsetattr(long fd, long optional_actions, void *termios_p);
long sys_ttysize(long fd, void *winsize_p);   /* maize-94: native $F6, forwarded to guests */
unsigned long sys_clock_ms(void);   /* maize-94: native $F0, forwarded to guests */

/* --- The metal half (quesos_boot.mazm). ---------------------------------------------
 * quesos_switch_to(pcb) loads the process's saved GP context, MOVTCRs its page-table
 * root into CR0, and IRETs into it (first entry or resume); it never returns. */
struct pcb;
void quesos_switch_to(struct pcb *p);        /* MOVTCR satp; restore regs; IRET (noreturn) */
void quesos_poweroff(void);                  /* CLRSYSG; SYS $3C (native VM halt)          */
void quesos_arm_timer(void);                 /* program the instruction-tick timer (OUT)   */
void quesos_idle(void);                      /* SETINT; spin so on_input_tick keeps running */
u64  quesos_con_status(void);                /* IN $01: console status (bit0 input-avail)  */
u64  quesos_con_data(void);                  /* IN $00: console data byte (clears avail)   */
/* maize-174: the user-mode signal-return trampoline lives in quesos_boot.mazm; its bytes
 * are copied onto a delivering process's stack and RET'd into, so it needs no fixed VA. */
extern u8 quesos_sigreturn_tramp[];
extern u8 quesos_sigreturn_tramp_end[];
/* maize-236: framebuffer registration-table ports (privileged IN/OUT, supervisor only). */
u64  quesos_fb_width(void);                  /* IN $50: framebuffer width (pixels)         */
u64  quesos_fb_height(void);                 /* IN $51: framebuffer height (pixels)        */
u64  quesos_fb_format(void);                 /* IN $52: pixel format id (1 = XRGB8888)     */
void quesos_fb_slot_select(u64 slot);        /* OUT $56: select the slot for base/status   */
u64  quesos_fb_base_read(void);              /* IN $53: selected slot's base (0=unclaimed) */
void quesos_fb_base_write(u64 base);         /* OUT $53: nonzero claims, zero releases      */
u64  quesos_fb_status_read(void);            /* IN $55: selected slot status (bit2=reject) */

/* quesOS's private kernel stack (quesos_boot.mazm switches RS here at _start and on
 * every trap entry, so the C dispatcher never runs on a user stack). 64 KiB. */
#define QUESOS_STACK_SIZE 0x10000u
u8 quesos_stack[QUESOS_STACK_SIZE];

/* ==================================================================================
 * Physical memory layout + Sv48 constants.
 * ================================================================================== */
#define TRAP_TABLE_PA     0x00001000ul
#define USER_STACK_TOP    0x00100000ul   /* user VA just above the stack region        */
#define USER_STACK_PAGES  16u            /* 64 KiB user stack, [0xF0000, 0x100000)      */
#define USER_BRK_MAX      0x000F0000ul   /* maize-94: heap ceiling (stack bottom)       */
#define QUESOS_IMG_BASE   0x00100000ul   /* quesOS link base; code+data in [base,+1MiB) */
#define QUESOS_IMG_TOP    0x00200000ul
#define FB_MMAP_BASE      0x00200000ul   /* maize-238: per-process fb-mmap window (region 1) */
#define POOL_BASE         0x00400000ul   /* frame + page-table pool (2 MiB aligned)     */
#define POOL_SUPERPAGES   32u            /* 32 * 2 MiB = 64 MiB pool                     */
#define POOL_TOP          (POOL_BASE + (u64)POOL_SUPERPAGES * 0x200000ul)
#define PAGE_SIZE         0x1000ul

/* Sv48 PTE flags (RISC-V shaped; see src/cpu.cpp sv48_walk). Leaf iff R|W|X != 0. */
#define PTE_V   0x1ul
#define PTE_R   0x2ul
#define PTE_W   0x4ul
#define PTE_X   0x8ul
#define PTE_U   0x10ul
#define PTE_KERNEL (PTE_V | PTE_R | PTE_W | PTE_X)          /* supervisor RWX, U=0 */
#define PTE_USER   (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U)  /* user RWX,       U=1 */

/* RF word IRET restores when a process (re)starts: user mode (privilege clear),
 * interrupts enabled (timer preempts), syscall-guest set (SYS traps to quesOS),
 * running set. Bit positions from src/cpu.cpp: privilege 1<<32, interrupt_enabled
 * 1<<33, running 1<<35, syscall_guest 1<<36. */
#define RF_INT_ENABLED   (1ul << 33)
#define RF_RUNNING       (1ul << 35)
#define RF_SYSCALL_GUEST (1ul << 36)
#define USER_RF (RF_INT_ENABLED | RF_RUNNING | RF_SYSCALL_GUEST)

/* Guest syscall numbers this dispatcher recognizes. The file/IO subset mirrors the
 * native SYS table (SYSCALL-ABI.md); the process calls are quesOS-private (guest-only,
 * a namespace distinct from the native table per that doc). */
#define SYS_read   0x00
#define SYS_write  0x01
#define SYS_open   0x02
#define SYS_close  0x03
#define SYS_brk    0x0C   /* maize-94: heap break, implemented against the process page table */
#define SYS_pipe   0x16   /* Linux x86-64 numbers below */
#define SYS_dup    0x20
#define SYS_dup2   0x21
#define SYS_getpid 0x27
#define SYS_fork   0x39
#define SYS_execve 0x3B
#define SYS_exit   0x3C
#define SYS_wait4  0x3D

/* maize-94 decision 8941: native hostfs file/dir syscalls forwarded to a quesOS process.
 * Each mirrors its frozen native number (SYSCALL-ABI.md); the handlers below bounce the
 * user buffer through g_iobuf exactly as do_read/do_write do. */
#define SYS_fstat      0x05
#define SYS_lseek      0x08
#define SYS_ftruncate  0x4D
#define SYS_rename     0x52
#define SYS_mkdir      0x53
#define SYS_unlink     0x57
#define SYS_getdents64 0xD9

/* maize-94 decision 8940 (numbering ruled by this card): per-process working directory.
 * SYS_chdir/SYS_getcwd are NEW quesOS-guest-only calls (no native VM/ISA change, the same
 * class as fork/execve/pipe from maize-93). They mirror the Linux x86-64 numbers
 * (chdir=80=$50, getcwd=79=$4F) per the SYSCALL-ABI.md numbering policy; neither collides
 * with a number this guest dispatcher already handles. */
#define SYS_getcwd 0x4F
#define SYS_chdir  0x50

/* maize-94 (OQ 8951 operator ruling): the native console termios calls, forwarded to a
 * quesOS process so oksh can enter raw mode. These reuse the existing native numbers; no
 * new number is minted. */
#define SYS_tcgetattr 0xF1
#define SYS_tcsetattr 0xF2

/* maize-94: sys_ttysize (native $F6, maize-228). An interactive oksh queries the terminal
 * size (ioctl TIOCGWINSZ) during line-editor init; forwarding it (same fd-mapped, bounce-
 * through-g_iobuf pattern as $F1/$F2) lets the shell get the real console dimensions and
 * reach its prompt instead of stranding on an unhandled syscall on the DEFAULT input path.
 * A non-native (pipe) fd is -ENOTTY, so the no-tty interactive path (oksh -i) degrades
 * gracefully to its default size. */
#define SYS_ttysize   0xF6

/* maize-94: the monotonic-clock and bulk-memory Maize-private numbers a borrowed guest
 * reaches. SYS_clock_ms takes no pointer args, so quesOS forwards it straight to the
 * native provider (quesOS runs on bare metal; its own sys_clock_ms() re-traps to $F0).
 * SYS_bulk_copy/SYS_bulk_set take USER virtual addresses for src/dst, which are not valid
 * in the native provider's flat physical view under per-process paging, so they cannot be
 * forwarded as-is; quesOS returns -ENOSYS and the guest RT memcpy/memset fall back to
 * their portable word loop (toolchain/rt/string.c). Handled explicitly (rather than via
 * the default) so the fallback is silent instead of printing an alarming "unhandled". */
#define SYS_clock_ms  0xF0
#define SYS_bulk_copy 0xF4
#define SYS_bulk_set  0xF5

/* The 4-word + NCCS-byte termios wire image (toolchain/rt/termios.h, src/console_io.h). */
#define TERMIOS_WIRE_SIZE 36
/* struct winsize wire image: ws_row, ws_col, ws_xpixel, ws_ypixel (four u16 LE, src/sys.cpp $F6). */
#define WINSIZE_WIRE_SIZE 8
/* c_lflag ISIG bit (Linux value; toolchain/rt/termios.h). */
#define TIO_ISIG 0x0001u
/* c_lflag ICANON bit (Linux value; toolchain/rt/termios.h). A raw-mode process clears it. */
#define TIO_ICANON 0x0002u
/* Byte offset of c_lflag (the 4th 32-bit LE word) within the 36-byte termios wire image. */
#define TERMIOS_OFF_LFLAG 12
/* struct stat wire size (toolchain/rt/sys/stat.h: 144 bytes, hostfs.md section 2). */
#define STAT_WIRE_SIZE 144
/* O_DIRECTORY (toolchain/rt/fcntl.h) for chdir's existence/is-a-directory validation. */
#define QOS_O_DIRECTORY 0x10000

/* maize-174: guest signal subsystem. The six Linux-mirror numbers plus two Maize-private
 * job-control numbers ($FA/$FB) drawn from the SYSCALL-ABI.md $F0-$FF ledger. */
#define SYS_rt_sigaction   0x0D
#define SYS_rt_sigprocmask 0x0E
#define SYS_rt_sigreturn   0x0F
#define SYS_kill           0x3E
#define SYS_setpgid        0x6D
#define SYS_getpgid        0x79
#define SYS_tcgetpgrp      0xFA
#define SYS_tcsetpgrp      0xFB

/* maize-174: signal numbers (Linux values, matching the syscall numbering convention). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGTERM  15
#define SIGCHLD  17

/* maize-236: framebuffer registration syscalls (Decision D6). These are quesOS-private,
 * guest-only, and Maize-specific display arbitration with no real Linux syscall-number
 * analog (Linux does it via ioctl on /dev/fb0 or a VT ioctl). Decision D6 names the
 * 0x1000-0x1002 range, but the SYS instruction rides only an 8-bit number into the cause-7
 * frame (src/cpu.cpp operand1.b0()), and widening it would be an ISA change this card
 * excludes. The frozen numbering policy (toolchain/rt/SYSCALL-ABI.md) puts every
 * no-Linux-equivalent call in the reserved Maize-private high block $F0-$FF; the low bytes
 * $F0-$F6 are already taken (sys_clock_ms $F0, termios $F1/$F2, palette_blit $F3,
 * bulk_copy $F4, bulk_set $F5, ttysize $F6), so the next genuinely-free numbers are
 * $F7-$F9. (Review cycle 1 rejected the earlier $E0-$E2 because those are real Linux
 * numbers, 224/225/226 timer_gettime family; Convention counterexamples Entry 5. It named
 * "$F3+" as free, but $F3-$F6 are occupied by the native private calls above, so the
 * genuinely-next-free numbers are $F7-$F9.) The syscall CONTRACT is unchanged. */
#define SYS_fb_geometry 0xF7   /* (u64 out_uva)  -> long (0)                              */
#define SYS_fb_register 0xF8   /* (u64 base_uva) -> long (slot, or -errno)                */
#define SYS_fb_release  0xF9   /* (void)         -> long (0, or -errno)                   */

/* maize-238 Phase 3: unix-domain sockets, select/poll, framebuffer mmap. The socket
 * family mirrors the Linux x86-64 numbers (socket 41=$29, bind 49=$31, connect 42=$2A,
 * listen 50=$32, accept 43=$2B, socketpair 53=$35, poll 7=$07, select 23=$17) per the
 * numbering policy; fb_mmap is Maize-private ($FC, next free after the $F7-$FB block). */
#define SYS_socket     0x29
#define SYS_bind       0x31
#define SYS_connect    0x2A
#define SYS_listen     0x32
#define SYS_accept     0x2B
#define SYS_socketpair 0x35
#define SYS_poll       0x07
#define SYS_select     0x17
#define SYS_fb_mmap    0xFC   /* (void) -> long (VA of the mapped fb buffer, or -errno)   */

/* maize-238: AF_UNIX SOCK_STREAM only (the X11-transport shape; maize-90 scoping). */
#define AF_UNIX     1
#define SOCK_STREAM 1

/* maize-238 errno magnitudes for the socket/mmap surface (real Linux values). */
#define QOS_ENOMEM         12   /* alloc_frames_contig could not satisfy the fb request  */
#define QOS_EPROTONOSUPPORT 93  /* socket(): protocol != 0                                */
#define QOS_EOPNOTSUPP     95   /* reserved (Branch B fast-fail; not built under Branch A)*/
#define QOS_EAFNOSUPPORT   97   /* socket(): domain != AF_UNIX                            */
#define QOS_EADDRINUSE     98   /* bind(): the path is already bound                      */
#define QOS_ECONNREFUSED  111   /* connect(): no listener, or listen queue full          */

/* maize-236 per-exec rejection errno magnitudes (Decision D7; real Linux values so a
 * ported shell's strerror needs no Maize-specific table). */
#define QOS_ENODEV 19          /* no display attached (claim rejected by the device)      */
#define QOS_EBUSY  16          /* this process already holds a registration (Decision D3) */
#define QOS_EINVAL 22          /* base_uva page unmapped / zero                           */
#define QOS_ENOSPC 28          /* registration table full                                 */
#define QOS_EBADF   9          /* release with nothing registered                         */
#define QOS_ESRCH   3          /* maize-174: kill target pid/pgid names no process        */
#define QOS_ENOENT  2          /* maize-94: execve target path does not exist             */
#define QOS_ENOEXEC 8          /* maize-94: execve target is not a loadable .mzx image    */

/* maize-236: registration-table capacity, mirroring src/maize_cpu.h fb_max_slots. */
#define QUESOS_FB_MAX_SLOTS 8

/* ==================================================================================
 * Process table.
 * ================================================================================== */
#define QUESOS_MAX_PROC 24
#define QUESOS_PATH_CAP 256
#define QUESOS_MAX_FD   16       /* per-process file descriptors                        */

enum proc_state { P_FREE = 0, P_RUNNABLE, P_BLOCKED, P_ZOMBIE };

/* Why a process is BLOCKED (block_kind), so the right waker completes its syscall. */
#define BLK_NONE    0
#define BLK_WAIT    1            /* parked in wait4 (woken by an exiting child)         */
#define BLK_PIPE_R  2            /* parked reading an empty pipe (woken by a writer)    */
#define BLK_PIPE_W  3            /* parked writing a full pipe (woken by a reader)      */
#define BLK_CONSOLE 4            /* parked reading fd 0 (woken by the console IRQ, v33)  */
#define BLK_CONNECT 5            /* maize-238: parked in connect() (woken by accept())   */
#define BLK_ACCEPT  6            /* maize-238: parked in accept() (woken by connect())   */
#define BLK_POLL    7            /* maize-238: parked in poll()/select() (readiness wake)*/

/* saved_rs (offset 0) and root_pa (offset 8) are read by the quesos_switch_to metal at
 * those exact byte offsets; keep them first and in this order. */
struct pcb {
    u64 saved_rs;                 /* offset 0:  RS at the saved-regs block (context)   */
    u64 root_pa;                  /* offset 8:  page-table root physical (satp = |1)   */
    u64 l0_pa;                    /* [0,2 MiB) L0 table physical (user-page mapping)    */
    u64 l1_pa;                    /* maize-238: L1 table physical (lazy multi-L0 walk)  */
    long pid;
    long parent;
    int  state;
    long exit_status;
    long wait_for;                /* waitpid target while BLOCKED (-1 = any child)     */
    u64  wait_status_uva;         /* user address to receive the wait status, or 0     */
    int  block_kind;              /* why BLOCKED: see BLK_* (wait vs a pipe end)       */
    int  block_pipe;             /* pipe index for a blocked pipe read/write          */
    u64  block_buf;              /* user buffer VA for a blocked pipe read/write      */
    long block_count;            /* requested byte count for a blocked pipe op        */
    int  fd[QUESOS_MAX_FD];      /* per-process fd table: open-file-description index  */
    long fb_slot;                /* maize-236: held framebuffer slot, or -1 (none)     */
    long pgid;                   /* maize-174: process group id (fg-group signal target)*/
    unsigned long pending;       /* maize-174: pending-signal bitmask (bit sig-1)      */
    unsigned long blocked;       /* maize-174: sigprocmask block mask                  */
    u64  handler[32];            /* maize-174: per-signal action VA (0=DFL, 1=IGN)     */
    long term_signal;            /* maize-174: terminating signal (0 = normal _exit)   */
    u64  sig_saved_rs;           /* maize-174: saved_rs to restore on rt_sigreturn     */
    int  in_handler;             /* maize-174: a signal handler is in progress          */
    char path[QUESOS_PATH_CAP];   /* argv[0] for the reap transcript                   */
    char cwd[QUESOS_PATH_CAP];    /* maize-94 decision 8940: per-process cwd (def "/") */
    u64  brk_cur;                 /* maize-94: current heap break (grows via SYS_brk)  */
    unsigned char termios_img[TERMIOS_WIRE_SIZE];  /* maize-250: last termios this proc set */
    int  termios_valid;           /* maize-250: 0 until this proc's first tcsetattr     */
    /* maize-238 Family B: poll()/select() park state. Read back by poll_recheck_all and
     * the timer timeout sweep to re-evaluate a parked caller's fd set. */
    long poll_fds_uva;            /* poll(): user VA of the pollfd array (select: unused)*/
    long poll_nfds;               /* poll()/select(): fd count                          */
    int  poll_mode;               /* 0 = poll(), 1 = select()                           */
    u64  poll_r_uva, poll_w_uva, poll_e_uva;  /* select(): the three fd_set VAs (0=NULL) */
    u64  poll_deadline_ms;        /* 0 = block forever; else absolute sys_clock_ms deadline */
    /* maize-238 Family C: the process's framebuffer mmap window base VA (0 = none). */
    long fb_mmap_va;
};

static struct pcb g_proc[QUESOS_MAX_PROC];
struct pcb *g_current;            /* the running process (read by the metal trampoline) */
static long g_next_pid = 1;

static void schedule(void);       /* round-robin scheduler; noreturn (defined below)    */
static void fb_release_held(struct pcb *p);   /* maize-236: release a held fb slot (exit/exec) */
static void deliver_pending_signal(struct pcb *p);   /* maize-174: apply a pending signal on resume */
static void terminate_by_signal(struct pcb *p, int sig);   /* maize-174: default-terminate (noreturn) */
static void reap_tail(struct pcb *self);             /* maize-174: shared zombie/reap/SIGCHLD tail */

/* Boot worklist: quesOS's own argv[1..] is the exec worklist (maize-24 decision D7). */
static char g_pathbuf[QUESOS_MAX_PROC][QUESOS_PATH_CAP];
static long g_worklist_count;

/* Whole child image is slurped here before segment placement. maize-94: raised from
 * 256 KiB to 640 KiB so the vendored oksh image (~458 KiB linked, the whole shell +
 * the freestanding libc slice) fits; the earlier 256 KiB cap silently truncated it and
 * load_segments then rejected it ("cannot start"). g_filebuf lives in quesOS's BSS
 * inside the [0x100000, 0x200000) 1 MiB image region; at 640 KiB it plus quesOS's code
 * (~32 KiB) and the rest of its static data (stack 64 KiB, pcbs, arg/io bounce buffers)
 * still fits comfortably under 1 MiB, verified against the linked image size. */
#define QUESOS_FILEBUF_CAP (640u * 1024u)
static u8 g_filebuf[QUESOS_FILEBUF_CAP];

/* Bounce buffer: user-pointer syscall args are copied through this identity-mapped
 * kernel staging area, because the native provider reads guest memory raw-physical
 * (no translation) and a user VA is not its own physical address under paging. */
#define QUESOS_IOBUF_CAP 4096u
static u8 g_iobuf[QUESOS_IOBUF_CAP];

/* .mzx on-disk constants (src/maize_obj.h / src/maize.cpp load_mzx). */
#define MZX_HEADER_SIZE  24u
#define MZX_SEGMENT_SIZE 40u

/* ==================================================================================
 * Freestanding primitives (quesOS links no libc).
 * ================================================================================== */
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

static u16 rd_u16(const u8 *b, unsigned long off) {
    return (u16)((u32)b[off] | ((u32)b[off + 1] << 8));
}

static u64 rd_u64(const u8 *b, unsigned long off) {
    u64 v = 0;
    int i;
    for (i = 7; i >= 0; --i) { v = (v << 8) | (u64)b[off + (unsigned)i]; }
    return v;
}

/* --- Console output over native write (dispatcher runs flag-clear). --------------- */
static void qos_puts(const char *s) {
    sys_write(1, s, (long)qos_strlen(s));
}

static void qos_put_u64(u64 v) {
    char tmp[24];
    int i = 0;
    if (v == 0) { sys_write(1, "0", 1); return; }
    while (v > 0 && i < 24) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i > 0) { char c = tmp[--i]; sys_write(1, &c, 1); }
}

/* ==================================================================================
 * Physical frame + page-table pool (bump allocator over identity-mapped RAM).
 * ================================================================================== */
static u64 g_pool_next = POOL_BASE;

static u64 alloc_frame(void) {
    u64 f = g_pool_next;
    if (f + PAGE_SIZE > POOL_TOP) {
        qos_puts("[quesos] PANIC: out of physical frames\n");
        quesos_poweroff();
    }
    g_pool_next += PAGE_SIZE;
    /* Zero the frame via its identity address (pool is identity-mapped). */
    memset((void *)f, 0, PAGE_SIZE);
    return f;
}

/* maize-238: a graceful batch allocator for the fb-mmap buffer. Unlike alloc_frame (which
 * PANICs and powers off the whole VM on exhaustion, acceptable for its small kernel-internal
 * single-frame requests), a large (~63-page), user-triggerable request that can legitimately
 * exhaust the pool across repeated exec cycles must fail cleanly. Checks headroom BEFORE
 * allocating (no partial allocation, no PANIC), then bumps g_pool_next by n pages in one
 * uninterrupted step -- so the n frames are physically contiguous with NO free-list needed
 * (the same atomicity argument as alloc_frame's implicit single-frame contiguity, since
 * quesOS's syscall dispatch is atomic w.r.t. the round-robin scheduler). Returns 0 and the
 * base physical address on success, -1 on exhaustion. */
static long alloc_frames_contig(u64 n, u64 *out_base_pa) {
    u64 base = g_pool_next;
    u64 i;
    if (n == 0) { return -1; }
    if (base + n * PAGE_SIZE > POOL_TOP) { return -1; }
    g_pool_next += n * PAGE_SIZE;
    for (i = 0; i < n; ++i) { memset((void *)(base + i * PAGE_SIZE), 0, PAGE_SIZE); }
    *out_base_pa = base;
    return 0;
}

/* Read/write a table entry via its identity (pool) address. */
static u64  pte_get(u64 table_pa, u64 idx)          { return ((u64 *)table_pa)[idx]; }
static void pte_set(u64 table_pa, u64 idx, u64 val) { ((u64 *)table_pa)[idx] = val; }

/* Build a fresh address space: allocate root/L2/L1/L0, wire the non-leaf chain, and map
 * the shared kernel region (trap page + quesOS image + pool superpages) identically.
 * Returns 0 on success. All user VAs [0, 2 MiB) live in the single L0. */
static int build_address_space(struct pcb *p) {
    u64 root = alloc_frame();
    u64 l2   = alloc_frame();
    u64 l1   = alloc_frame();
    u64 l0   = alloc_frame();
    u64 i;

    pte_set(root, 0, l2 | PTE_V);
    pte_set(l2,   0, l1 | PTE_V);
    pte_set(l1,   0, l0 | PTE_V);

    /* Trap vector table page (VPN0 = 1), kernel. */
    pte_set(l0, (TRAP_TABLE_PA >> 12) & 0x1FF, TRAP_TABLE_PA | PTE_KERNEL);

    /* quesOS image [0x100000, 0x200000): VPN0 256..511, kernel identity. */
    for (i = QUESOS_IMG_BASE; i < QUESOS_IMG_TOP; i += PAGE_SIZE) {
        pte_set(l0, (i >> 12) & 0x1FF, i | PTE_KERNEL);
    }

    /* Frame + page-table pool: 2 MiB identity superpages at L1 (VPN1 = 2..), kernel.
     * POOL_BASE 0x400000 -> VPN1 = 2. */
    for (i = 0; i < POOL_SUPERPAGES; ++i) {
        u64 sp = POOL_BASE + i * 0x200000ul;
        pte_set(l1, (sp >> 21) & 0x1FF, sp | PTE_KERNEL);
    }

    p->root_pa = root;
    p->l1_pa   = l1;
    p->l0_pa   = l0;
    return 0;
}

/* maize-238: resolve the L0 table for VA's 2 MiB region, allocating and linking a fresh
 * L0 in the process's L1 on first touch of a region above the pre-allocated region 0.
 * This lifts the old single-L0 cap (user VA <= 2 MiB) so a dedicated fb-mmap window
 * (FB_MMAP_BASE, region 1) coexists with the process's own code/data/heap in region 0.
 * Region 0's L0 stays pre-allocated at build_address_space (l1[0]), so this returns it
 * unchanged for VA < 2 MiB. Any 2 MiB region, not a hard-coded second special case. */
static u64 ensure_l0(struct pcb *p, u64 va) {
    u64 i1 = (va >> 21) & 0x1FF;
    u64 e  = pte_get(p->l1_pa, i1);
    if ((e & PTE_V) == 0) {
        u64 nl0 = alloc_frame();
        pte_set(p->l1_pa, i1, nl0 | PTE_V);
        return nl0;
    }
    return e & ~0xFFFul;
}

/* Non-allocating L0 lookup for an already-mapped VA (returns region 0's L0 fast, or the
 * region's linked L0; 0 if the region has no L0 yet). */
static u64 va_l0(struct pcb *p, u64 va) {
    u64 i1 = (va >> 21) & 0x1FF;
    if (i1 == 0) { return p->l0_pa; }
    return pte_get(p->l1_pa, i1) & ~0xFFFul;
}

/* Map one user 4 KiB page to a physical frame, U=1, in p's L0 for VA's region. */
static void map_user_page(struct pcb *p, u64 va, u64 frame) {
    u64 l0 = ensure_l0(p, va);
    pte_set(l0, (va >> 12) & 0x1FF, (frame & ~0xFFFul) | PTE_USER);
}

/* Resolve a mapped user VA to its physical frame address (for building the image /
 * stack contents while another address space, or bare mode, is active in CR0). */
static u64 user_pa(struct pcb *p, u64 va) {
    u64 pte = pte_get(va_l0(p, va), (va >> 12) & 0x1FF);
    return (pte & ~0xFFFul) + (va & 0xFFF);
}

static void as_write8(struct pcb *p, u64 va, u8 val)   { *(u8 *)user_pa(p, va) = val; }
static void as_write32(struct pcb *p, u64 va, u32 val) { *(u32 *)user_pa(p, va) = val; }
static void as_write64(struct pcb *p, u64 va, u64 val) { *(u64 *)user_pa(p, va) = val; }

/* maize-238: mirror-image reads of a (possibly other) process's memory. Assembled
 * byte-wise via user_pa so a multi-byte field that straddles a 4 KiB page boundary
 * (a pollfd array, a sockaddr_un on the stack) reads correctly across non-contiguous
 * frames. poll_recheck_all reads a PARKED process's fd set through these, translating
 * via that process's own page table rather than the live CR0. */
static u8  as_read8(struct pcb *p, u64 va)  { return *(u8 *)user_pa(p, va); }
static u16 as_read16(struct pcb *p, u64 va) {
    return (u16)((u16)as_read8(p, va) | ((u16)as_read8(p, va + 1) << 8));
}
static u32 as_read32(struct pcb *p, u64 va) {
    u32 v = 0; int i;
    for (i = 0; i < 4; ++i) { v |= (u32)as_read8(p, va + (u64)i) << (8 * i); }
    return v;
}
static u64 as_read64(struct pcb *p, u64 va) {
    u64 v = 0; int i;
    for (i = 0; i < 8; ++i) { v |= (u64)as_read8(p, va + (u64)i) << (8 * i); }
    return v;
}
static void as_write16(struct pcb *p, u64 va, u16 val) {
    as_write8(p, va, (u8)(val & 0xFF));
    as_write8(p, va + 1, (u8)((val >> 8) & 0xFF));
}

/* Ensure the page containing user VA `va` is mapped (allocate + map on first touch). */
static void ensure_user_page(struct pcb *p, u64 va) {
    u64 l0 = ensure_l0(p, va);
    u64 idx = (va >> 12) & 0x1FF;
    if ((pte_get(l0, idx) & PTE_V) == 0) {
        map_user_page(p, va, alloc_frame());
    }
}

/* ==================================================================================
 * Image load + first-entry stack.
 * ================================================================================== */
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

/* Marshalled argv/envp for the next process image. build_start_block reads these; they
 * are filled by marshal_single (argv0 = path only) for a worklist/fork exec and by
 * marshal_argv (the full argv/envp) for execve. */
#define QUESOS_MAX_ARG   32
#define QUESOS_ARGBUF    4096u
static char g_arg_strbuf[QUESOS_ARGBUF];  /* packed NUL-terminated arg + env strings   */
static u64  g_arg_off[QUESOS_MAX_ARG];    /* byte offset of each string in g_arg_strbuf */
static int  g_arg_argc;                   /* number of argv strings (offsets [0,argc))  */
static int  g_arg_envc;                   /* number of envp strings ([argc, argc+envc)) */
static u64  g_arg_pack;                   /* total bytes packed into g_arg_strbuf       */

/* Single-argument marshal: argv = { path }, empty envp. */
static void marshal_single(const char *path) {
    u64 len = qos_strlen(path) + 1;
    u64 k;
    if (len > QUESOS_ARGBUF) { len = QUESOS_ARGBUF; }
    for (k = 0; k < len; ++k) { g_arg_strbuf[k] = path[k]; }
    g_arg_off[0] = 0;
    g_arg_pack = len;
    g_arg_argc = 1;
    g_arg_envc = 0;
}

/* Map the user stack region and lay out the SysV process-start block + a synthesized
 * trap frame so quesos_switch_to enters at `entry` in user mode. Uses the marshalled
 * g_arg_* set. Layout on the user stack, low to high address:
 *   [R0..RB]  13 saved GP regs (all 0)   <- pcb->saved_rs points here
 *   [aux][cause]                          (2 words, ignored on entry)
 *   [rf][pc]                              (rf = USER_RF, pc = entry)
 *   [argc][argv..][NULL][envp..][NULL]    (the SysV start block crt0 consumes)
 *   [packed argv/envp strings ...]
 * After quesos_switch_to (POP x13; ADD $10; IRET) RS lands on [argc]. */
static void build_start_block(struct pcb *p, u64 entry) {
    u64 top = USER_STACK_TOP;
    u64 str_base = top - g_arg_pack;                          /* packed strings area    */
    int nptr = 1 + g_arg_argc + 1 + g_arg_envc + 1;          /* argc,argv,NULL,env,NULL */
    u64 argc_va = (str_base - (u64)nptr * 8ul) & ~15ul;
    u64 pc_va    = argc_va - 8;
    u64 rf_va    = argc_va - 16;
    u64 cause_va = argc_va - 24;
    u64 aux_va   = argc_va - 32;
    u64 regs_base = aux_va - 13ul * 8ul;
    u64 va, slot;
    u64 k;
    int i;

    for (va = top - (u64)USER_STACK_PAGES * PAGE_SIZE; va < top; va += PAGE_SIZE) {
        ensure_user_page(p, va);
    }

    /* Copy packed strings to the top of the user stack. */
    for (k = 0; k < g_arg_pack; ++k) { as_write8(p, str_base + k, (u8)g_arg_strbuf[k]); }

    /* argc, then argv[] pointers, NULL, envp[] pointers, NULL. */
    slot = argc_va;
    as_write64(p, slot, (u64)g_arg_argc); slot += 8;
    for (i = 0; i < g_arg_argc; ++i) { as_write64(p, slot, str_base + g_arg_off[i]); slot += 8; }
    as_write64(p, slot, 0); slot += 8;
    for (i = 0; i < g_arg_envc; ++i) { as_write64(p, slot, str_base + g_arg_off[g_arg_argc + i]); slot += 8; }
    as_write64(p, slot, 0); slot += 8;

    as_write64(p, pc_va,    entry);
    as_write64(p, rf_va,    USER_RF);
    as_write64(p, cause_va, 0);
    as_write64(p, aux_va,   0);
    for (k = 0; k < 13; ++k) { as_write64(p, regs_base + k * 8, 0); }

    p->saved_rs = regs_base;
}

/* Parse the .mzx and place each segment into freshly-allocated user frames mapped at
 * the segment vaddr, zero-filling the BSS tail. Writes the entry point to *entry_out.
 * Does NOT build the start block (the caller marshals argv/envp first). Returns 0. */
static int load_segments(struct pcb *p, const char *path, u64 *entry_out) {
    long size = quesos_slurp(path);
    const u8 *b = g_filebuf;
    u16 seg_count;
    u64 shoff;
    u64 top_va = 0;   /* maize-94: highest segment end, becomes the initial heap break */
    u16 i;

    if (size < (long)MZX_HEADER_SIZE
        || b[0] != 'M' || b[1] != 'Z' || b[2] != 'X' || b[3] != 0x01) {
        qos_puts("[quesos] not a loadable .mzx image: ");
        qos_puts(path);
        qos_puts("\n");
        return -1;
    }

    seg_count = rd_u16(b, 6);
    *entry_out = rd_u64(b, 8);
    shoff      = rd_u64(b, 16);

    for (i = 0; i < seg_count; ++i) {
        u64 so = shoff + (u64)i * MZX_SEGMENT_SIZE;
        u64 vaddr, file_off, mem_size, file_size, j, va;

        if (so + MZX_SEGMENT_SIZE > (u64)size) { return -1; }
        vaddr     = rd_u64(b, so + 8);
        file_off  = rd_u64(b, so + 16);
        mem_size  = rd_u64(b, so + 24);
        file_size = rd_u64(b, so + 32);
        if (file_off + file_size > (u64)size) { return -1; }

        /* Map every page the segment spans, then copy bytes through the frame's
         * identity address (CR0 is not necessarily this process's table yet). */
        for (va = vaddr & ~0xFFFul; va < vaddr + mem_size; va += PAGE_SIZE) {
            ensure_user_page(p, va);
        }
        for (j = 0; j < file_size; ++j) { as_write8(p, vaddr + j, b[file_off + j]); }
        for (j = file_size; j < mem_size; ++j) { as_write8(p, vaddr + j, 0); }
        if (vaddr + mem_size > top_va) { top_va = vaddr + mem_size; }
    }
    /* maize-94: the heap break starts page-aligned above the highest loaded segment and
     * grows upward via SYS_brk (do_brk maps pages on demand up to USER_BRK_MAX). */
    p->brk_cur = (top_va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return 0;
}

/* ==================================================================================
 * File descriptors, open-file descriptions, and pipes.
 *
 * Each process has a small fd table (pcb.fd) whose entries index a shared open-file-
 * description table (g_ofd). An OFD is a native host fd (fd 0/1/2 stdio, or a hostfs
 * open) or one end of a pipe; it is refcounted so fork (which copies the fd table) and
 * dup2 (which aliases a slot) share the same description. A pipe is a kernel ring
 * buffer; an empty-pipe read or full-pipe write PARKS the process (BLOCKED) and the
 * peer op (write / read / last-close) completes it and wakes it. All cross-process
 * buffer copies go through the target process's page table (as_write8 / user_pa), so
 * they are correct regardless of which address space CR0 currently names.
 * ================================================================================== */
#define QUESOS_MAX_OFD  128
/* maize-238: the shared ring pool now backs both plain pipes AND every socket connection
 * (each connected pair consumes 2 rings). Raised from 16 to 48 so a TinyX-shaped scenario
 * (a listener + a few accepted clients + a shell's own pipes) does not exhaust the pool.
 * A fixed compile-time capacity, not a contract (DIRT: cheap to raise later). */
#define QUESOS_MAX_PIPE 48
#define PIPE_CAP        4096u

#define OFD_FREE   0
#define OFD_NATIVE 1
#define OFD_PIPE_R 2
#define OFD_PIPE_W 3
#define OFD_RESVD  4    /* reserved by ofd_alloc; the caller fills in the real kind */
#define OFD_SOCK        5   /* maize-238: a connected AF_UNIX SOCK_STREAM end (full duplex) */
#define OFD_SOCK_LISTEN 6   /* maize-238: a bound+listening socket (namespace entry only)   */

struct ofd {
    int  kind;
    long native_fd;   /* OFD_NATIVE: the host fd. OFD_SOCK_LISTEN: the g_unix_bind[] index */
    int  pipe_idx;    /* OFD_PIPE_*: the ring. OFD_SOCK: the RECV ring (reads)             */
    int  peer_idx;    /* maize-238: OFD_SOCK only: the SEND ring (writes)                  */
    int  refcount;
};
static struct ofd g_ofd[QUESOS_MAX_OFD];

/* maize-238: AF_UNIX namespace. bind() records a path -> listening-socket entry; connect()
 * looks a path up here. Purely in-kernel: no hostfs file is created at the bound path
 * (there is no way to represent a listening-socket special file over the hostfs
 * passthrough, and no named consumer needs the path to appear in a directory listing;
 * only connect()-by-path must succeed). */
#define QUESOS_MAX_UNIX_BIND    8
#define QUESOS_MAX_UNIX_BACKLOG 8   /* also the pending-connect queue depth */

struct unix_bind {
    int  used;
    char path[QUESOS_PATH_CAP];
    int  listening;              /* set by listen(); bind() alone is not enough  */
    int  backlog;
    long pending_pid[QUESOS_MAX_UNIX_BACKLOG];   /* FIFO of blocked connectors    */
    int  pending_ofd[QUESOS_MAX_UNIX_BACKLOG];   /* the connector's socket ofd idx */
    int  pending_head, pending_tail, pending_count;
};
static struct unix_bind g_unix_bind[QUESOS_MAX_UNIX_BIND];

struct pipe_obj {
    int used;
    int readers;      /* open read ends                                            */
    int writers;      /* open write ends                                           */
    u32 r, w, count;  /* ring read/write cursors + fill level                      */
    u8  buf[PIPE_CAP];
};
static struct pipe_obj g_pipe[QUESOS_MAX_PIPE];

static int g_stdio_ofd[3];        /* shared native OFDs for fd 0/1/2                */

static int ofd_alloc(void) {
    int i;
    for (i = 0; i < QUESOS_MAX_OFD; ++i) {
        if (g_ofd[i].kind == OFD_FREE) {
            /* Reserve the slot NOW so a second alloc before the caller fills in the
             * real kind does not hand back the same slot. */
            g_ofd[i].kind = OFD_RESVD;
            g_ofd[i].refcount = 0;
            g_ofd[i].pipe_idx = -1;
            g_ofd[i].peer_idx = -1;
            return i;
        }
    }
    return -1;
}
static void ofd_ref(int idx) { if (idx >= 0) { g_ofd[idx].refcount++; } }

static void pipe_wake_readers(int pi);
static void pipe_wake_writers(int pi);
static void poll_recheck_all(void);   /* maize-238: re-evaluate parked poll()/select() */
static void wake_with(struct pcb *p, long rv);
static struct pcb *find_by_pid(long pid);

/* Drop a reference to an OFD; on the last reference close it (a hostfs fd is closed
 * natively; a pipe end decrements the pipe's reader/writer count and wakes any peer
 * blocked on the now-changed condition, e.g. a reader waiting for EOF). */
static void ofd_unref(int idx) {
    struct ofd *o;
    if (idx < 0) { return; }
    o = &g_ofd[idx];
    if (o->refcount > 0) { o->refcount--; }
    if (o->refcount > 0) { return; }
    if (o->kind == OFD_NATIVE) {
        if (o->native_fd > 2) { sys_close(o->native_fd); }
    } else if (o->kind == OFD_PIPE_R) {
        g_pipe[o->pipe_idx].readers--;
        if (g_pipe[o->pipe_idx].readers == 0) { pipe_wake_writers(o->pipe_idx); }
    } else if (o->kind == OFD_PIPE_W) {
        g_pipe[o->pipe_idx].writers--;
        if (g_pipe[o->pipe_idx].writers == 0) { pipe_wake_readers(o->pipe_idx); }
    } else if (o->kind == OFD_SOCK) {
        /* maize-238: one socket fd close tears down BOTH directions at once. The recv
         * ring (pipe_idx) had this end as its reader; the send ring (peer_idx) had it as
         * its writer. Do both pipe branches' effects so the peer sees EOF and -EPIPE. */
        if (o->pipe_idx >= 0) {
            g_pipe[o->pipe_idx].readers--;
            if (g_pipe[o->pipe_idx].readers == 0) { pipe_wake_writers(o->pipe_idx); }
        }
        if (o->peer_idx >= 0) {
            g_pipe[o->peer_idx].writers--;
            if (g_pipe[o->peer_idx].writers == 0) { pipe_wake_readers(o->peer_idx); }
        }
    } else if (o->kind == OFD_SOCK_LISTEN) {
        /* maize-238: closing a listening socket wakes every still-parked connector with
         * -ECONNREFUSED and frees the bound path for a fresh bind(). */
        int bi = (int)o->native_fd;
        if (bi >= 0 && bi < QUESOS_MAX_UNIX_BIND) {
            struct unix_bind *b = &g_unix_bind[bi];
            while (b->pending_count > 0) {
                long pid = b->pending_pid[b->pending_head];
                struct pcb *c = find_by_pid(pid);
                b->pending_head = (b->pending_head + 1) % QUESOS_MAX_UNIX_BACKLOG;
                b->pending_count--;
                if (c != 0 && c->state == P_BLOCKED && c->block_kind == BLK_CONNECT) {
                    wake_with(c, -(long)QOS_ECONNREFUSED);
                }
            }
            b->used = 0; b->listening = 0; b->path[0] = 0;
        }
    }
    o->kind = OFD_FREE;
    poll_recheck_all();   /* maize-238: a reader/writer count hit 0 (readiness changed) */
}

/* Allocate the shared stdio OFDs once at boot. */
static void ofd_init(void) {
    int i;
    for (i = 0; i < 3; ++i) {
        int o = ofd_alloc();
        g_ofd[o].kind = OFD_NATIVE;
        g_ofd[o].native_fd = i;
        g_stdio_ofd[i] = o;
    }
}

static void fdtable_init(struct pcb *p) {
    int i;
    for (i = 0; i < QUESOS_MAX_FD; ++i) { p->fd[i] = -1; }
    for (i = 0; i < 3; ++i) { p->fd[i] = g_stdio_ofd[i]; ofd_ref(g_stdio_ofd[i]); }
}
static void fdtable_copy(struct pcb *dst, struct pcb *src) {
    int i;
    for (i = 0; i < QUESOS_MAX_FD; ++i) { dst->fd[i] = src->fd[i]; ofd_ref(src->fd[i]); }
}
static void fdtable_close_all(struct pcb *p) {
    int i;
    for (i = 0; i < QUESOS_MAX_FD; ++i) {
        if (p->fd[i] >= 0) { ofd_unref(p->fd[i]); p->fd[i] = -1; }
    }
}
static int fd_alloc_slot(struct pcb *p) {
    int i;
    for (i = 0; i < QUESOS_MAX_FD; ++i) { if (p->fd[i] < 0) { return i; } }
    return -1;
}

/* Copy up to `count` bytes out of pipe `pi`'s ring into process `dst`'s buffer. */
static long pipe_fetch(int pi, struct pcb *dst, u64 uva, long count) {
    struct pipe_obj *q = &g_pipe[pi];
    long n = 0;
    while (n < count && q->count > 0) {
        as_write8(dst, uva + (u64)n, q->buf[q->r]);
        q->r = (q->r + 1) % PIPE_CAP;
        q->count--;
        ++n;
    }
    if (n > 0) { poll_recheck_all(); }   /* maize-238: ring drained (POLLOUT may open) */
    return n;
}
/* Copy up to `count` bytes from process `src`'s buffer into pipe `pi`'s ring. */
static long pipe_deposit(int pi, struct pcb *src, u64 uva, long count) {
    struct pipe_obj *q = &g_pipe[pi];
    long n = 0;
    while (n < count && q->count < PIPE_CAP) {
        q->buf[q->w] = *(u8 *)user_pa(src, uva + (u64)n);
        q->w = (q->w + 1) % PIPE_CAP;
        q->count++;
        ++n;
    }
    if (n > 0) { poll_recheck_all(); }   /* maize-238: ring filled (POLLIN may open) */
    return n;
}

static void wake_with(struct pcb *p, long rv) {
    as_write64(p, p->saved_rs + 11ul * 8ul, (u64)rv);
    p->block_kind = BLK_NONE;
    p->state = P_RUNNABLE;
}

/* A writer added data (or the last writer closed): complete blocked readers on `pi`. */
static void pipe_wake_readers(int pi) {
    int i;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        struct pcb *r = &g_proc[i];
        if (r->state == P_BLOCKED && r->block_kind == BLK_PIPE_R && r->block_pipe == pi) {
            long n = pipe_fetch(pi, r, r->block_buf, r->block_count);
            if (n > 0 || g_pipe[pi].writers == 0) { wake_with(r, n); }
        }
    }
}
/* A reader freed space (or the last reader closed): complete blocked writers on `pi`. */
static void pipe_wake_writers(int pi) {
    int i;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        struct pcb *w = &g_proc[i];
        if (w->state == P_BLOCKED && w->block_kind == BLK_PIPE_W && w->block_pipe == pi) {
            if (g_pipe[pi].readers == 0) { wake_with(w, -32); continue; }   /* -EPIPE */
            long n = pipe_deposit(pi, w, w->block_buf, w->block_count);
            if (n > 0) { pipe_wake_readers(pi); wake_with(w, n); }
        }
    }
}

/* Allocate a free process slot, or 0 if the table is full. */
static struct pcb *alloc_pcb(void) {
    int i;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        if (g_proc[i].state == P_FREE) { return &g_proc[i]; }
    }
    return 0;
}

/* Load `path` into a brand-new process; returns the pcb or 0 on failure. */
static struct pcb *spawn(const char *path, long parent) {
    struct pcb *p = alloc_pcb();
    unsigned long j;
    u64 entry;
    if (p == 0) { return 0; }
    p->state = P_RUNNABLE;
    p->pid = g_next_pid++;
    p->parent = parent;
    p->exit_status = 0;
    p->block_kind = BLK_NONE;
    p->fb_slot = -1;   /* maize-236: no framebuffer registration until SYS_fb_register */
    p->fb_mmap_va = 0; /* maize-238: no fb-mmap window until SYS_fb_mmap */
    p->pgid = p->pid;  /* maize-174: a top-level spawn starts its own process group */
    p->pending = 0; p->blocked = 0; p->term_signal = 0; p->in_handler = 0;
    p->termios_valid = 0;   /* maize-250: no console termios set until this proc tcsetattr's */
    { int _s; for (_s = 0; _s < 32; ++_s) { p->handler[_s] = 0; } }
    for (j = 0; j < QUESOS_PATH_CAP - 1 && path[j]; ++j) { p->path[j] = path[j]; }
    p->path[j] = 0;
    p->cwd[0] = '/'; p->cwd[1] = 0;   /* maize-94: a top-level process starts at root */
    fdtable_init(p);
    if (build_address_space(p) != 0 || load_segments(p, path, &entry) != 0) {
        fdtable_close_all(p);
        p->state = P_FREE;
        return 0;
    }
    marshal_single(path);
    build_start_block(p, entry);
    return p;
}

/* ==================================================================================
 * Scheduler: round-robin over the process table. schedule() picks the next RUNNABLE
 * process after g_current and enters it (noreturn); with no runnable process the VM
 * powers off. Cooperative today (processes yield by blocking on wait or exiting); the
 * instruction-tick timer adds preemption in a later phase. Single-tasking is the
 * degenerate case (one runnable process), which the maize-24 selfcheck exercises.
 * ================================================================================== */
static void schedule(void) {
    int base = (g_current != 0) ? (int)(g_current - g_proc) : -1;
    int i;
    for (i = 1; i <= QUESOS_MAX_PROC; ++i) {
        int idx = (base + i) % QUESOS_MAX_PROC;
        if (g_proc[idx].state == P_RUNNABLE) {
            g_current = &g_proc[idx];
            deliver_pending_signal(g_current);   /* maize-174: apply pending signals on resume */
            quesos_switch_to(g_current);   /* noreturn */
        }
    }
    /* Nothing runnable. If a process is parked on console input, spin (interrupts on) so
     * the console device keeps ticking, latching bytes, and raising IRQ 33, which wakes
     * the reader; the idle spin is kernel overhead, not a process slice, so it does not
     * violate "blocked processes consume no slices". Otherwise the worklist is drained
     * (or the survivors are deadlocked with no possible waker): power off cleanly. */
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        if (g_proc[i].state == P_BLOCKED
            && (g_proc[i].block_kind == BLK_CONSOLE || g_proc[i].block_kind == BLK_POLL)) {
            /* maize-238: a BLK_POLL caller is woken by the console IRQ (fd 0 readiness) or,
             * for a finite timeout, by the timer-tick deadline sweep, both of which keep
             * firing during the idle spin (interrupts on). Idle-spin rather than declaring a
             * deadlock so poll()/select() with a timeout returns 0 on schedule instead of
             * powering off the VM when nothing else is runnable. */
            g_current = 0;
            quesos_idle();   /* noreturn: SETINT; spin */
        }
    }
    quesos_poweroff();
}

/* Does `parent_pid` have any child (matching wpid) still alive or unreaped? */
static int has_child(long parent_pid, long wpid) {
    int i;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        struct pcb *c = &g_proc[i];
        if (c->state != P_FREE && c->parent == parent_pid
            && (wpid <= 0 || c->pid == wpid)) {
            return 1;
        }
    }
    return 0;
}

static struct pcb *find_by_pid(long pid) {
    int i;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        if (g_proc[i].state != P_FREE && g_proc[i].pid == pid) { return &g_proc[i]; }
    }
    return 0;
}

/* ==================================================================================
 * Syscall dispatcher. Entered from the trap trampoline (quesos_boot.mazm) on a cause-7
 * SYS trap, running supervisor on the kernel stack with the syscall-guest flag CLEAR.
 * The saved user register block lives at g_current->saved_rs: r[0..12] = R0..RB,
 * r[13] = aux (the syscall number), r[14] = cause, r[15] = rf, r[16] = pc. The result
 * is written back into the RV slot (r[11]) so the trampoline's restore delivers it.
 * ================================================================================== */

/* Copy `count` bytes from user VA `uva` into g_iobuf (bounded). Reads go through the
 * live translation (CR0 = the faulting process's table, supervisor), so a user VA
 * resolves correctly; the kernel-identity destination is what the native call reads. */
static long bounce_out(u64 uva, long count) {
    long n = count;
    long i;
    if (n < 0) { n = 0; }
    if (n > (long)QUESOS_IOBUF_CAP) { n = (long)QUESOS_IOBUF_CAP; }
    for (i = 0; i < n; ++i) { g_iobuf[i] = *(u8 *)(uva + (u64)i); }
    return n;
}

/* Native passthrough (fd 0/1/2 or a hostfs fd): bounce the user buffer through the
 * identity-mapped kernel staging area, then issue the native SYS. g_iobuf is only
 * QUESOS_IOBUF_CAP (4096) bytes, so a single write larger than that is delivered in a
 * loop of capped chunks (maize-250). A full-screen TUI frame (kilo's editorRefreshScreen
 * paints the whole screen in one write()) routinely exceeds 4096 bytes; without the loop
 * the tail was silently dropped mid-escape-sequence, garbling the paint. A genuine short
 * or negative return from the host stops the loop and is surfaced (POSIX-legal). */
static long native_write(long nfd, u64 uva, long count) {
    long remaining = count < 0 ? 0 : count;
    long total = 0;
    while (remaining > 0) {
        long chunk = remaining > (long)QUESOS_IOBUF_CAP ? (long)QUESOS_IOBUF_CAP : remaining;
        long n = bounce_out(uva + (u64)total, chunk);
        long w = sys_write(nfd, g_iobuf, n);
        if (w <= 0) { return total > 0 ? total : w; }   /* propagate error/EOF if nothing sent yet */
        total += w;
        remaining -= w;
        if (w < n) { break; }   /* genuine short write from the host: return the partial total */
    }
    return total;
}
static long native_read(long nfd, u64 uva, long count) {
    long n = count;
    long got;
    long i;
    if (n < 0) { n = 0; }
    if (n > (long)QUESOS_IOBUF_CAP) { n = (long)QUESOS_IOBUF_CAP; }
    got = sys_read(nfd, g_iobuf, n);
    for (i = 0; i < got; ++i) { *(u8 *)(uva + (u64)i) = g_iobuf[i]; }
    return got;
}

/* Resolve a process fd to its open-file description, or 0 for a bad fd. */
static struct ofd *fd_ofd(struct pcb *p, u64 fd_num) {
    int slot;
    if (fd_num >= QUESOS_MAX_FD) { return 0; }
    slot = p->fd[fd_num];
    if (slot < 0) { return 0; }
    return &g_ofd[slot];
}

static long do_write(u64 fd_num, u64 uva, long count) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    if (o == 0) { return -9; }   /* -EBADF */
    if (o->kind == OFD_NATIVE) { return native_write(o->native_fd, uva, count); }
    if (o->kind == OFD_PIPE_W || o->kind == OFD_SOCK) {
        /* maize-238: a socket writes into its SEND ring (peer_idx); the pipe-write path is
         * reused verbatim (BLK_PIPE_W park + reader-wake), only the ring index differs. */
        int pi = (o->kind == OFD_SOCK) ? o->peer_idx : o->pipe_idx;
        if (g_pipe[pi].readers == 0) { return -32; }   /* -EPIPE */
        if (g_pipe[pi].count < PIPE_CAP) {
            long n = pipe_deposit(pi, self, uva, count);
            pipe_wake_readers(pi);
            return n;
        }
        self->block_kind = BLK_PIPE_W; self->block_pipe = pi;
        self->block_buf = uva; self->block_count = count;
        self->state = P_BLOCKED;
        schedule();   /* noreturn: a reader completes this write */
        return 0;
    }
    return -9;
}

/* ==================================================================================
 * maize-174: guest signal subsystem -- console ISIG interception + foreground group.
 *
 * quesOS reads the console as an undifferentiated raw byte stream (it forwards no
 * termios to the host), so ISIG is realized here, in guest code, by raw byte VALUE:
 * a console read recognizes 0x03 (INTR) / 0x1C (QUIT) and raises SIGINT / SIGQUIT on
 * the foreground process group.
 *
 * maize-238 Branch A (decision 9285): the console device SIGNALS readiness (the IRQ,
 * vector 33) and console bytes are CONSUMED only at read time (console_read, or the
 * IRQ handing a byte directly to an already-parked reader) -- never pre-read into a
 * kernel ring ahead of a reader. So there is no input ring to buffer an ownerless
 * byte: fd-0 readiness is the device's own non-consuming status probe, and ISIG on a
 * data byte is recognized where the byte is consumed. This retires the phase-2 eager
 * pre-read model, whose stranded latch raced the oksh-to-child console handoff.
 * ================================================================================== */

/* The single controlling tty's foreground process group; set at boot to the first
 * worklist job's pgid, changed only by SYS_tcsetpgrp. */
static long g_fg_pgid;

/* maize-94 (OQ 8951): the console's line-discipline ISIG state, mirroring the one window
 * console's termios c_lflag & ISIG. Default 1 (canonical: 0x03/0x1C intercepted as
 * SIGINT/SIGQUIT, the maize-174 behavior). do_tcsetattr updates it from the forwarded
 * termios, so a raw-mode shell (oksh, ISIG cleared) receives 0x03/0x1C as literal data
 * bytes (decision 8947: Ctrl-C is a literal byte in wave 1). One console => one flag. */
static int g_tty_isig = 1;

/* Record a pending signal. Signals are always recorded regardless of the block mask;
 * `blocked` only gates DELIVERY (deliver_pending_signal), per POSIX. A signal raised on
 * a process blocked in a syscall is delivered when it next becomes runnable; in-flight
 * blocking syscalls are not interrupted (the SA_RESTART-implicit, no-EINTR model). */
static void raise_on_pcb(struct pcb *p, int sig) {
    if (p->state == P_RUNNABLE || p->state == P_BLOCKED) { p->pending |= (1ul << (sig - 1)); }
}
static int raise_on_pgid(long pgid, int sig) {
    int i, found = 0;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        struct pcb *p = &g_proc[i];
        if ((p->state == P_RUNNABLE || p->state == P_BLOCKED) && p->pgid == pgid) {
            raise_on_pcb(p, sig); found = 1;
        }
    }
    return found;
}
static void signal_fg_group(int sig) { raise_on_pgid(g_fg_pgid, sig); }

/* Console (fd 0) read via the device IRQ/status path, NEVER the native blocking read:
 * a native read(0) would park the whole CPU thread and freeze every process (doc 17).
 * Poll the console status port; if a byte is latched, take it; otherwise park the
 * PROCESS on the console IRQ, which delivers the byte and wakes it. */
/* console status bits (src/devices.cpp console_device::port_read). */
#define CON_STAT_INPUT 0x1ul   /* bit0: a data byte is available to read                 */
#define CON_STAT_EOF   0x4ul   /* bit2: host stdin is exhausted (real end-of-input)      */

static long console_read(struct pcb *self, u64 uva, long count) {
    u8 b;
    if (count <= 0) { return 0; }
    if (quesos_con_status() & CON_STAT_INPUT) {
        b = (u8)quesos_con_data();
        /* maize-94: the on-demand data-port read latches EOF when host stdin returns 0, so
         * re-check status AFTER the read: a real end-of-input returns 0 (EOF) from this fd0
         * read so a shell exits normally instead of looping on a synthesized NUL byte (this
         * is what makes a piped script WITHOUT an explicit `exit` terminate). */
        if (quesos_con_status() & CON_STAT_EOF) { return 0; }
        if (g_tty_isig && b == 0x03) { signal_fg_group(SIGINT); }        /* INTR: no data */
        else if (g_tty_isig && b == 0x1C) { signal_fg_group(SIGQUIT); }  /* QUIT: no data */
        else { as_write8(self, uva, b); return 1; }   /* raw mode delivers 0x03/0x1C as data */
        /* a control byte was consumed; fall through and park (the raised signal is
         * delivered the next time a targeted process is runnable). */
    }
    else if (quesos_con_status() & CON_STAT_EOF) {
        return 0;   /* the eager injector hit EOF with no byte pending: end-of-input */
    }
    self->block_kind = BLK_CONSOLE;
    self->block_buf = uva;
    self->block_count = count;
    self->state = P_BLOCKED;
    schedule();   /* noreturn: the console IRQ (vector 33) completes this read */
    return 0;
}

static long do_read(u64 fd_num, u64 uva, long count) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    if (o == 0) { return -9; }   /* -EBADF */
    if (o->kind == OFD_NATIVE) {
        if (o->native_fd == 0) { return console_read(self, uva, count); }
        return native_read(o->native_fd, uva, count);
    }
    if (o->kind == OFD_PIPE_R || o->kind == OFD_SOCK) {
        /* maize-238: a socket reads from its RECV ring (pipe_idx); the pipe-read path is
         * reused verbatim (BLK_PIPE_R park + writer-wake), only the ring index differs. */
        int pi = o->pipe_idx;
        if (g_pipe[pi].count > 0) {
            long n = pipe_fetch(pi, self, uva, count);
            pipe_wake_writers(pi);
            return n;
        }
        if (g_pipe[pi].writers == 0) { return 0; }   /* EOF: all writers closed */
        self->block_kind = BLK_PIPE_R; self->block_pipe = pi;
        self->block_buf = uva; self->block_count = count;
        self->state = P_BLOCKED;
        schedule();   /* noreturn: a writer completes this read */
        return 0;
    }
    return -9;
}

/* ==================================================================================
 * maize-94: per-process cwd + path resolution (decision 8940).
 *
 * Native hostfs has a fixed cwd of "/" (docs/design/hostfs.md), so a shell-local cd
 * string would not follow into a child's relative opens. Instead every quesOS process
 * carries pcb->cwd, and do_open / do_execve / the path-mutating forwarders join a
 * non-absolute incoming path onto pcb->cwd before handing an ABSOLUTE path to the
 * native stub. cwd defaults to "/", is inherited across fork, and survives execve.
 * ================================================================================== */

/* Copy a user-space NUL-terminated path into a kernel buffer (bounded, direct deref
 * through the live translation: CR0 = the calling process, supervisor). */
static void copy_user_path(u64 uva, char *dst) {
    int i;
    for (i = 0; i < QUESOS_PATH_CAP - 1 && *(char *)(uva + (u64)i); ++i) {
        dst[i] = *(char *)(uva + (u64)i);
    }
    dst[i] = 0;
}

/* Join a possibly-relative path onto base (an already-absolute cwd) into out. An
 * absolute `in` passes through unchanged; otherwise out = base + "/" + in (no double
 * slash when base is "/"). No "."/".." collapsing here: the native hostfs layer resolves
 * interior ".." within the mount (RESOLVE_BENEATH), so an unnormalized join is correct
 * for open/unlink/exec. cwd itself is kept normalized by do_chdir. */
static void join_path(const char *base, const char *in, char *out) {
    int n = 0, i;
    if (in[0] == '/') {
        for (i = 0; in[i] && n < QUESOS_PATH_CAP - 1; ++i) { out[n++] = in[i]; }
        out[n] = 0;
        return;
    }
    for (i = 0; base[i] && n < QUESOS_PATH_CAP - 1; ++i) { out[n++] = base[i]; }
    if (n == 0 || out[n - 1] != '/') { if (n < QUESOS_PATH_CAP - 1) { out[n++] = '/'; } }
    for (i = 0; in[i] && n < QUESOS_PATH_CAP - 1; ++i) { out[n++] = in[i]; }
    out[n] = 0;
}

/* Canonicalize an absolute path in place-free form: collapse "//", drop ".", and pop a
 * component on "..". Used by do_chdir so pcb->cwd (and getcwd's answer) stays clean even
 * after `cd ../x`. Single forward pass building an offset stack of component starts. */
static void normalize_path(const char *raw, char *out) {
    /* maize-94: one entry per kept component. Sized to the most components that can fit in
     * a QUESOS_PATH_CAP buffer (each is at least a '/' + one char = 2 bytes), so no path
     * that fits in `out` can overflow this or exceed it: the earlier QUESOS_MAX_ARG (32)
     * bound silently mistracked '..' for paths deeper than 32 components. */
    int comp_start[(QUESOS_PATH_CAP / 2) + 1];   /* out[] offset of each kept component's '/' */
    int max_comp = (int)(sizeof(comp_start) / sizeof(comp_start[0]));
    int ncomp = 0;
    int n = 0;                        /* length written to out so far                      */
    int i = 0;
    out[0] = 0;
    while (raw[i]) {
        int seg_len = 0;
        char seg[QUESOS_PATH_CAP];
        while (raw[i] == '/') { ++i; }                 /* skip separators */
        while (raw[i] && raw[i] != '/') {              /* gather one component */
            if (seg_len < QUESOS_PATH_CAP - 1) { seg[seg_len++] = raw[i]; }
            ++i;
        }
        if (seg_len == 0) { continue; }
        seg[seg_len] = 0;
        if (seg[0] == '.' && seg[1] == 0) { continue; }              /* "." */
        if (seg[0] == '.' && seg[1] == '.' && seg[2] == 0) {         /* ".." */
            if (ncomp > 0) { n = comp_start[--ncomp]; out[n] = 0; }
            continue;
        }
        if (ncomp < max_comp) { comp_start[ncomp++] = n; }
        if (n < QUESOS_PATH_CAP - 1) { out[n++] = '/'; }
        { int k; for (k = 0; k < seg_len && n < QUESOS_PATH_CAP - 1; ++k) { out[n++] = seg[k]; } }
        out[n] = 0;
    }
    if (n == 0) { out[0] = '/'; out[1] = 0; }          /* root, or all-".." above root */
}

/* open/close/dup/dup2/pipe: manage the fd table + open-file descriptions. */
static long do_open(u64 path_uva, long flags, long mode) {
    char in[QUESOS_PATH_CAP], kpath[QUESOS_PATH_CAP];
    int slot, o;
    long nfd;
    copy_user_path(path_uva, in);
    join_path(g_current->cwd, in, kpath);              /* resolve against the process cwd */
    nfd = sys_open(kpath, flags, mode);
    if (nfd < 0) { return nfd; }
    slot = fd_alloc_slot(g_current);
    o = ofd_alloc();
    if (slot < 0 || o < 0) { sys_close(nfd); return -24; }   /* -EMFILE */
    g_ofd[o].kind = OFD_NATIVE; g_ofd[o].native_fd = nfd; g_ofd[o].refcount = 1;
    g_current->fd[slot] = o;
    return slot;
}

/* maize-94 decision 8941: native hostfs file/dir forwarders. Each resolves a process fd
 * to its native fd (or a path against cwd) and bounces the fixed-size struct / dir-record
 * buffer through g_iobuf, mirroring do_write. A non-native (pipe) fd is rejected the way
 * Linux rejects the op on a pipe. */
static long do_fstat(u64 fd_num, u64 statbuf_uva) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    long r; int i;
    if (o == 0) { return -9; }                     /* -EBADF */
    if (o->kind != OFD_NATIVE) { return -9; }      /* fstat of a pipe end: -EBADF (no stat) */
    r = sys_fstat(o->native_fd, g_iobuf);
    if (r == 0) { for (i = 0; i < STAT_WIRE_SIZE; ++i) { as_write8(self, statbuf_uva + (u64)i, g_iobuf[i]); } }
    return r;
}

static long do_lseek(u64 fd_num, long offset, long whence) {
    struct ofd *o = fd_ofd(g_current, fd_num);
    if (o == 0) { return -9; }                     /* -EBADF */
    if (o->kind != OFD_NATIVE) { return -29; }     /* -ESPIPE: cannot seek a pipe */
    return sys_lseek(o->native_fd, offset, whence);
}

static long do_getdents64(u64 fd_num, u64 dirp_uva, long count) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    long n, i;
    if (o == 0) { return -9; }                     /* -EBADF */
    if (o->kind != OFD_NATIVE) { return -20; }     /* -ENOTDIR: a pipe is not a directory */
    if (count > (long)QUESOS_IOBUF_CAP) { count = (long)QUESOS_IOBUF_CAP; }
    n = sys_getdents64(o->native_fd, g_iobuf, count);
    if (n > 0) { for (i = 0; i < n; ++i) { as_write8(self, dirp_uva + (u64)i, g_iobuf[i]); } }
    return n;
}

static long do_unlink(u64 path_uva) {
    char in[QUESOS_PATH_CAP], kpath[QUESOS_PATH_CAP];
    copy_user_path(path_uva, in);
    join_path(g_current->cwd, in, kpath);
    return sys_unlink(kpath);
}

static long do_mkdir(u64 path_uva, long mode) {
    char in[QUESOS_PATH_CAP], kpath[QUESOS_PATH_CAP];
    copy_user_path(path_uva, in);
    join_path(g_current->cwd, in, kpath);
    return sys_mkdir(kpath, mode);
}

static long do_rename(u64 old_uva, u64 new_uva) {
    char oin[QUESOS_PATH_CAP], nin[QUESOS_PATH_CAP];
    char okpath[QUESOS_PATH_CAP], nkpath[QUESOS_PATH_CAP];
    copy_user_path(old_uva, oin);
    copy_user_path(new_uva, nin);
    join_path(g_current->cwd, oin, okpath);
    join_path(g_current->cwd, nin, nkpath);
    return sys_rename(okpath, nkpath);
}

static long do_ftruncate(u64 fd_num, long length) {
    struct ofd *o = fd_ofd(g_current, fd_num);
    if (o == 0) { return -9; }                     /* -EBADF */
    if (o->kind != OFD_NATIVE) { return -22; }     /* -EINVAL: not a regular file */
    return sys_ftruncate(o->native_fd, length);
}

/* maize-94 decision 8940: chdir/getcwd. chdir joins + normalizes against the current cwd,
 * validates the target is an existing directory (O_DIRECTORY open), and only then commits
 * pcb->cwd. getcwd copies pcb->cwd out (Linux semantics: returns strlen+1, -ERANGE when
 * the caller's buffer is too small). */
static long do_chdir(u64 path_uva) {
    struct pcb *self = g_current;
    char in[QUESOS_PATH_CAP], joined[QUESOS_PATH_CAP], norm[QUESOS_PATH_CAP];
    long fd; int i;
    copy_user_path(path_uva, in);
    join_path(self->cwd, in, joined);
    normalize_path(joined, norm);
    fd = sys_open(norm, QOS_O_DIRECTORY, 0);       /* validate existence + directory-ness */
    if (fd < 0) { return fd; }                     /* -ENOENT / -ENOTDIR from the backend */
    sys_close(fd);
    for (i = 0; i < QUESOS_PATH_CAP - 1 && norm[i]; ++i) { self->cwd[i] = norm[i]; }
    self->cwd[i] = 0;
    return 0;
}

static long do_getcwd(u64 buf_uva, long size) {
    struct pcb *self = g_current;
    long len = (long)qos_strlen(self->cwd);
    long i;
    if (size < len + 1) { return -34; }            /* -ERANGE */
    for (i = 0; i <= len; ++i) { as_write8(self, buf_uva + (u64)i, (u8)self->cwd[i]); }
    return len + 1;                                /* Linux getcwd: bytes incl. the NUL */
}

/* maize-94: heap break for a quesOS process. The native SYS_brk manages the VM's flat
 * memory break, which is meaningless under per-process Sv48 paging, so quesOS implements
 * brk against the process's own page table: a grow maps fresh (zeroed) user pages on
 * demand up to USER_BRK_MAX (just below the stack); a query (new==0) returns the current
 * break. Like the native brk, this is exempt from the errno convention (it always returns
 * a break address, never -errno): the sbrk wrapper detects failure by comparing the
 * returned break to the requested one, so a refused grow returns the UNCHANGED break. */
static long do_brk(u64 newbrk) {
    struct pcb *self = g_current;
    u64 cur = self->brk_cur;
    u64 va;
    if (newbrk == 0) { return (long)cur; }         /* query */
    if (newbrk > USER_BRK_MAX) { return (long)cur; }   /* refuse: caller sees no change */
    if (newbrk > cur) {
        for (va = cur & ~(PAGE_SIZE - 1); va < newbrk; va += PAGE_SIZE) {
            ensure_user_page(self, va);            /* map + zero-fill each new heap page */
        }
    }
    self->brk_cur = newbrk;
    return (long)newbrk;
}

/* maize-94 (OQ 8951 ruling): termios forwarders. A quesOS process's SYS $F1/$F2 bounces
 * the 36-byte termios wire image through g_iobuf to/from the native window console. A
 * non-native (pipe) fd is -ENOTTY. do_tcsetattr also mirrors the ISIG lflag into
 * g_tty_isig so the console signal interception tracks the shell's raw-mode setting. */
/* Decode c_lflag (the 4th 32-bit LE word, byte offset TERMIOS_OFF_LFLAG) out of a 36-byte
 * termios wire image. Shared by do_tcsetattr and restore_console_on_death (maize-250). */
static u32 termios_lflag(const unsigned char *img) {
    return (u32)img[TERMIOS_OFF_LFLAG]
         | ((u32)img[TERMIOS_OFF_LFLAG + 1] << 8)
         | ((u32)img[TERMIOS_OFF_LFLAG + 2] << 16)
         | ((u32)img[TERMIOS_OFF_LFLAG + 3] << 24);
}

static long do_tcgetattr(u64 fd_num, u64 t_uva) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    long r; int i;
    if (o == 0) { return -9; }                     /* -EBADF */
    if (o->kind != OFD_NATIVE) { return -25; }     /* -ENOTTY: a pipe is not a terminal */
    r = sys_tcgetattr(o->native_fd, g_iobuf);
    if (r == 0) { for (i = 0; i < TERMIOS_WIRE_SIZE; ++i) { as_write8(self, t_uva + (u64)i, g_iobuf[i]); } }
    return r;
}

static long do_tcsetattr(u64 fd_num, long optional_actions, u64 t_uva) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    long r; int i; u32 lflag;
    if (o == 0) { return -9; }                     /* -EBADF */
    if (o->kind != OFD_NATIVE) { return -25; }     /* -ENOTTY */
    for (i = 0; i < TERMIOS_WIRE_SIZE; ++i) { g_iobuf[i] = *(u8 *)(t_uva + (u64)i); }
    r = sys_tcsetattr(o->native_fd, optional_actions, g_iobuf);
    if (r == 0) {
        /* c_lflag is the 4th 32-bit LE word (byte offset 12). Track ISIG so console_read /
         * quesos_console_irq stop intercepting 0x03/0x1C once the shell clears it. */
        lflag = termios_lflag((const unsigned char *)g_iobuf);
        g_tty_isig = (lflag & TIO_ISIG) ? 1 : 0;
        /* maize-250: remember this process's own last-set termios image so reap_tail can
         * re-apply the parent's console state if this process dies before its own
         * userspace cleanup (kilo's atexit -> disableRawMode) runs. */
        for (i = 0; i < TERMIOS_WIRE_SIZE; ++i) { self->termios_img[i] = (unsigned char)g_iobuf[i]; }
        self->termios_valid = 1;
    }
    return r;
}

/* maize-94: sys_ttysize forwarder ($F6). Mirrors do_tcgetattr: map the guest fd to its
 * native fd, ask the native provider for the terminal size, and bounce the 8-byte struct
 * winsize back through g_iobuf. A non-native (pipe) fd is -ENOTTY, and the native provider
 * itself returns -ENOTTY on a windowed console or non-tty host stdio, so the caller (oksh)
 * degrades to its default size on either. This is what an interactive oksh needs to reach
 * its prompt on the DEFAULT input path (a real console) rather than stranding. */
static long do_ttysize(u64 fd_num, u64 ws_uva) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    long r; int i;
    if (o == 0) { return -9; }                     /* -EBADF */
    if (o->kind != OFD_NATIVE) { return -25; }     /* -ENOTTY: a pipe is not a terminal */
    r = sys_ttysize(o->native_fd, g_iobuf);
    if (r == 0) { for (i = 0; i < WINSIZE_WIRE_SIZE; ++i) { as_write8(self, ws_uva + (u64)i, g_iobuf[i]); } }
    return r;
}

static long do_close(u64 fd_num) {
    struct pcb *self = g_current;
    if (fd_num >= QUESOS_MAX_FD || self->fd[fd_num] < 0) { return -9; }
    ofd_unref(self->fd[fd_num]);
    self->fd[fd_num] = -1;
    return 0;
}

static long do_dup2(u64 oldfd, u64 newfd) {
    struct pcb *self = g_current;
    if (oldfd >= QUESOS_MAX_FD || newfd >= QUESOS_MAX_FD || self->fd[oldfd] < 0) { return -9; }
    if (oldfd == newfd) { return (long)newfd; }
    if (self->fd[newfd] >= 0) { ofd_unref(self->fd[newfd]); }
    self->fd[newfd] = self->fd[oldfd];
    ofd_ref(self->fd[newfd]);
    return (long)newfd;
}

static long do_dup(u64 oldfd) {
    struct pcb *self = g_current;
    int slot;
    if (oldfd >= QUESOS_MAX_FD || self->fd[oldfd] < 0) { return -9; }
    slot = fd_alloc_slot(self);
    if (slot < 0) { return -24; }
    self->fd[slot] = self->fd[oldfd];
    ofd_ref(self->fd[slot]);
    return slot;
}

static long do_pipe(u64 fds_uva) {
    struct pcb *self = g_current;
    int pi, i, rslot, wslot, ro, wo;

    pi = -1;
    for (i = 0; i < QUESOS_MAX_PIPE; ++i) { if (!g_pipe[i].used) { pi = i; break; } }
    if (pi < 0) { return -24; }
    ro = ofd_alloc();
    wo = ofd_alloc();
    if (ro < 0 || wo < 0) { return -24; }
    rslot = fd_alloc_slot(self);
    if (rslot < 0) { return -24; }
    self->fd[rslot] = ro;                 /* claim so the write slot differs */
    wslot = fd_alloc_slot(self);
    if (wslot < 0) { self->fd[rslot] = -1; return -24; }
    self->fd[wslot] = wo;

    g_pipe[pi].used = 1; g_pipe[pi].readers = 1; g_pipe[pi].writers = 1;
    g_pipe[pi].r = 0; g_pipe[pi].w = 0; g_pipe[pi].count = 0;
    g_ofd[ro].kind = OFD_PIPE_R; g_ofd[ro].pipe_idx = pi; g_ofd[ro].refcount = 1;
    g_ofd[wo].kind = OFD_PIPE_W; g_ofd[wo].pipe_idx = pi; g_ofd[wo].refcount = 1;

    as_write32(self, fds_uva,     (u32)rslot);
    as_write32(self, fds_uva + 4, (u32)wslot);
    return 0;
}

/* ==================================================================================
 * maize-238 Family A: AF_UNIX SOCK_STREAM sockets over the existing pipe ring pool.
 *
 * A connected socket pair is two cross-wired rings (one per direction = full duplex);
 * a socket's read()/write() reuse pipe_fetch/pipe_deposit against the recv/send ring,
 * so no new data-I/O path exists (only a new ring-index selection at each dispatch
 * site, above). bind()/listen()/accept()/connect() add an in-kernel namespace and a
 * park/handshake, reusing wake_with exactly as the pipe and wait paths do.
 * ================================================================================== */

/* Allocate a fresh ring from the shared pool (readers=writers=1, empty). -1 if full. */
static int alloc_ring(void) {
    int i;
    for (i = 0; i < QUESOS_MAX_PIPE; ++i) {
        if (!g_pipe[i].used) {
            g_pipe[i].used = 1; g_pipe[i].readers = 1; g_pipe[i].writers = 1;
            g_pipe[i].r = 0; g_pipe[i].w = 0; g_pipe[i].count = 0;
            return i;
        }
    }
    return -1;
}

static int unix_bind_lookup(const char *path) {
    int i, j;
    for (i = 0; i < QUESOS_MAX_UNIX_BIND; ++i) {
        if (!g_unix_bind[i].used) { continue; }
        for (j = 0; g_unix_bind[i].path[j] == path[j]; ++j) {
            if (path[j] == 0) { return i; }
        }
    }
    return -1;
}

/* Copy a sockaddr_un's sun_path (bytes after the 2-byte sun_family) out of process p's
 * address space into a kernel buffer, NUL-terminated. */
static void read_sun_path(struct pcb *p, u64 addr_uva, char *dst) {
    int i;
    for (i = 0; i < QUESOS_PATH_CAP - 1; ++i) {
        char c = (char)as_read8(p, addr_uva + 2u + (u64)i);
        dst[i] = c;
        if (c == 0) { return; }
    }
    dst[QUESOS_PATH_CAP - 1] = 0;
}

/* accept()'s peer address for a unix socket has no meaningful value; synthesize the
 * unnamed {sun_family=AF_UNIX, sun_path=""} Linux itself often returns for this side. */
static void write_empty_sockaddr(struct pcb *p, u64 addr_uva, u64 addrlen_uva) {
    as_write16(p, addr_uva, (u16)AF_UNIX);
    if (addrlen_uva != 0) { as_write32(p, addrlen_uva, 2u); }   /* sizeof(sa_family_t) */
}

/* Complete the connect/accept handshake: allocate the two cross-wired rings, wire the
 * connector's own ofd (cofd) in place, and create the accepter's new fd. Returns the
 * accepter's new fd slot, or -1 if a ring / fd / ofd could not be allocated. Does NOT
 * wake either party (the caller does, so the accept and connect legs stay symmetric). */
static long socket_handshake(struct pcb *accepter, int cofd) {
    int x, y, aslot, aofd;
    x = alloc_ring();
    if (x < 0) { return -1; }
    y = alloc_ring();
    if (y < 0) { g_pipe[x].used = 0; return -1; }
    aslot = fd_alloc_slot(accepter);
    if (aslot < 0) { g_pipe[x].used = 0; g_pipe[y].used = 0; return -1; }
    aofd = ofd_alloc();
    if (aofd < 0) { g_pipe[x].used = 0; g_pipe[y].used = 0; return -1; }
    /* Ring X: connector writes, accepter reads. Ring Y: accepter writes, connector reads. */
    g_ofd[cofd].pipe_idx = y; g_ofd[cofd].peer_idx = x;   /* connector recv=Y send=X */
    g_ofd[aofd].kind = OFD_SOCK; g_ofd[aofd].pipe_idx = x; g_ofd[aofd].peer_idx = y;
    g_ofd[aofd].refcount = 1;
    accepter->fd[aslot] = aofd;
    return aslot;
}

static long do_socket(long domain, long type, long protocol) {
    struct pcb *self = g_current;
    int slot, o;
    if (domain != AF_UNIX)   { return -(long)QOS_EAFNOSUPPORT; }
    if (type != SOCK_STREAM) { return -(long)QOS_EINVAL; }   /* reject SOCK_NONBLOCK/CLOEXEC honestly */
    if (protocol != 0)       { return -(long)QOS_EPROTONOSUPPORT; }
    slot = fd_alloc_slot(self);
    if (slot < 0) { return -24; }   /* -EMFILE */
    o = ofd_alloc();
    if (o < 0) { return -24; }
    g_ofd[o].kind = OFD_SOCK; g_ofd[o].pipe_idx = -1; g_ofd[o].peer_idx = -1; g_ofd[o].refcount = 1;
    self->fd[slot] = o;
    return slot;
}

static long do_bind(u64 fd_num, u64 addr_uva, u64 addrlen) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    char path[QUESOS_PATH_CAP];
    int bi, k;
    (void)addrlen;
    if (o == 0 || o->kind != OFD_SOCK || o->pipe_idx >= 0) { return -(long)QOS_EINVAL; }
    if (as_read16(self, addr_uva) != (u16)AF_UNIX) { return -(long)QOS_EINVAL; }
    read_sun_path(self, addr_uva, path);
    if (unix_bind_lookup(path) >= 0) { return -(long)QOS_EADDRINUSE; }
    bi = -1;
    for (k = 0; k < QUESOS_MAX_UNIX_BIND; ++k) { if (!g_unix_bind[k].used) { bi = k; break; } }
    if (bi < 0) { return -(long)QOS_ENOSPC; }
    {
        struct unix_bind *b = &g_unix_bind[bi];
        b->used = 1; b->listening = 0; b->backlog = QUESOS_MAX_UNIX_BACKLOG;
        b->pending_head = 0; b->pending_tail = 0; b->pending_count = 0;
        for (k = 0; k < QUESOS_PATH_CAP - 1 && path[k]; ++k) { b->path[k] = path[k]; }
        b->path[k] = 0;
    }
    o->kind = OFD_SOCK_LISTEN;   /* bind() converts the ofd to a namespace entry */
    o->native_fd = bi;
    return 0;
}

static long do_listen(u64 fd_num, long backlog) {
    struct ofd *o = fd_ofd(g_current, fd_num);
    struct unix_bind *b;
    if (o == 0 || o->kind != OFD_SOCK_LISTEN) { return -(long)QOS_EINVAL; }
    b = &g_unix_bind[(int)o->native_fd];
    if (backlog < 1) { backlog = 1; }
    if (backlog > QUESOS_MAX_UNIX_BACKLOG) { backlog = QUESOS_MAX_UNIX_BACKLOG; }
    b->backlog = (int)backlog;
    b->listening = 1;
    return 0;   /* Linux silently clamps an over-large backlog, always succeeds */
}

static long do_connect(u64 fd_num, u64 addr_uva, u64 addrlen) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    char path[QUESOS_PATH_CAP];
    struct unix_bind *b;
    int bi, cofd, j;
    (void)addrlen;
    if (o == 0 || o->kind != OFD_SOCK || o->pipe_idx >= 0) { return -(long)QOS_EINVAL; }
    read_sun_path(self, addr_uva, path);
    bi = unix_bind_lookup(path);
    if (bi < 0) { return -(long)QOS_ECONNREFUSED; }
    b = &g_unix_bind[bi];
    if (!b->listening) { return -(long)QOS_ECONNREFUSED; }
    cofd = self->fd[fd_num];
    /* If an accepter is already parked on this listener, complete the handshake now and
     * wake it with its new fd; the connector (self) returns 0 without parking. */
    for (j = 0; j < QUESOS_MAX_PROC; ++j) {
        struct pcb *a = &g_proc[j];
        if (a->state == P_BLOCKED && a->block_kind == BLK_ACCEPT && a->block_pipe == bi) {
            long afd = socket_handshake(a, cofd);
            if (afd < 0) { return -(long)QOS_ECONNREFUSED; }
            if (a->block_buf != 0) { write_empty_sockaddr(a, a->block_buf, a->block_count); }
            wake_with(a, afd);
            return 0;
        }
    }
    if (b->pending_count >= b->backlog) { return -(long)QOS_ECONNREFUSED; }
    /* Enqueue and park; a later accept() completes the handshake and wakes us with 0. */
    b->pending_pid[b->pending_tail] = self->pid;
    b->pending_ofd[b->pending_tail] = cofd;
    b->pending_tail = (b->pending_tail + 1) % QUESOS_MAX_UNIX_BACKLOG;
    b->pending_count++;
    self->block_kind = BLK_CONNECT; self->block_pipe = bi;
    self->state = P_BLOCKED;
    poll_recheck_all();   /* the listener just became readable (a connection is waiting) */
    schedule();           /* noreturn: accept() completes this connect */
    return 0;
}

static long do_accept(u64 fd_num, u64 addr_uva, u64 addrlen_uva) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    struct unix_bind *b;
    int bi;
    if (o == 0 || o->kind != OFD_SOCK_LISTEN) { return -(long)QOS_EINVAL; }
    bi = (int)o->native_fd;
    b = &g_unix_bind[bi];
    if (b->pending_count > 0) {
        long pid = b->pending_pid[b->pending_head];
        int cofd = b->pending_ofd[b->pending_head];
        struct pcb *conn;
        long afd;
        b->pending_head = (b->pending_head + 1) % QUESOS_MAX_UNIX_BACKLOG;
        b->pending_count--;
        conn = find_by_pid(pid);
        if (conn == 0 || conn->state != P_BLOCKED || conn->block_kind != BLK_CONNECT) {
            return -(long)QOS_ECONNREFUSED;   /* connector vanished */
        }
        afd = socket_handshake(self, cofd);
        if (afd < 0) { wake_with(conn, -(long)QOS_ECONNREFUSED); return -24; }
        if (addr_uva != 0) { write_empty_sockaddr(self, addr_uva, addrlen_uva); }
        wake_with(conn, 0);
        return afd;
    }
    /* Nothing pending: park until a connect() enqueues and completes us. Stash the
     * caller's out-address so the completing connect can fill it. */
    self->block_kind = BLK_ACCEPT; self->block_pipe = bi;
    self->block_buf = addr_uva; self->block_count = (long)addrlen_uva;
    self->state = P_BLOCKED;
    schedule();   /* noreturn: connect() completes this accept */
    return 0;
}

static long do_socketpair(long domain, long type, long protocol, u64 sv_uva) {
    struct pcb *self = g_current;
    int x, y, s0, s1, o0, o1;
    if (domain != AF_UNIX)   { return -(long)QOS_EAFNOSUPPORT; }
    if (type != SOCK_STREAM) { return -(long)QOS_EINVAL; }
    if (protocol != 0)       { return -(long)QOS_EPROTONOSUPPORT; }
    x = alloc_ring(); if (x < 0) { return -24; }
    y = alloc_ring(); if (y < 0) { g_pipe[x].used = 0; return -24; }
    s0 = fd_alloc_slot(self);
    o0 = ofd_alloc();
    if (s0 < 0 || o0 < 0) { g_pipe[x].used = 0; g_pipe[y].used = 0; if (o0 >= 0) { g_ofd[o0].kind = OFD_FREE; } return -24; }
    self->fd[s0] = o0;                 /* claim so the second slot differs */
    s1 = fd_alloc_slot(self);
    o1 = ofd_alloc();
    if (s1 < 0 || o1 < 0) {
        g_pipe[x].used = 0; g_pipe[y].used = 0;
        self->fd[s0] = -1; g_ofd[o0].kind = OFD_FREE;
        if (o1 >= 0) { g_ofd[o1].kind = OFD_FREE; }
        return -24;
    }
    self->fd[s1] = o1;
    /* end0 recv=X send=Y; end1 recv=Y send=X (cross-wired full duplex). */
    g_ofd[o0].kind = OFD_SOCK; g_ofd[o0].pipe_idx = x; g_ofd[o0].peer_idx = y; g_ofd[o0].refcount = 1;
    g_ofd[o1].kind = OFD_SOCK; g_ofd[o1].pipe_idx = y; g_ofd[o1].peer_idx = x; g_ofd[o1].refcount = 1;
    as_write32(self, sv_uva,      (u32)s0);
    as_write32(self, sv_uva + 4u, (u32)s1);
    return 0;
}

/* ==================================================================================
 * maize-238 Family B: select()/poll() readiness multiplexing.
 *
 * A non-blocking readiness check (fd_ready) is evaluated first on every call; if nothing
 * is ready the caller parks (BLK_POLL) and a shared O(QUESOS_MAX_PROC) recheck sweep
 * (poll_recheck_all), fired from every event that can change readiness (pipe fill-level,
 * a reader/writer count hitting 0, a byte arriving on the console, a listener gaining a
 * pending connection), re-evaluates each parked caller's fd set and wakes any now-ready
 * one via wake_with. The timer tick sweeps parked callers past their timeout deadline.
 * ================================================================================== */
#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

/* Readiness of one fd against the requested events (POLLERR/POLLNVAL always reported).
 * fd 0 (console) is readable when a DATA byte is pending on host stdin (the device's
 * non-consuming status probe, the IRQ/readiness model -- both invocation modes under
 * ratified Branch A); a hostfs/stdout fd is always ready
 * (Linux regular-file semantics); a pipe/socket read side is readable with data or at EOF
 * (writers==0); a pipe/socket write side is writable with room, but reports POLLERR (not
 * POLLOUT) once the peer read end is gone, since a write would then fail -EPIPE; a
 * listening socket is readable when a connection is waiting for accept(). */
static short fd_ready(struct pcb *p, int fd, short events) {
    short r = 0;
    struct ofd *o;
    if (fd < 0 || fd >= QUESOS_MAX_FD || p->fd[fd] < 0) { return POLLNVAL; }
    o = &g_ofd[p->fd[fd]];
    if (o->kind == OFD_NATIVE) {
        if (o->native_fd == 0) {
            /* maize-238 Branch A (decision 9285): readable when a DATA byte is pending
             * (CON_STAT_INPUT from the non-consuming probe), NOT merely at end-of-input --
             * console EOF surfaces through a read returning 0, not as poll-readable, so a
             * drained-then-EOF console does not spuriously wake a poll/select waiter. */
            if ((events & POLLIN) && (quesos_con_status() & CON_STAT_INPUT)) { r |= POLLIN; }
        } else {
            if (events & POLLIN)  { r |= POLLIN; }
            if (events & POLLOUT) { r |= POLLOUT; }
        }
    } else if (o->kind == OFD_PIPE_R) {
        int pi = o->pipe_idx;
        if ((events & POLLIN) && (g_pipe[pi].count > 0 || g_pipe[pi].writers == 0)) { r |= POLLIN; }
    } else if (o->kind == OFD_PIPE_W) {
        int pi = o->pipe_idx;
        if (g_pipe[pi].readers == 0) { r |= POLLERR; }
        else if ((events & POLLOUT) && g_pipe[pi].count < PIPE_CAP) { r |= POLLOUT; }
    } else if (o->kind == OFD_SOCK) {
        int rr = o->pipe_idx, ww = o->peer_idx;
        if ((events & POLLIN) && (g_pipe[rr].count > 0 || g_pipe[rr].writers == 0)) { r |= POLLIN; }
        if (g_pipe[ww].readers == 0) { r |= POLLERR; }
        else if ((events & POLLOUT) && g_pipe[ww].count < PIPE_CAP) { r |= POLLOUT; }
    } else if (o->kind == OFD_SOCK_LISTEN) {
        if ((events & POLLIN) && g_unix_bind[(int)o->native_fd].pending_count > 0) { r |= POLLIN; }
    }
    return r;
}

/* Evaluate process p's parked (or freshly requested) fd set. commit != 0 writes the
 * revents / result fd_sets back into p's own address space. Returns the ready count
 * (poll: pollfds with nonzero revents; select: total ready read + write bits). */
static long poll_evaluate(struct pcb *p, int commit) {
    long count = 0;
    if (p->poll_mode == 0) {
        u64 base = (u64)p->poll_fds_uva;
        long i;
        for (i = 0; i < p->poll_nfds; ++i) {
            u64 e = base + (u64)i * 8ul;
            int fd = (int)as_read32(p, e);
            short events = (short)as_read16(p, e + 4u);
            short rev = fd_ready(p, fd, events);
            if (commit) { as_write16(p, e + 6u, (u16)rev); }
            if (rev != 0) { ++count; }
        }
    } else {
        u64 rmask = (p->poll_r_uva != 0) ? as_read64(p, p->poll_r_uva) : 0;
        u64 wmask = (p->poll_w_uva != 0) ? as_read64(p, p->poll_w_uva) : 0;
        u64 rword = 0, wword = 0;
        int nf = (int)p->poll_nfds;
        int fd;
        if (nf > QUESOS_MAX_FD) { nf = QUESOS_MAX_FD; }
        for (fd = 0; fd < nf; ++fd) {
            int in_r = (int)((rmask >> fd) & 1ul);
            int in_w = (int)((wmask >> fd) & 1ul);
            short events, rev;
            if (!in_r && !in_w) { continue; }
            events = (short)((in_r ? POLLIN : 0) | (in_w ? POLLOUT : 0));
            rev = fd_ready(p, fd, events);
            if (in_r && (rev & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) { rword |= (1ul << fd); ++count; }
            if (in_w && (rev & (POLLOUT | POLLERR | POLLNVAL)))          { wword |= (1ul << fd); ++count; }
        }
        if (commit) {
            if (p->poll_r_uva != 0) { as_write64(p, p->poll_r_uva, rword); }
            if (p->poll_w_uva != 0) { as_write64(p, p->poll_w_uva, wword); }
            if (p->poll_e_uva != 0) { as_write64(p, p->poll_e_uva, 0); }   /* no OOB model */
        }
    }
    return count;
}

/* Re-evaluate every parked poll()/select() caller; wake any whose condition is now met.
 * Called at the tail of every readiness-changing event (see pipe_fetch/pipe_deposit,
 * ofd_unref, quesos_console_irq, connect()'s enqueue). */
static void poll_recheck_all(void) {
    int i;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        struct pcb *p = &g_proc[i];
        if (p->state == P_BLOCKED && p->block_kind == BLK_POLL) {
            if (poll_evaluate(p, 0) > 0) {
                long ready = poll_evaluate(p, 1);
                wake_with(p, ready);
            }
        }
    }
}

static long do_poll(u64 fds_uva, u64 nfds, long timeout_ms) {
    struct pcb *self = g_current;
    long ready;
    self->poll_mode = 0;
    self->poll_fds_uva = (long)fds_uva;
    self->poll_nfds = (long)nfds;
    ready = poll_evaluate(self, 1);
    if (ready > 0) { return ready; }
    if (timeout_ms == 0) { return 0; }   /* pure non-blocking: revents committed (all 0) */
    self->poll_deadline_ms = (timeout_ms < 0) ? 0 : (sys_clock_ms() + (u64)timeout_ms);
    self->block_kind = BLK_POLL;
    self->state = P_BLOCKED;
    schedule();   /* noreturn: poll_recheck_all or the timer timeout sweep completes this */
    return 0;
}

static long do_select(long nfds, u64 r_uva, u64 w_uva, u64 e_uva, u64 tv_uva) {
    struct pcb *self = g_current;
    long ready;
    self->poll_mode = 1;
    self->poll_nfds = nfds;
    self->poll_r_uva = r_uva;
    self->poll_w_uva = w_uva;
    self->poll_e_uva = e_uva;
    ready = poll_evaluate(self, 1);
    if (ready > 0) { return ready; }
    if (tv_uva != 0) {
        u64 sec  = as_read64(self, tv_uva);
        u64 usec = as_read64(self, tv_uva + 8u);
        if (sec == 0 && usec == 0) { return 0; }   /* {0,0}: non-blocking */
        self->poll_deadline_ms = sys_clock_ms() + sec * 1000ul + usec / 1000ul;
    } else {
        self->poll_deadline_ms = 0;   /* NULL timeval: block forever */
    }
    self->block_kind = BLK_POLL;
    self->state = P_BLOCKED;
    schedule();   /* noreturn */
    return 0;
}

/* Encode a normal-exit wait status the way Linux does: exit code in bits 8..15
 * (WIFEXITED true, WEXITSTATUS = (status >> 8) & 0xFF). */
/* maize-174: WIFSIGNALED (low 7 bits = terminating signal) when term_signal is set;
 * else WIFEXITED (exit code in bits 8..15). This discharges maize-94's stubbed-false
 * WIFSIGNALED deviation for real. */
static u32 wait_status(struct pcb *c) {
    if (c->term_signal != 0) { return (u32)(c->term_signal & 0x7F); }
    return (u32)((c->exit_status & 0xFF) << 8);
}

/* fork (eager copy, ratified). Allocate a child, build it a fresh address space, and
 * EAGER-COPY every mapped user page into distinct child frames (the separation that
 * makes the two address spaces independent). The child resumes at the same saved
 * context (the copied stack holds an identical frame) with the fork result forced to 0;
 * the parent's fork returns the child pid. */
static long do_fork(void) {
    struct pcb *parent = g_current;
    struct pcb *child = alloc_pcb();
    u64 idx;
    unsigned long k;

    if (child == 0) { return -11; }   /* -EAGAIN */
    child->state = P_RUNNABLE;
    child->pid = g_next_pid++;
    child->parent = parent->pid;
    child->exit_status = 0;
    child->wait_for = 0;
    child->wait_status_uva = 0;
    child->block_kind = BLK_NONE;
    /* maize-236 Decision D4: fork does NOT propagate the framebuffer registration. The
     * registered base is a physical-frame snapshot belonging to the parent; copying fb_slot
     * would let the child's later exit wrongly release the parent's still-live claim. */
    child->fb_slot = -1;
    /* maize-238 Decision: the fb-mmap window is excluded from fork's eager copy (region 0
     * only; the fb window lives in region 1, which build_address_space(child) leaves
     * unmapped). fb MEMORY does not propagate to a child, mirroring maize-236 D4's fb
     * REGISTRATION non-propagation one level down. */
    child->fb_mmap_va = 0;
    /* maize-174: fork inherits the process group, the block mask, and the installed
     * handlers (POSIX); pending signals are NOT inherited. */
    child->pgid = parent->pgid;
    child->pending = 0; child->blocked = parent->blocked;
    child->term_signal = 0; child->in_handler = 0;
    child->termios_valid = 0;   /* maize-250: the child has not itself set console termios yet */
    { unsigned long _s; for (_s = 0; _s < 32; ++_s) { child->handler[_s] = parent->handler[_s]; } }
    for (k = 0; k < QUESOS_PATH_CAP; ++k) { child->path[k] = parent->path[k]; }
    for (k = 0; k < QUESOS_PATH_CAP; ++k) { child->cwd[k] = parent->cwd[k]; }   /* maize-94: cwd inherited */
    child->brk_cur = parent->brk_cur;   /* maize-94: heap break inherited (pages copied below) */
    fdtable_copy(child, parent);   /* the child inherits the fd table (shared OFDs) */

    build_address_space(child);
    for (idx = 0; idx < 512; ++idx) {
        u64 ppte = pte_get(parent->l0_pa, idx);
        if ((ppte & PTE_V) && (ppte & PTE_U)) {
            u64 cframe = alloc_frame();
            memcpy((void *)cframe, (void *)(ppte & ~0xFFFul), PAGE_SIZE);
            pte_set(child->l0_pa, idx, (cframe & ~0xFFFul) | PTE_USER);
        }
    }

    /* Child's saved context is the copy at the same stack VA; force its RV slot to 0. */
    child->saved_rs = parent->saved_rs;
    as_write64(child, child->saved_rs + 11ul * 8ul, 0);
    return child->pid;
}

/* Deliver a reaped child's result into a waiting parent's saved context + status word,
 * free the child, and mark the parent runnable. */
static void deliver_wait(struct pcb *parent, struct pcb *child) {
    as_write64(parent, parent->saved_rs + 11ul * 8ul, (u64)child->pid);  /* RV = pid  */
    if (parent->wait_status_uva != 0) {
        as_write32(parent, parent->wait_status_uva, wait_status(child));
    }
    child->state = P_FREE;
    parent->state = P_RUNNABLE;
}

/* wait4/waitpid. Reap a matching zombie child immediately, or block until one exits
 * (the exiting child completes this call via deliver_wait). Returns the child pid, or
 * -ECHILD when there is no matching child. */
static long do_wait(long wpid, u64 status_uva) {
    struct pcb *parent = g_current;
    int i;

    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        struct pcb *c = &g_proc[i];
        if (c->state == P_ZOMBIE && c->parent == parent->pid
            && (wpid <= 0 || c->pid == wpid)) {
            long rpid = c->pid;
            if (status_uva != 0) { as_write32(parent, status_uva, wait_status(c)); }
            c->state = P_FREE;
            return rpid;
        }
    }

    if (!has_child(parent->pid, wpid)) { return -10; }   /* -ECHILD */

    parent->wait_for = wpid;
    parent->wait_status_uva = status_uva;
    parent->state = P_BLOCKED;
    schedule();   /* noreturn: the waker (do_exit) delivers this call's result later */
    return 0;     /* unreachable */
}

/* Terminate the current process. A child of a parent blocked in wait completes that
 * parent's wait; a child of init (a top-level worklist process, parent 0) is reaped by
 * quesOS with the transcript line; otherwise it lingers as a zombie until reaped. */
static void do_exit(long status) {
    struct pcb *self = g_current;

    self->exit_status = status;
    self->term_signal = 0;         /* maize-174: a normal _exit clears WIFSIGNALED */
    reap_tail(self);               /* maize-174: shared zombie/reap/SIGCHLD tail */
    schedule();   /* noreturn */
}

/* Marshal a user argv/envp pointer array (of user string pointers, NULL-terminated)
 * into the shared g_arg_* packed buffer, appending to `*nstr` / `*pack`. Reads through
 * the current address space (direct deref; CR0 = the calling process). Returns the
 * number of strings copied. */
static int marshal_vec(u64 vec_uva, int *nstr, u64 *pack, int max_more) {
    int n = 0;
    if (vec_uva == 0) { return 0; }
    while (n < max_more) {
        u64 sp = *(u64 *)(vec_uva + (u64)n * 8ul);
        u64 j = 0;
        if (sp == 0) { break; }
        g_arg_off[*nstr] = *pack;
        while (*(char *)(sp + j) != 0 && *pack < QUESOS_ARGBUF - 1) {
            g_arg_strbuf[*pack] = *(char *)(sp + j);
            (*pack)++; ++j;
        }
        g_arg_strbuf[(*pack)++] = 0;
        (*nstr)++; ++n;
    }
    return n;
}

/* execve: replace the calling process's image with `path`, passing argv/envp. The fd
 * table survives (per POSIX). The argv/envp and path are marshalled out of the OLD
 * address space first (direct deref, CR0 = the caller), then a fresh address space is
 * built and the new image loaded; the trampoline enters it on return. Does not return
 * on success (the process now runs the new image); a load failure kills the process. */
static long do_execve(u64 path_uva, u64 argv_uva, u64 envp_uva) {
    struct pcb *self = g_current;
    char in[QUESOS_PATH_CAP], kpath[QUESOS_PATH_CAP];
    int i, nstr = 0;
    u64 pack = 0, entry;

    copy_user_path(path_uva, in);
    join_path(self->cwd, in, kpath);   /* maize-94: resolve a relative exec target against cwd */

    /* maize-94 (decision 9084 enabler): pre-validate the target image BEFORE tearing down
     * the caller's address space, so a missing or malformed path returns an error to
     * execve() (as real Linux does) instead of destroying the caller with do_exit(127).
     * This is what lets a userland PATH walk (libc execvp's exact -> .mzx -> .mzb fallback,
     * or a shell's own retry) probe candidates: a miss returns ENOENT and the caller tries
     * the next form. The kernel still does NO name rewriting itself (it stays dumb). */
    {
        long psize = quesos_slurp(kpath);
        const u8 *pb = g_filebuf;
        if (psize < 0) { return -(long)QOS_ENOENT; }
        if (psize < (long)MZX_HEADER_SIZE
            || pb[0] != 'M' || pb[1] != 'Z' || pb[2] != 'X' || pb[3] != 0x01) {
            return -(long)QOS_ENOEXEC;
        }
    }

    marshal_vec(argv_uva, &nstr, &pack, QUESOS_MAX_ARG);
    g_arg_argc = nstr;
    marshal_vec(envp_uva, &nstr, &pack, QUESOS_MAX_ARG - nstr);
    g_arg_envc = nstr - g_arg_argc;
    g_arg_pack = pack;

    /* maize-236 Decision D5: release any framebuffer registration before the old address
     * space is torn down. The registered base is a frame of the OLD image about to be
     * orphaned; leaving it claimed would show stale memory and wrongly block the new
     * image's own SYS_fb_register with -EBUSY. */
    fb_release_held(self);

    /* Build a fresh address space (the old one's frames leak; acceptable for the POC
     * pool) and load the new image. build_start_block lays out the marshalled argv. */
    build_address_space(self);
    if (load_segments(self, kpath, &entry) != 0) {
        do_exit(127);   /* image destroyed and unloadable: terminate (noreturn) */
    }
    build_start_block(self, entry);
    /* maize-174: POSIX exec signal semantics. A caught handler (a VA above the SIG_IGN/
     * SIG_DFL sentinels 1/0) points into the OLD image about to be orphaned, so reset it
     * to SIG_DFL; never jump to a stale VA. SIG_IGN dispositions are preserved, as are the
     * blocked mask, the pending set, and the pgid (all untouched). No handler is in progress
     * in the new image and no old signal frame is carried over. */
    { int s; for (s = 0; s < 32; ++s) { if (self->handler[s] > 1) { self->handler[s] = 0; } } }
    self->in_handler = 0;
    self->sig_saved_rs = 0;
    for (i = 0; i < QUESOS_PATH_CAP - 1 && kpath[i]; ++i) { self->path[i] = kpath[i]; }
    self->path[i] = 0;
    return 0;
}

/* ==================================================================================
 * maize-236: framebuffer registration syscalls + exec/exit cleanup.
 * The device holds the registration table; quesOS tracks which slot (if any) a process
 * owns in pcb.fb_slot (Decision D1). IN/OUT are privileged, so only quesOS (supervisor)
 * can reach the ports: a user process must ask via these syscalls.
 * ================================================================================== */

/* SYS_fb_geometry(out_uva): write {u32 width; u32 height; u32 format} to the caller's
 * buffer. Reads the host-config ports directly (legal: quesOS runs supervisor). Always
 * succeeds. Geometry is global and fixed per boot (Decision D2). */
static long do_fb_geometry(u64 out_uva) {
    struct pcb *self = g_current;
    as_write32(self, out_uva,      (u32)quesos_fb_width());
    as_write32(self, out_uva + 4u, (u32)quesos_fb_height());
    as_write32(self, out_uva + 8u, (u32)quesos_fb_format());
    return 0;
}

/* SYS_fb_register(base_uva): register the caller's pixel buffer and return its slot.
 * -EBUSY if the caller already holds one (Decision D3), -EINVAL if base_uva is zero or its
 * page is unmapped, -ENOSPC if the table is full, -ENODEV if the device rejects the claim
 * (a display-less view; Decision D7, evolving maize-221). On success sets pcb.fb_slot. */
/* maize-238: the fb buffer's page count from the fixed per-boot geometry (XRGB8888 = 4
 * bytes/pixel), rounded up. 63 pages for 320x200x4. Shared by sys_fb_mmap and the
 * hardened do_fb_register contiguity walk. */
static u64 fb_page_count(void) {
    u64 bytes = (u64)quesos_fb_width() * (u64)quesos_fb_height() * 4ul;
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

static long do_fb_register(u64 base_uva) {
    struct pcb *self = g_current;
    u64 pte, pa;
    long slot;

    if (self->fb_slot >= 0) { return -(long)QOS_EBUSY; }
    if (base_uva == 0) { return -(long)QOS_EINVAL; }
    pte = pte_get(va_l0(self, base_uva), (base_uva >> 12) & 0x1FF);
    if ((pte & PTE_V) == 0) { return -(long)QOS_EINVAL; }
    pa = user_pa(self, base_uva);

    /* maize-238: validate that the mapped extent of [base, base + width*height*4) is
     * physically contiguous, not just base's own page. The device reads that linear range
     * with zero page-table awareness (doc 18's dumb-device contract), so a caller passing a
     * non-fb_mmap'd buffer whose mapped pages are scattered would silently scan out wrong
     * physical memory beyond page 0 -- defined bytes, wrong picture. Reject that with
     * -EINVAL. A page that is simply unmapped stops the walk rather than rejecting: the
     * existing one-page maize-236 fixtures register a tiny dummy buffer (the tail is
     * unmapped, no next page to check) and must stay green, and the device tolerates
     * reading defined-but-unwritten memory past a short buffer's end (devices.h: every
     * address is defined, no host OOB). A genuinely scattered buffer, by contrast, has a
     * MAPPED page at a non-contiguous physical address, which is what this catches. */
    {
        u64 n = fb_page_count();
        u64 base_page = base_uva & ~0xFFFul;
        u64 prev_pa = pa & ~0xFFFul;
        u64 k;
        for (k = 1; k < n; ++k) {
            u64 va = base_page + k * PAGE_SIZE;
            u64 pte2 = pte_get(va_l0(self, va), (va >> 12) & 0x1FF);
            u64 this_pa;
            if ((pte2 & PTE_V) == 0) { break; }   /* short buffer: no further page to check */
            this_pa = pte2 & ~0xFFFul;
            if (this_pa != prev_pa + PAGE_SIZE) { return -(long)QOS_EINVAL; }
            prev_pa = this_pa;
        }
    }

    /* Free-slot scan: select each slot and read its base back; a zero base is free. The
     * dispatcher runs with interrupts masked, so no other process can race the claim
     * between the scan and the write. */
    for (slot = 0; slot < QUESOS_FB_MAX_SLOTS; ++slot) {
        quesos_fb_slot_select((u64)slot);
        if (quesos_fb_base_read() == 0) { break; }
    }
    if (slot >= QUESOS_FB_MAX_SLOTS) { return -(long)QOS_ENOSPC; }

    quesos_fb_slot_select((u64)slot);
    quesos_fb_base_write(pa);
    /* Device rejected a display-less claim: STATUS bit2 set and the base stayed 0. */
    if ((quesos_fb_status_read() & 0x4u) != 0 || quesos_fb_base_read() == 0) {
        return -(long)QOS_ENODEV;
    }
    self->fb_slot = slot;
    return slot;
}

/* SYS_fb_release(): release the caller's registration. -EBADF if it holds none. */
static long do_fb_release(void) {
    struct pcb *self = g_current;
    if (self->fb_slot < 0) { return -(long)QOS_EBADF; }
    quesos_fb_slot_select((u64)self->fb_slot);
    quesos_fb_base_write(0);   /* zero base releases the slot (device switches scanout back) */
    self->fb_slot = -1;
    return 0;
}

/* SYS_fb_mmap(): map a contiguous, page-aligned framebuffer buffer into the caller's
 * fb-mmap window (FB_MMAP_BASE, region 1) and return its VA. No arguments (geometry is
 * fixed per boot, maize-236 D2). Idempotent: a second call while one buffer is already
 * mapped returns the same VA rather than -EBUSY. -ENOMEM if the frame pool cannot satisfy
 * the contiguous request (graceful, no PANIC). The buffer persists for the process's
 * lifetime (released implicitly at address-space teardown, like BSS/heap); there is no
 * fb_munmap. A registered scanout is then set up by passing this VA to SYS_fb_register. */
static long do_fb_mmap(void) {
    struct pcb *self = g_current;
    u64 n = fb_page_count();
    u64 base_pa = 0, i;
    if (self->fb_mmap_va != 0) { return self->fb_mmap_va; }   /* idempotent re-query */
    if (alloc_frames_contig(n, &base_pa) != 0) { return -(long)QOS_ENOMEM; }
    (void)ensure_l0(self, FB_MMAP_BASE);   /* lazily allocate region 1's L0 on first touch */
    for (i = 0; i < n; ++i) {
        map_user_page(self, FB_MMAP_BASE + i * PAGE_SIZE, base_pa + i * PAGE_SIZE);
    }
    self->fb_mmap_va = FB_MMAP_BASE;
    return (long)FB_MMAP_BASE;
}

/* Cleanup: release a held registration on exit / exec (Decision D5, the per-exec-lifetime
 * scoping). A no-op when the process holds none. */
static void fb_release_held(struct pcb *p) {
    if (p->fb_slot >= 0) {
        quesos_fb_slot_select((u64)p->fb_slot);
        quesos_fb_base_write(0);
        p->fb_slot = -1;
    }
}

/* Timer IRQ (vector 32): the running process is preempted at an instruction boundary.
 * The metal handler already saved its context and acked the timer; leave it RUNNABLE
 * (it was running, not blocked) and round-robin to the next runnable process. This is
 * what bounds a compute-bound process so it cannot starve the others. */
void quesos_timer_tick(void) {
    /* maize-238: sweep parked poll()/select() callers whose finite deadline has elapsed,
     * waking each with a 0 (timeout) result; their revents/fd_sets were committed empty
     * at call time and no recheck rewrote them, so a timeout returns a clean zero set. */
    int i;
    u64 now = sys_clock_ms();
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        struct pcb *p = &g_proc[i];
        if (p->state == P_BLOCKED && p->block_kind == BLK_POLL
            && p->poll_deadline_ms != 0 && now >= p->poll_deadline_ms) {
            wake_with(p, 0);
        }
    }
    if (g_current != 0) { g_current->state = P_RUNNABLE; }   /* 0 while idle-spinning */
    schedule();   /* noreturn */
}

/* Console input IRQ (vector 33): the console device signaled host-stdin readiness (a
 * data byte pending, or end-of-input) and raised its IRQ. maize-238 Branch A (decision
 * 9285): the IRQ does NOT pre-read/buffer a byte. If a reader is parked, CONSUME one byte
 * now -- readiness was just signaled, so the data-port read does not block -- and hand it
 * over; this is the parked read COMPLETING, at read time, not an eager pre-read into an
 * ownerless ring. With no reader parked, consume nothing (the byte stays in host stdin
 * until a read/poll drains it); just re-check poll()/select() waiters keyed to fd-0
 * readiness. Then reschedule (the metal handler saved the interrupted process, or none if
 * idle). */
void quesos_console_irq(void) {
    struct pcb *r = 0;
    int i;
    for (i = 0; i < QUESOS_MAX_PROC; ++i) {
        if (g_proc[i].state == P_BLOCKED && g_proc[i].block_kind == BLK_CONSOLE) {
            r = &g_proc[i];
            break;
        }
    }
    if (r != 0) {
        if (quesos_con_status() & CON_STAT_INPUT) {
            /* A data byte is pending: consume it at read time and complete the parked read.
             * A control byte (0x03/0x1C) in canonical mode is intercepted as a signal on the
             * foreground group and NOT delivered as data (the reader stays parked for a real
             * byte, matching the no-EINTR model); raw mode delivers it as an ordinary byte. */
            u8 b = (u8)quesos_con_data();
            if (quesos_con_status() & CON_STAT_EOF) { wake_with(r, 0); }              /* raced to EOF */
            else if (g_tty_isig && b == 0x03) { signal_fg_group(SIGINT); }
            else if (g_tty_isig && b == 0x1C) { signal_fg_group(SIGQUIT); }
            else { as_write8(r, r->block_buf, b); wake_with(r, 1); }
        } else if (quesos_con_status() & CON_STAT_EOF) {
            wake_with(r, 0);   /* end-of-input: the parked read returns 0 (EOF) */
        }
    }
    /* fd 0 became readable (or hit EOF): wake any poll()/select() caller watching it. */
    poll_recheck_all();
    if (g_current != 0) { g_current->state = P_RUNNABLE; }
    schedule();   /* noreturn */
}

/* ==================================================================================
 * maize-174: signal delivery, default actions, and the job-control / signal syscalls.
 * ================================================================================== */
static int lowest_set_bit(unsigned long m) {
    int b = 0;
    while ((m & 1ul) == 0ul) { m >>= 1; ++b; }
    return b + 1;   /* signal number (1-based) */
}

/* maize-250: restore the console when a process that owned console termios dies. Runs in
 * quesOS's own C dispatcher context (CLRSYSG), so it can issue raw sys_write/sys_tcsetattr
 * calls that reach the native provider directly, exactly as do_tcsetattr does.
 *
 * A clean exit (term_signal == 0) has already restored the parent's termios via the dying
 * process's own userspace cleanup (kilo's atexit -> disableRawMode -> tcsetattr) before it
 * called _exit, so the parent-side re-apply below just idempotently reconfirms that state.
 * An abnormal death (term_signal != 0) never ran that cleanup: if the process was in raw
 * mode it may have left the alternate screen buffer active, so emit the alt-screen-exit
 * sequence (mirroring kilo's own clean-exit sequence, demos/kilo/kilo.c) to un-strand the
 * visible terminal, then re-apply the parent's (the resuming foreground shell's) termios so
 * its prompt, line editing, and ISIG interception work again. */
static void restore_console_on_death(struct pcb *self) {
    struct pcb *parent;
    if (!self->termios_valid) { return; }   /* never touched console termios (e.g. a pipe filter) */
    if (self->term_signal != 0) {           /* abnormal/signaled death: userspace cleanup did not run */
        if ((termios_lflag(self->termios_img) & TIO_ICANON) == 0) {   /* was raw */
            sys_write(1, "\x1b[?1049l", 8);  /* leave the alt screen buffer, if it was entered */
        }
    }
    parent = find_by_pid(self->parent);
    if (parent != 0 && parent->termios_valid) {
        sys_tcsetattr(0, 0 /* TCSANOW */, parent->termios_img);
        g_tty_isig = (termios_lflag(parent->termios_img) & TIO_ISIG) ? 1 : 0;
    }
}

/* Shared zombie/reap tail for do_exit and terminate_by_signal, so wait4/zombie/reap
 * semantics are identical however a process ends. Also raises SIGCHLD on the parent. */
static void reap_tail(struct pcb *self) {
    struct pcb *parent;
    restore_console_on_death(self);   /* maize-250: un-strand + restore the console first */
    self->state = P_ZOMBIE;
    fdtable_close_all(self);   /* closing a pipe write end wakes blocked readers (EOF) */
    fb_release_held(self);     /* maize-236: free any framebuffer registration */
    parent = find_by_pid(self->parent);
    if (parent != 0) { parent->pending |= (1ul << (SIGCHLD - 1)); }   /* maize-174 SIGCHLD */
    if (parent != 0 && parent->state == P_BLOCKED
        && (parent->wait_for <= 0 || parent->wait_for == self->pid)) {
        deliver_wait(parent, self);   /* frees self, wakes parent */
    } else if (self->parent == 0) {
        qos_puts("[quesos] reaped ");
        qos_puts(self->path);
        qos_puts(" status=");
        qos_put_u64((u64)(self->exit_status & 0xFF));
        qos_puts("\n");
        self->state = P_FREE;
    }
    /* else: leave a zombie for a later wait. */
}

/* Default action: terminate. Records the terminating signal (WIFSIGNALED) and reaps. */
static void terminate_by_signal(struct pcb *p, int sig) {
    p->term_signal = sig;
    p->exit_status = 0;
    reap_tail(p);
    schedule();   /* noreturn */
}

/* Below the interrupted frame (left intact at saved_rs), lay out the trampoline bytes,
 * a return-address word, then a fresh handler frame. switch_to enters the handler with
 * R0 = sig; the handler RETs into the trampoline, whose SYS $0F (rt_sigreturn) restores
 * the interrupted context. Opaque to everything but this pair (OQ 9015). */
static void push_signal_frame(struct pcb *p, int sig) {
    u64 old_rs = p->saved_rs;
    unsigned long tramp_len = (unsigned long)(quesos_sigreturn_tramp_end - quesos_sigreturn_tramp);
    u64 tramp_va, ret_va, new_rs;
    unsigned long k;

    tramp_va = (old_rs - tramp_len) & ~15ul;
    ret_va   = tramp_va - 8ul;
    new_rs   = ret_va - 17ul * 8ul;   /* 13 GP + aux + cause + rf + pc */

    for (k = 0; k < tramp_len; ++k) { as_write8(p, tramp_va + k, quesos_sigreturn_tramp[k]); }
    as_write64(p, ret_va, tramp_va);                       /* handler RETs here */

    as_write64(p, new_rs + 0ul * 8ul, (u64)sig);           /* R0 = signal number */
    for (k = 1; k < 13; ++k) { as_write64(p, new_rs + k * 8ul, 0); }
    as_write64(p, new_rs + 13ul * 8ul, 0);                 /* aux */
    as_write64(p, new_rs + 14ul * 8ul, 0);                 /* cause */
    as_write64(p, new_rs + 15ul * 8ul, USER_RF);           /* rf: user, ints on, guest */
    as_write64(p, new_rs + 16ul * 8ul, p->handler[sig]);   /* pc = handler entry */

    p->sig_saved_rs = old_rs;
    p->in_handler = 1;
    p->saved_rs = new_rs;
}

/* Apply the highest-priority pending, unblocked signal to p before it resumes. Called
 * from schedule() at the one choke point every resume passes through. */
static void deliver_pending_signal(struct pcb *p) {
    unsigned long ready;
    int sig;
    /* SIGKILL is uncatchable and unblockable (OQ 9014): it must terminate the process
     * even while it is running a handler (in_handler set), so it precedes the defer
     * guard below. Otherwise a runaway or timer-preempted handler could never be killed. */
    if (p->pending & (1ul << (SIGKILL - 1))) {
        p->pending &= ~(1ul << (SIGKILL - 1));
        terminate_by_signal(p, SIGKILL);   /* noreturn */
    }
    if (p->in_handler) { return; }   /* v1: one handler at a time; defer others while in one */
    ready = p->pending & ~p->blocked;
    if (ready == 0ul) { return; }
    sig = lowest_set_bit(ready);
    p->pending &= ~(1ul << (sig - 1));
    if (p->handler[sig] == 0) {           /* SIG_DFL */
        if (sig == SIGCHLD) { return; }   /* default action: ignore */
        terminate_by_signal(p, sig);      /* default action: terminate (noreturn) */
    }
    if (p->handler[sig] == 1) { return; } /* SIG_IGN */
    push_signal_frame(p, sig);            /* handler dispatch */
}

/* SYS_kill: pid>0 one process; pid==0 caller's group; pid<0 the group -pid. */
static long do_kill(long pid, int sig) {
    if (sig < 1 || sig > 31) { return -(long)QOS_EINVAL; }
    if (pid > 0) {
        struct pcb *p = find_by_pid(pid);
        if (p == 0) { return -(long)QOS_ESRCH; }
        raise_on_pcb(p, sig);
        return 0;
    }
    if (pid == 0) { return raise_on_pgid(g_current->pgid, sig) ? 0 : -(long)QOS_ESRCH; }
    return raise_on_pgid(-pid, sig) ? 0 : -(long)QOS_ESRCH;
}

/* SYS_rt_sigaction: subset -- sa_handler only (sa_mask/sa_flags read/written as 0). */
static long do_rt_sigaction(long sig, u64 act_uva, u64 oldact_uva) {
    struct pcb *self = g_current;
    if (sig < 1 || sig > 31) { return -(long)QOS_EINVAL; }
    if (sig == SIGKILL) { return -(long)QOS_EINVAL; }   /* uncatchable (OQ 9014) */
    if (oldact_uva != 0) {
        as_write64(self, oldact_uva + 0ul, self->handler[sig]);
        as_write64(self, oldact_uva + 8ul, 0);
        as_write32(self, oldact_uva + 16ul, 0);
    }
    if (act_uva != 0) { self->handler[sig] = *(u64 *)(act_uva + 0ul); }
    return 0;
}

/* SYS_rt_sigprocmask: how in {SIG_BLOCK=0, SIG_UNBLOCK=1, SIG_SETMASK=2}. */
static long do_rt_sigprocmask(long how, u64 set_uva, u64 oldset_uva) {
    struct pcb *self = g_current;
    unsigned long set;
    if (oldset_uva != 0) { as_write64(self, oldset_uva, self->blocked); }
    if (set_uva != 0) {
        set = *(unsigned long *)(set_uva);
        set &= ~(1ul << (SIGKILL - 1));   /* SIGKILL cannot be blocked (OQ 9014) */
        if (how == 0) { self->blocked |= set; }
        else if (how == 1) { self->blocked &= ~set; }
        else if (how == 2) { self->blocked = set; }
        /* maize-94: POSIX -- unblocking a signal that is already pending delivers it
         * immediately. Without this, a shell (oksh) that blocks SIGCHLD around its job
         * bookkeeping and then unblocks it inside sigsuspend() to wait for a child would
         * never see the pending SIGCHLD (quesOS otherwise only delivers on scheduler
         * resume), so its foreground-wait loop would spin forever. deliver_pending_signal
         * is a no-op unless the new mask makes a pending signal ready; when it dispatches
         * a handler it repoints saved_rs, and the r[11]=result write in quesos_syscall
         * still lands on the saved (post-handler resume) frame, so the sigprocmask return
         * value is preserved. */
        deliver_pending_signal(self);
    }
    return 0;
}

static long do_setpgid(long pid, long pgid) {
    struct pcb *p = (pid == 0) ? g_current : find_by_pid(pid);
    if (p == 0) { return -(long)QOS_ESRCH; }
    p->pgid = (pgid == 0) ? p->pid : pgid;
    return 0;
}
static long do_getpgid(long pid) {
    struct pcb *p = (pid == 0) ? g_current : find_by_pid(pid);
    if (p == 0) { return -(long)QOS_ESRCH; }
    return p->pgid;
}

void quesos_syscall(void) {
    u64 *r = (u64 *)g_current->saved_rs;
    u64 num = r[13];
    u64 a0 = r[0], a1 = r[1], a2 = r[2], a3 = r[3], a4 = r[4];
    long result;

    switch (num) {
        case SYS_write:  result = do_write(a0, a1, (long)a2);      break;
        case SYS_read:   result = do_read(a0, a1, (long)a2);       break;
        case SYS_open:   result = do_open(a0, (long)a1, (long)a2); break;
        case SYS_close:  result = do_close(a0);                    break;
        case SYS_brk:    result = do_brk(a0);                      break;   /* maize-94 */
        /* maize-94 decision 8941: native hostfs file/dir forwarders. */
        case SYS_fstat:      result = do_fstat(a0, a1);                    break;
        case SYS_lseek:      result = do_lseek(a0, (long)a1, (long)a2);    break;
        case SYS_getdents64: result = do_getdents64(a0, a1, (long)a2);     break;
        case SYS_unlink:     result = do_unlink(a0);                       break;
        case SYS_mkdir:      result = do_mkdir(a0, (long)a1);              break;
        case SYS_rename:     result = do_rename(a0, a1);                   break;
        case SYS_ftruncate:  result = do_ftruncate(a0, (long)a1);         break;
        /* maize-94 decision 8940: per-process working directory. */
        case SYS_chdir:      result = do_chdir(a0);                        break;
        case SYS_getcwd:     result = do_getcwd(a0, (long)a1);            break;
        /* maize-94 (OQ 8951 ruling): forwarded console termios. */
        case SYS_tcgetattr:  result = do_tcgetattr(a0, a1);               break;
        case SYS_tcsetattr:  result = do_tcsetattr(a0, (long)a1, a2);     break;
        /* maize-94 (operator reopen): forwarded terminal-size query so an interactive oksh
         * on the default input path reaches its prompt instead of stranding on $F6. */
        case SYS_ttysize:    result = do_ttysize(a0, a1);                 break;
        case SYS_pipe:   result = do_pipe(a0);                     break;
        case SYS_dup:    result = do_dup(a0);                      break;
        case SYS_dup2:   result = do_dup2(a0, a1);                 break;
        case SYS_getpid: result = g_current->pid;                 break;
        case SYS_fork:   result = do_fork();                      break;
        case SYS_wait4:  result = do_wait((long)a0, a1);          break;
        case SYS_execve: result = do_execve(a0, a1, a2);
                         if (result == 0) { return; }             /* new image entered */
                         break;
        case SYS_exit:   do_exit((long)a0); return;               /* noreturn */
        case SYS_fb_geometry: result = do_fb_geometry(a0);        break;
        case SYS_fb_register: result = do_fb_register(a0);        break;
        case SYS_fb_release:  result = do_fb_release();           break;
        /* maize-238 Family A: unix-domain sockets. */
        case SYS_socket:     result = do_socket((long)a0, (long)a1, (long)a2);  break;
        case SYS_bind:       result = do_bind(a0, a1, a2);                      break;
        case SYS_connect:    result = do_connect(a0, a1, a2);                   break;
        case SYS_listen:     result = do_listen(a0, (long)a1);                  break;
        case SYS_accept:     result = do_accept(a0, a1, a2);                    break;
        case SYS_socketpair: result = do_socketpair((long)a0, (long)a1, (long)a2, a3); break;
        /* maize-238 Family B: select/poll readiness multiplexing. */
        case SYS_poll:       result = do_poll(a0, a1, (long)a2);                break;
        case SYS_select:     result = do_select((long)a0, a1, a2, a3, a4);      break;
        /* maize-238 Family C: framebuffer mmap. */
        case SYS_fb_mmap:    result = do_fb_mmap();                             break;
        case SYS_kill:           result = do_kill((long)a0, (int)a1);          break;
        case SYS_rt_sigaction:   result = do_rt_sigaction((long)a0, a1, a2);   break;
        case SYS_rt_sigprocmask: result = do_rt_sigprocmask((long)a0, a1, a2); break;
        case SYS_rt_sigreturn:
            g_current->saved_rs = g_current->sig_saved_rs;   /* pop the signal frame */
            g_current->in_handler = 0;
            return;   /* resume the interrupted context (switch_to reloads saved_rs) */
        case SYS_setpgid:        result = do_setpgid((long)a0, (long)a1);      break;
        case SYS_getpgid:        result = do_getpgid((long)a0);                break;
        case SYS_tcgetpgrp:      result = g_fg_pgid;                           break;
        case SYS_tcsetpgrp:      g_fg_pgid = (long)a0; result = 0;             break;
        /* maize-94: forward the pointer-free monotonic clock to the native provider. */
        case SYS_clock_ms:       result = (long)sys_clock_ms();                break;
        /* maize-94: the bulk-memory accelerators cannot be forwarded under paging (user
         * VAs are not native-physical); return -ENOSYS silently so the guest RT copy
         * loop takes over. See the SYS_bulk_* note above. */
        case SYS_bulk_copy:
        case SYS_bulk_set:       result = -38;                                 break;
        default:
            /* maize-94 (operator reopen) unhandled-syscall POLICY: return -ENOSYS to the
             * caller (never strand the process) so a Linux-shaped userland degrades on an
             * unforwarded call the way it would on a real kernel that lacks the syscall,
             * and keep the one-line diagnostic so an operator can see which number to
             * forward next. This is what lets a shell survive a call quesOS has not wired
             * yet (e.g. before $F6 ttysize was forwarded, oksh got -ENOSYS here and would
             * fall back rather than crash). */
            qos_puts("[quesos] unhandled syscall ");
            qos_put_u64(num);
            qos_puts("\n");
            result = -38;   /* -ENOSYS */
            break;
    }
    r[11] = (u64)result;    /* RV slot */
}

/* ==================================================================================
 * Init entry from _start (quesos_boot.mazm), after the cause-7 handler is installed and
 * RS is on quesOS's private kernel stack. argv[1..] is the exec worklist; copy the paths
 * into kernel storage before any child stack is built.
 * ================================================================================== */
void quesos_main(long argc, char **argv) {
    long i;

    g_worklist_count = 0;

    for (i = 1; i < argc && g_worklist_count < QUESOS_MAX_PROC; ++i) {
        const char *src = argv[i];
        long j = 0;
        while (src[j] && j < QUESOS_PATH_CAP - 1) {
            g_pathbuf[g_worklist_count][j] = src[j];
            ++j;
        }
        g_pathbuf[g_worklist_count][j] = 0;
        ++g_worklist_count;
    }

    if (g_worklist_count == 0) {
        qos_puts("[quesos] no programs on the exec worklist; powering off\n");
        quesos_poweroff();
    }

    ofd_init();   /* allocate the shared stdio open-file descriptions (fd 0/1/2) */

    qos_puts("[quesos] init: cause-7 handler resident; running ");
    qos_put_u64((u64)g_worklist_count);
    qos_puts(" program(s)\n");

    /* Spawn every worklist entry as a top-level process (parent = init = 0), then let
     * the scheduler run them. With no forking and no timer this is round-robin over
     * processes that each run to exit, so the worklist runs in order (the maize-24
     * transcript); a process that forks introduces real concurrency the scheduler and
     * wait path handle. */
    for (i = 0; i < g_worklist_count; ++i) {
        struct pcb *sp = spawn(g_pathbuf[i], 0);
        if (sp == 0) {
            qos_puts("[quesos] cannot start ");
            qos_puts(g_pathbuf[i]);
            qos_puts("\n");
        } else if (g_fg_pgid == 0) {
            g_fg_pgid = sp->pgid;   /* maize-174: the first worklist job owns the tty */
        }
    }

    /* Arm the instruction-tick timer only now that setup is done, so no interrupt is
     * pending when the first process is entered (the timer counts its first full slice
     * from the first user instruction; a program shorter than a slice is never
     * preempted, which keeps short-program transcripts stable). */
    quesos_arm_timer();
    schedule();   /* noreturn */
}

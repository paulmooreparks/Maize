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
 *   0x00002000 .. 0x000FFFFF  user VA region        per-process user pages (U=1)
 *   0x00100000 .. 0x001FFFFF  quesOS image (code+data)   kernel (U=0), 4 KiB leaves
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

/* --- The metal half (quesos_boot.mazm). ---------------------------------------------
 * quesos_switch_to(pcb) loads the process's saved GP context, MOVTCRs its page-table
 * root into CR0, and IRETs into it (first entry or resume); it never returns. */
struct pcb;
void quesos_switch_to(struct pcb *p);        /* MOVTCR satp; restore regs; IRET (noreturn) */
void quesos_poweroff(void);                  /* CLRSYSG; SYS $3C (native VM halt)          */
void quesos_arm_timer(void);                 /* program the instruction-tick timer (OUT)   */

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
#define QUESOS_IMG_BASE   0x00100000ul   /* quesOS link base; code+data in [base,+1MiB) */
#define QUESOS_IMG_TOP    0x00200000ul
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
#define SYS_pipe   0x16   /* Linux x86-64 numbers below */
#define SYS_dup    0x20
#define SYS_dup2   0x21
#define SYS_getpid 0x27
#define SYS_fork   0x39
#define SYS_execve 0x3B
#define SYS_exit   0x3C
#define SYS_wait4  0x3D

/* ==================================================================================
 * Process table.
 * ================================================================================== */
#define QUESOS_MAX_PROC 24
#define QUESOS_PATH_CAP 256
#define QUESOS_MAX_FD   16       /* per-process file descriptors                        */

enum proc_state { P_FREE = 0, P_RUNNABLE, P_BLOCKED, P_ZOMBIE };

/* Why a process is BLOCKED (block_kind), so the right waker completes its syscall. */
#define BLK_NONE   0
#define BLK_WAIT   1             /* parked in wait4 (woken by an exiting child)         */
#define BLK_PIPE_R 2             /* parked reading an empty pipe (woken by a writer)    */
#define BLK_PIPE_W 3             /* parked writing a full pipe (woken by a reader)      */

/* saved_rs (offset 0) and root_pa (offset 8) are read by the quesos_switch_to metal at
 * those exact byte offsets; keep them first and in this order. */
struct pcb {
    u64 saved_rs;                 /* offset 0:  RS at the saved-regs block (context)   */
    u64 root_pa;                  /* offset 8:  page-table root physical (satp = |1)   */
    u64 l0_pa;                    /* [0,2 MiB) L0 table physical (user-page mapping)    */
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
    char path[QUESOS_PATH_CAP];   /* argv[0] for the reap transcript                   */
};

static struct pcb g_proc[QUESOS_MAX_PROC];
struct pcb *g_current;            /* the running process (read by the metal trampoline) */
static long g_next_pid = 1;

static void schedule(void);       /* round-robin scheduler; noreturn (defined below)    */

/* Boot worklist: quesOS's own argv[1..] is the exec worklist (maize-24 decision D7). */
static char g_pathbuf[QUESOS_MAX_PROC][QUESOS_PATH_CAP];
static long g_worklist_count;

/* Whole child image is slurped here before segment placement. */
#define QUESOS_FILEBUF_CAP (256u * 1024u)
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
    p->l0_pa   = l0;
    return 0;
}

/* Map one user 4 KiB page (VA < 2 MiB) to a physical frame, U=1, in p's L0. */
static void map_user_page(struct pcb *p, u64 va, u64 frame) {
    pte_set(p->l0_pa, (va >> 12) & 0x1FF, (frame & ~0xFFFul) | PTE_USER);
}

/* Resolve a mapped user VA to its physical frame address (for building the image /
 * stack contents while another address space, or bare mode, is active in CR0). */
static u64 user_pa(struct pcb *p, u64 va) {
    u64 pte = pte_get(p->l0_pa, (va >> 12) & 0x1FF);
    return (pte & ~0xFFFul) + (va & 0xFFF);
}

static void as_write8(struct pcb *p, u64 va, u8 val)   { *(u8 *)user_pa(p, va) = val; }
static void as_write32(struct pcb *p, u64 va, u32 val) { *(u32 *)user_pa(p, va) = val; }
static void as_write64(struct pcb *p, u64 va, u64 val) { *(u64 *)user_pa(p, va) = val; }

/* Ensure the page containing user VA `va` is mapped (allocate + map on first touch). */
static void ensure_user_page(struct pcb *p, u64 va) {
    u64 idx = (va >> 12) & 0x1FF;
    if ((pte_get(p->l0_pa, idx) & PTE_V) == 0) {
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
    }
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
#define QUESOS_MAX_PIPE 16
#define PIPE_CAP        4096u

#define OFD_FREE   0
#define OFD_NATIVE 1
#define OFD_PIPE_R 2
#define OFD_PIPE_W 3
#define OFD_RESVD  4    /* reserved by ofd_alloc; the caller fills in the real kind */

struct ofd {
    int  kind;
    long native_fd;   /* OFD_NATIVE: the host fd handed to the native SYS provider */
    int  pipe_idx;    /* OFD_PIPE_*: index into g_pipe                              */
    int  refcount;
};
static struct ofd g_ofd[QUESOS_MAX_OFD];

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
            return i;
        }
    }
    return -1;
}
static void ofd_ref(int idx) { if (idx >= 0) { g_ofd[idx].refcount++; } }

static void pipe_wake_readers(int pi);
static void pipe_wake_writers(int pi);

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
    }
    o->kind = OFD_FREE;
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
    for (j = 0; j < QUESOS_PATH_CAP - 1 && path[j]; ++j) { p->path[j] = path[j]; }
    p->path[j] = 0;
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
            quesos_switch_to(g_current);   /* noreturn */
        }
    }
    quesos_poweroff();   /* nothing runnable: worklist drained */
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
 * identity-mapped kernel staging area, then issue the native SYS. */
static long native_write(long nfd, u64 uva, long count) {
    long n = bounce_out(uva, count);
    return sys_write(nfd, g_iobuf, n);
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
    if (o->kind == OFD_PIPE_W) {
        int pi = o->pipe_idx;
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

static long do_read(u64 fd_num, u64 uva, long count) {
    struct pcb *self = g_current;
    struct ofd *o = fd_ofd(self, fd_num);
    if (o == 0) { return -9; }   /* -EBADF */
    if (o->kind == OFD_NATIVE) { return native_read(o->native_fd, uva, count); }
    if (o->kind == OFD_PIPE_R) {
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

/* open/close/dup/dup2/pipe: manage the fd table + open-file descriptions. */
static long do_open(u64 path_uva, long flags, long mode) {
    char kpath[QUESOS_PATH_CAP];
    int i, slot, o;
    long nfd;
    for (i = 0; i < QUESOS_PATH_CAP - 1 && *(char *)(path_uva + (u64)i); ++i) {
        kpath[i] = *(char *)(path_uva + (u64)i);
    }
    kpath[i] = 0;
    nfd = sys_open(kpath, flags, mode);
    if (nfd < 0) { return nfd; }
    slot = fd_alloc_slot(g_current);
    o = ofd_alloc();
    if (slot < 0 || o < 0) { sys_close(nfd); return -24; }   /* -EMFILE */
    g_ofd[o].kind = OFD_NATIVE; g_ofd[o].native_fd = nfd; g_ofd[o].refcount = 1;
    g_current->fd[slot] = o;
    return slot;
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

/* Encode a normal-exit wait status the way Linux does: exit code in bits 8..15
 * (WIFEXITED true, WEXITSTATUS = (status >> 8) & 0xFF). */
static u32 wait_status(long exit_code) {
    return (u32)((exit_code & 0xFF) << 8);
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
    for (k = 0; k < QUESOS_PATH_CAP; ++k) { child->path[k] = parent->path[k]; }
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
        as_write32(parent, parent->wait_status_uva, wait_status(child->exit_status));
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
            if (status_uva != 0) { as_write32(parent, status_uva, wait_status(c->exit_status)); }
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
    struct pcb *parent;

    self->exit_status = status;
    self->state = P_ZOMBIE;
    fdtable_close_all(self);   /* closing a pipe write end wakes blocked readers (EOF) */

    parent = find_by_pid(self->parent);
    if (parent != 0 && parent->state == P_BLOCKED
        && (parent->wait_for <= 0 || parent->wait_for == self->pid)) {
        deliver_wait(parent, self);   /* frees self, wakes parent */
    } else if (self->parent == 0) {
        qos_puts("[quesos] reaped ");
        qos_puts(self->path);
        qos_puts(" status=");
        qos_put_u64((u64)(status & 0xFF));
        qos_puts("\n");
        self->state = P_FREE;
    }
    /* else: leave a zombie for a later wait. */

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
    char kpath[QUESOS_PATH_CAP];
    int i, nstr = 0;
    u64 pack = 0, entry;

    for (i = 0; i < QUESOS_PATH_CAP - 1 && *(char *)(path_uva + (u64)i); ++i) {
        kpath[i] = *(char *)(path_uva + (u64)i);
    }
    kpath[i] = 0;

    marshal_vec(argv_uva, &nstr, &pack, QUESOS_MAX_ARG);
    g_arg_argc = nstr;
    marshal_vec(envp_uva, &nstr, &pack, QUESOS_MAX_ARG - nstr);
    g_arg_envc = nstr - g_arg_argc;
    g_arg_pack = pack;

    /* Build a fresh address space (the old one's frames leak; acceptable for the POC
     * pool) and load the new image. build_start_block lays out the marshalled argv. */
    build_address_space(self);
    if (load_segments(self, kpath, &entry) != 0) {
        do_exit(127);   /* image destroyed and unloadable: terminate (noreturn) */
    }
    build_start_block(self, entry);
    for (i = 0; i < QUESOS_PATH_CAP - 1 && kpath[i]; ++i) { self->path[i] = kpath[i]; }
    self->path[i] = 0;
    return 0;
}

/* Timer IRQ (vector 32): the running process is preempted at an instruction boundary.
 * The metal handler already saved its context and acked the timer; leave it RUNNABLE
 * (it was running, not blocked) and round-robin to the next runnable process. This is
 * what bounds a compute-bound process so it cannot starve the others. */
void quesos_timer_tick(void) {
    g_current->state = P_RUNNABLE;
    schedule();   /* noreturn */
}

void quesos_syscall(void) {
    u64 *r = (u64 *)g_current->saved_rs;
    u64 num = r[13];
    u64 a0 = r[0], a1 = r[1], a2 = r[2];
    long result;

    switch (num) {
        case SYS_write:  result = do_write(a0, a1, (long)a2);      break;
        case SYS_read:   result = do_read(a0, a1, (long)a2);       break;
        case SYS_open:   result = do_open(a0, (long)a1, (long)a2); break;
        case SYS_close:  result = do_close(a0);                    break;
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
        default:
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
        if (spawn(g_pathbuf[i], 0) == 0) {
            qos_puts("[quesos] cannot start ");
            qos_puts(g_pathbuf[i]);
            qos_puts("\n");
        }
    }

    /* Arm the instruction-tick timer only now that setup is done, so no interrupt is
     * pending when the first process is entered (the timer counts its first full slice
     * from the first user instruction; a program shorter than a slice is never
     * preempted, which keeps short-program transcripts stable). */
    quesos_arm_timer();
    schedule();   /* noreturn */
}

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
#define SYS_getpid 0x27   /* Linux x86-64 number for getpid */
#define SYS_exit   0x3C

/* ==================================================================================
 * Process table.
 * ================================================================================== */
#define QUESOS_MAX_PROC 24
#define QUESOS_PATH_CAP 256

enum proc_state { P_FREE = 0, P_RUNNABLE, P_BLOCKED, P_ZOMBIE };

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
    char path[QUESOS_PATH_CAP];   /* argv[0] for the reap transcript                   */
};

static struct pcb g_proc[QUESOS_MAX_PROC];
struct pcb *g_current;            /* the running process (read by the metal trampoline) */
static long g_next_pid = 1;

/* Boot worklist: quesOS's own argv[1..] is the exec worklist (maize-24 decision D7). */
static char g_pathbuf[QUESOS_MAX_PROC][QUESOS_PATH_CAP];
static long g_worklist_count;
static long g_worklist_next;

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

/* Map the user stack region and build the process-start block + a synthesized trap
 * frame so quesos_switch_to enters at `entry` in user mode. Layout on the user stack,
 * low to high address:
 *   [R0..RB]  13 saved GP regs (all 0)   <- pcb->saved_rs points here
 *   [aux][cause]                          (2 words, ignored on entry)
 *   [rf][pc]                              (rf = USER_RF, pc = entry)
 *   [argc][argv0][NULL][NULL]             (the SysV start block crt0 consumes)
 *   [argv0 string ...]
 * After quesos_switch_to (POP x13; ADD $10; IRET) RS lands on [argc]. */
static void build_first_entry(struct pcb *p, const char *path, u64 entry) {
    u64 top = USER_STACK_TOP;
    u64 plen = qos_strlen(path) + 1;
    u64 string_base = top - plen;
    u64 ptr_block = 8ul * (1u + 2u + 1u);      /* argc + argv0 + NULL + envp NULL */
    u64 argc_va = (string_base - ptr_block) & ~7ul;
    u64 pc_va    = argc_va - 8;
    u64 rf_va    = argc_va - 16;
    u64 cause_va = argc_va - 24;
    u64 aux_va   = argc_va - 32;
    u64 regs_base = aux_va - 13ul * 8ul;
    u64 va;
    u64 k;

    /* Map the stack region [top - USER_STACK_PAGES*PAGE, top). */
    for (va = top - (u64)USER_STACK_PAGES * PAGE_SIZE; va < top; va += PAGE_SIZE) {
        ensure_user_page(p, va);
    }

    for (k = 0; k < plen - 1; ++k) { as_write8(p, string_base + k, (u8)path[k]); }
    as_write8(p, string_base + (plen - 1), 0);

    as_write64(p, argc_va,      1);
    as_write64(p, argc_va + 8,  string_base);
    as_write64(p, argc_va + 16, 0);
    as_write64(p, argc_va + 24, 0);

    as_write64(p, pc_va,    entry);
    as_write64(p, rf_va,    USER_RF);
    as_write64(p, cause_va, 0);
    as_write64(p, aux_va,   0);

    for (k = 0; k < 13; ++k) { as_write64(p, regs_base + k * 8, 0); }

    p->saved_rs = regs_base;
}

/* Parse the .mzx and place each segment into freshly-allocated user frames mapped at
 * the segment vaddr, zero-filling the BSS tail. Returns 0 on success. */
static int load_image_into(struct pcb *p, const char *path) {
    long size = quesos_slurp(path);
    const u8 *b = g_filebuf;
    u16 seg_count;
    u64 entry;
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
    entry     = rd_u64(b, 8);
    shoff     = rd_u64(b, 16);

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

    build_first_entry(p, path, entry);
    return 0;
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
    if (p == 0) { return 0; }
    p->state = P_RUNNABLE;
    p->pid = g_next_pid++;
    p->parent = parent;
    p->exit_status = 0;
    for (j = 0; j < QUESOS_PATH_CAP - 1 && path[j]; ++j) { p->path[j] = path[j]; }
    p->path[j] = 0;
    if (build_address_space(p) != 0 || load_image_into(p, path) != 0) {
        p->state = P_FREE;
        return 0;
    }
    return p;
}

/* ==================================================================================
 * Reap loop: run the boot worklist one process at a time (single-tasking behavior is
 * the degenerate case of the multi-process kernel; the maize-24 selfcheck exercises
 * it and is the regression anchor). fork/wait/scheduler land in later phases.
 * ================================================================================== */
static void run_next_worklist(void) {
    for (;;) {
        struct pcb *p;
        if (g_worklist_next >= g_worklist_count) {
            quesos_poweroff();   /* noreturn */
        }
        p = spawn(g_pathbuf[g_worklist_next], 0);
        if (p == 0) {
            qos_puts("[quesos] cannot start ");
            qos_puts(g_pathbuf[g_worklist_next]);
            qos_puts("\n");
            ++g_worklist_next;
            continue;
        }
        g_current = p;
        quesos_switch_to(p);   /* noreturn: enter the process in user mode */
    }
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

static long do_write(u64 fd, u64 uva, long count) {
    long n = bounce_out(uva, count);
    return sys_write((long)fd, g_iobuf, n);
}

static long do_read(u64 fd, u64 uva, long count) {
    long n = count;
    long got;
    long i;
    if (n < 0) { n = 0; }
    if (n > (long)QUESOS_IOBUF_CAP) { n = (long)QUESOS_IOBUF_CAP; }
    got = sys_read((long)fd, g_iobuf, n);
    for (i = 0; i < got; ++i) { *(u8 *)(uva + (u64)i) = g_iobuf[i]; }
    return got;
}

/* Record the exiting process's status and run the next worklist entry (or power off).
 * Later phases turn this into zombie + waitpid + scheduler; single-tasking today. */
static void do_exit(long status) {
    g_current->exit_status = status;
    g_current->state = P_ZOMBIE;

    qos_puts("[quesos] reaped ");
    qos_puts(g_current->path);
    qos_puts(" status=");
    qos_put_u64((u64)(status & 0xFF));
    qos_puts("\n");

    g_current->state = P_FREE;
    ++g_worklist_next;
    run_next_worklist();   /* noreturn */
}

void quesos_syscall(void) {
    u64 *r = (u64 *)g_current->saved_rs;
    u64 num = r[13];
    u64 a0 = r[0], a1 = r[1], a2 = r[2];
    long result;

    switch (num) {
        case SYS_write:  result = do_write(a0, a1, (long)a2); break;
        case SYS_read:   result = do_read(a0, a1, (long)a2);  break;
        case SYS_getpid: result = g_current->pid;             break;
        case SYS_exit:   do_exit((long)a0); return;           /* noreturn */
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
    g_worklist_next = 0;

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

    qos_puts("[quesos] init: cause-7 handler resident; running ");
    qos_put_u64((u64)g_worklist_count);
    qos_puts(" program(s)\n");

    run_next_worklist();   /* noreturn */
}

/* maize-326: guest-supplied-length bounds self-check over the three older,
 * Linux-numbered syscalls that let a raw guest count size a host buffer:
 * sys_read (SYS $00), sys_write (SYS $01) and sys_getdents64 (SYS $D9).
 * Prints exactly "rw-bounds: PASS" iff every invariant holds, else a single
 * FAIL line naming the leg.
 *
 * SECURITY (deny-by-default): each of those three sites used to feed `count`
 * straight into a host std::vector resize (sys_read, sys_getdents64) or into
 * memory_module::read's reserve (sys_write), with no cap and no base+len wrap
 * check. An implausible count therefore threw std::length_error or
 * std::bad_alloc out of the VM with nothing to catch it, so a guest could abort
 * the host process with one syscall and no mount grant at all. The fix rejects
 * an oversized count with -EINVAL and a wrapping address+count with -EFAULT
 * BEFORE any allocation or guest-memory read, matching the idiom $F3/$F4/$F5
 * already use.
 *
 * This fixture drives both bad shapes at all three sites and asserts the
 * -errno band, the exact errno, an untouched destination sentinel, and that the
 * VM is still running afterwards. It also drives one well-formed call per
 * syscall so the new check is shown to reject only the bad ones.
 *
 * The rejection legs need no hostfs mount: the check runs before the fd is
 * inspected, so an arbitrary never-opened fd is enough and the fixture stays
 * bare-VM.
 */
#include "syscall.h"
#include "stdio.h"

#define N 64

/* The shared cap in src/sys.cpp (256 MiB), the value $F4/$F5 already used. */
#define MAX_BULK_BYTES (1UL << 28)

/* A base so near the top of the address space that base + 0x200 wraps u64. */
#define WRAP_PTR ((void *)0xFFFFFFFFFFFFFF00UL)
#define WRAP_LEN 0x200UL

/* An fd that was never opened. The bounds check runs ahead of fd validation. */
#define BAD_FD 99

/* [-4095,-1] result band: (unsigned long)r > (unsigned long)-4096. */
#define IN_ERRBAND(r) ((unsigned long)(r) > 0xFFFFFFFFFFFFF000UL)

static unsigned char buf[N];

static int
sentinel_intact(void)
{
    int i;
    for (i = 0; i < N; ++i) {
        if (buf[i] != 0xA5) { return 0; }
    }
    return 1;
}

int
main(void)
{
    int i;
    long rv;

    for (i = 0; i < N; ++i) { buf[i] = 0xA5; }

    /* (1) sys_read, well formed. A zero count is the one legitimate read this
       fixture can make without blocking on host stdin; it must stay a benign
       no-op returning 0, not a rejection. */
    rv = sys_read(0, buf, 0UL);
    if (rv != 0)        { puts("rw-bounds: FAIL read-zero-ret");   return 1; }
    if (!sentinel_intact()) { puts("rw-bounds: FAIL read-zero-write"); return 1; }

    /* (2) sys_read, oversized count. Must be rejected with -EINVAL before the
       host buffer is sized, leaving the destination untouched. */
    rv = sys_read(0, buf, MAX_BULK_BYTES + 1UL);
    if (!IN_ERRBAND(rv)) { puts("rw-bounds: FAIL read-oversize-band");  return 1; }
    if (rv != -22)       { puts("rw-bounds: FAIL read-oversize-errno"); return 1; }
    if (!sentinel_intact()) { puts("rw-bounds: FAIL read-oversize-write"); return 1; }

    /* (3) sys_read, wrapping destination. address + count wraps u64, so no
       write may be attempted anywhere in that bogus range. */
    rv = sys_read(0, WRAP_PTR, WRAP_LEN);
    if (!IN_ERRBAND(rv)) { puts("rw-bounds: FAIL read-wrap-band");  return 1; }
    if (rv != -14)       { puts("rw-bounds: FAIL read-wrap-errno"); return 1; }

    /* (4) sys_write, well formed. fd 2 keeps this off the stdout stream the
       harness byte-compares. A small legitimate write must still succeed. */
    rv = sys_write(2, "x", 1UL);
    if (rv != 1)         { puts("rw-bounds: FAIL write-small-ret");  return 1; }
    rv = sys_write(1, buf, 0UL);
    if (rv != 0)         { puts("rw-bounds: FAIL write-zero-ret");   return 1; }

    /* (5) sys_write, oversized count. Rejected before mm.read materializes the
       source range into a host buffer, so nothing reaches stdout either. */
    rv = sys_write(1, buf, MAX_BULK_BYTES + 1UL);
    if (!IN_ERRBAND(rv)) { puts("rw-bounds: FAIL write-oversize-band");  return 1; }
    if (rv != -22)       { puts("rw-bounds: FAIL write-oversize-errno"); return 1; }

    /* (6) sys_write, wrapping source. No read of the bogus range may happen. */
    rv = sys_write(1, WRAP_PTR, WRAP_LEN);
    if (!IN_ERRBAND(rv)) { puts("rw-bounds: FAIL write-wrap-band");  return 1; }
    if (rv != -14)       { puts("rw-bounds: FAIL write-wrap-errno"); return 1; }

    /* (7) sys_getdents64, well formed. A small count on a never-opened fd must
       reach the fd lookup and fail there (-EBADF), NOT be turned away by the
       new cap or wrap check. */
    rv = sys_getdents64(BAD_FD, buf, (unsigned long)N);
    if (!IN_ERRBAND(rv)) { puts("rw-bounds: FAIL getdents-small-band");  return 1; }
    if (rv == -22 || rv == -14) { puts("rw-bounds: FAIL getdents-small-errno"); return 1; }
    if (!sentinel_intact()) { puts("rw-bounds: FAIL getdents-small-write"); return 1; }

    /* (8) sys_getdents64, oversized count, rejected before the resize. */
    rv = sys_getdents64(BAD_FD, buf, MAX_BULK_BYTES + 1UL);
    if (!IN_ERRBAND(rv)) { puts("rw-bounds: FAIL getdents-oversize-band");  return 1; }
    if (rv != -22)       { puts("rw-bounds: FAIL getdents-oversize-errno"); return 1; }
    if (!sentinel_intact()) { puts("rw-bounds: FAIL getdents-oversize-write"); return 1; }

    /* (9) sys_getdents64, wrapping destination. */
    rv = sys_getdents64(BAD_FD, WRAP_PTR, WRAP_LEN);
    if (!IN_ERRBAND(rv)) { puts("rw-bounds: FAIL getdents-wrap-band");  return 1; }
    if (rv != -14)       { puts("rw-bounds: FAIL getdents-wrap-errno"); return 1; }

    /* Reaching here proves the VM survived every bad call (no uncaught host
       allocation failure) and that no rejection touched guest memory. */
    puts("rw-bounds: PASS");
    return 0;
}

/* wave2_stdin_reuse.c -- maize-292 cycle-2 fixture, run UNDER quesOS.
 *
 * Code-review push-back (cycle 1): toolchain/rt/stdio.c's fclose() unconditionally
 * did `free(stream->buf); free(stream);`. That was a safe no-op while stdin had no
 * real backing buffer, but this card gave stdin one (_stdin_buf, a static array,
 * backing the static _stdin FILE object), so fclose(stdin) / fshut(stdin, ...) (which
 * every stdin-consuming Group-B tool calls unconditionally on a normal run) handed
 * two non-heap pointers to the RT allocator's free-list (stdlib.c's free_insert),
 * corrupting static memory and, on a later malloc(), the heap.
 *
 * wave2_stdin_pipe.c already proves the stdin FIX works for its own purpose (real
 * bytes flow through oksh's pipeline into uniq/md5sum). It does NOT catch this
 * corruption, because every process it drives exits immediately after its own
 * fshut(stdin, ...), before any corrupted free-list entry is ever touched again.
 *
 * This fixture closes that gap: in ONE process, it (1) actually reads real piped
 * bytes through stdin's buffered path (fread, the same path fshut's callers use),
 * (2) calls fclose(stdin) (the exact corruption trigger), THEN (3) reads the
 * memory back two ways:
 *
 *   a) The deterministic check. stdlib.c's free()/free_insert writes the
 *      allocator's own bookkeeping into the freed block: free_insert's
 *      `b[1] = (word)cur` lands exactly on the first HDR (8) bytes of whatever
 *      was freed, since b == ptr - HDR. For free(stream) with stream == &_stdin,
 *      that write lands on _stdin's own leading fields (fd, then flags), so an
 *      unguarded fclose(stdin) always clobbers stdin->fd/flags with free-list
 *      linkage, layout-independent of whatever else sits in .bss around it. A
 *      guarded fclose (this fix) never frees the static stream, so those fields
 *      survive untouched. This is read-after-free by design, to observe exactly
 *      the corruption the guard exists to prevent.
 *   b) The general heap-reuse check the review asked for: allocate a batch of
 *      differently-sized heap blocks, stamp each with a distinct byte pattern
 *      immediately after allocation, then verify every one at the end. Any
 *      overlap (a corrupted free-list handing back an aliased block) shows up
 *      as a mismatched byte here.
 *
 * No /bin mount is required: this exercises toolchain/rt directly, not an sbase
 * tool.
 */

#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"   /* _exit */

int printf(const char *, ...);

#define NALLOC 16

int main(void) {
    int p[2];
    static const char msg[] = "hello-stdin-reuse\npayload\n";
    char rbuf[64];
    size_t got;
    int i;
    int fd_before, flags_before;
    unsigned char *blocks[NALLOC];
    static const size_t sizes[NALLOC] = {
        24, 40, 8, 128, 16, 64, 32, 256, 48, 96, 16, 200, 8, 72, 120, 40
    };

    if (pipe(p) != 0) {
        printf("wave2-stdin-reuse: FAIL pipe\n");
        return 1;
    }
    if (write(p[1], msg, strlen(msg)) != (long)strlen(msg)) {
        printf("wave2-stdin-reuse: FAIL pipe-write\n");
        return 1;
    }
    close(p[1]);
    dup2(p[0], STDIN_FILENO);
    close(p[0]);

    /* Drive the real buffered-read path (fill_rbuf), the same path every
     * stdin-consuming sbase tool takes via fread/getc(stdin). */
    got = fread(rbuf, 1, sizeof(rbuf) - 1, stdin);
    rbuf[got] = 0;
    if (got == 0 || strcmp(rbuf, msg) != 0) {
        printf("wave2-stdin-reuse: FAIL stdin-readback got=%lu buf=[%s]\n",
               (unsigned long)got, rbuf);
        return 1;
    }

    fd_before = stdin->fd;
    flags_before = stdin->flags;

    /* The exact corruption trigger: fclose(stdin), matching fshut(stdin, "<stdin>")
     * unconditionally called by every Group-B tool on a normal run. */
    if (fclose(stdin) != 0) {
        printf("wave2-stdin-reuse: FAIL fclose-stdin\n");
        return 1;
    }

    /* Deterministic corruption check (a, above): an unguarded fclose stomps
     * stdin's own leading fields with free-list linkage. */
    if (stdin->fd != fd_before || stdin->flags != flags_before) {
        printf("wave2-stdin-reuse: FAIL stdin-object-corrupted fd=%d(expect %d) "
               "flags=%d(expect %d)\n", stdin->fd, fd_before, stdin->flags, flags_before);
        return 1;
    }

    /* General heap-reuse check (b, above): stamp then verify a batch of blocks. */
    for (i = 0; i < NALLOC; i++) {
        blocks[i] = malloc(sizes[i]);
        if (blocks[i] == NULL) {
            printf("wave2-stdin-reuse: FAIL malloc[%d] size=%lu\n",
                   i, (unsigned long)sizes[i]);
            return 1;
        }
        memset(blocks[i], 0x40 + i, sizes[i]);
    }
    for (i = 0; i < NALLOC; i++) {
        size_t j;
        for (j = 0; j < sizes[i]; j++) {
            if (blocks[i][j] != (unsigned char)(0x40 + i)) {
                printf("wave2-stdin-reuse: FAIL corruption block=%d offset=%lu "
                       "expected=0x%x got=0x%x\n",
                       i, (unsigned long)j, 0x40 + i, blocks[i][j]);
                return 1;
            }
        }
        free(blocks[i]);
    }

    printf("wave2-stdin-reuse: PASS\n");
    return 0;
}

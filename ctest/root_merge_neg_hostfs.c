/* maize-255 REOPEN regression: the merged mount-name listing must appear ONLY in
 * the root "/" listing, never in any deeper directory that merely resolves through
 * the "/" root mount. The original fix gated merge_root on the matched mount's
 * identity ("/" mount) alone; because that mount is match_mount's fallback and wins
 * for every path no deeper mount claims (/home/user, /tmp, /home/user/doom, ...),
 * every such listing wrongly appended the synthetic mount names. A subdirectory that
 * held a physical dir named like a granted mount then showed it twice: the operator's
 * live "double doom" (ls /home/user -> "... doom doom ...").
 *
 * This fixture reproduces the operator's exact shape: a --root sandbox with a physical
 * /home/user/doom subdir, plus /bin and /doom OVERLAY grants (the two names that were
 * bleeding in). It lists several directories, each with a distinct line prefix, so the
 * harness can assert per-directory:
 *   ROOT: (open "/")           -> merges: {bin, doom, home, tmp}   (positive path)
 *   SUB:  (open "/home/user")  -> physical only: {doom, sub_marker.txt}
 *                                 doom appears EXACTLY ONCE (physical subdir, no
 *                                 synthetic clone); NO bin.
 *   TMP:  (open "/tmp")        -> physical only: {tmp_marker.txt}; NO bin/doom.
 *   DOOM: (open "/doom")       -> the /doom overlay mount root: {doom_marker.txt};
 *                                 NO synthetic bin/doom (the /bin-analog: an overlay
 *                                 mount root's own listing carries no merge names).
 *   BIN:  (open "/bin")        -> the /bin overlay mount root: {bin_marker.txt};
 *                                 NO synthetic names.
 * Byte indexing only (element size 1), so the address math never multiplies. */
#include "syscall.h"

static void write_str(const char *s) {
    unsigned long len = 0;
    while (s[len] != 0) {
        ++len;
    }
    sys_write(1, s, len);
}

/* Open path, drain its full getdents stream with a small buffer (forcing multiple
   calls, so a mount-name phase that leaked into a deeper listing would still be
   caught across the phase boundary), print each d_name with the given prefix. */
static void list_dir(const char *path, const char *prefix) {
    long fd = sys_open(path, 0x10000 /* O_DIRECTORY */, 0);
    if (fd < 0) {
        write_str(prefix);
        write_str("OPENFAIL\n");
        return;
    }
    char buf[64];
    for (;;) {
        long n = sys_getdents64((int)fd, buf, (unsigned long)sizeof buf);
        if (n <= 0) {
            break;
        }
        long pos = 0;
        while (pos < n) {
            unsigned char *rec = (unsigned char *)buf + pos;
            unsigned short reclen = (unsigned short)(rec[16] | (rec[17] << 8));
            char *name = (char *)rec + 19;
            unsigned long len = 0;
            while (name[len] != 0) {
                ++len;
            }
            write_str(prefix);
            sys_write(1, name, len);
            sys_write(1, "\n", 1);
            pos += reclen;
        }
    }
    sys_close((int)fd);
}

int main(void) {
    list_dir("/",           "ROOT:");
    list_dir("/home/user",  "SUB:");
    list_dir("/tmp",        "TMP:");
    list_dir("/doom",       "DOOM:");
    list_dir("/bin",        "BIN:");
    return 0;
}

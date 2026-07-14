/* maize-151 hostfs acceptance: path-mutating syscalls under the default sandbox root.
 *
 * Exercises the DOOM save shape end-to-end against a writable filesystem, using RELATIVE
 * paths so they resolve against the startup cwd (/home/user) the sandbox root installs,
 * exactly as DOOM's `mkdir ./.savegame; write temp.dsg; rename temp.dsg -> slot` does:
 *   1. mkdir a new directory under the cwd
 *   2. create + write a file inside it
 *   3. rename that file to the save-slot name
 *   4. the old name is gone; the new name stats to the written size and reads back
 *   5. unlink the slot; it is then gone
 * Each step must succeed (or, for the two "is it gone" probes, fail with the file
 * absent). Prints a single PASS/FAIL line. The harness grants a fresh --root sandbox. */
#include "syscall.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40

int main(void) {
    int ok = 1;

    /* 1. mkdir ./savedir under the cwd. */
    if (sys_mkdir("./savedir", 0755) < 0) {
        ok = 0;
    }

    /* 2. create + write a file in it. */
    static const char payload[] = "savegame-bytes";
    long fd = sys_open("./savedir/temp.dsg", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        ok = 0;
    } else {
        if (sys_write((int)fd, payload, 14) != 14) {
            ok = 0;
        }
        sys_close((int)fd);
    }

    /* 3. rename temp.dsg -> slot0.dsg. */
    if (sys_rename("./savedir/temp.dsg", "./savedir/slot0.dsg") < 0) {
        ok = 0;
    }

    /* 4a. the old name is gone. */
    long gone = sys_open("./savedir/temp.dsg", O_RDONLY, 0);
    if (gone >= 0) {
        ok = 0;
        sys_close((int)gone);
    }

    /* 4b. the new name stats to size 14 and reads back its bytes. */
    long sfd = sys_open("./savedir/slot0.dsg", O_RDONLY, 0);
    if (sfd < 0) {
        ok = 0;
    } else {
        unsigned char st[144];
        if (sys_fstat((int)sfd, st) < 0) {
            ok = 0;
        } else {
            unsigned long size = 0;
            for (int i = 55; i >= 48; --i) {
                size = (size << 8) | (unsigned long)st[i];
            }
            if (size != 14UL) {
                ok = 0;
            }
        }
        char buf[16];
        long rd = sys_read((int)sfd, buf, 14);
        if (rd != 14) {
            ok = 0;
        } else {
            for (int i = 0; i < 14; ++i) {
                if (buf[i] != payload[i]) {
                    ok = 0;
                }
            }
        }
        sys_close((int)sfd);
    }

    /* 5. unlink the slot; it is then gone. */
    if (sys_unlink("./savedir/slot0.dsg") < 0) {
        ok = 0;
    }
    long gone2 = sys_open("./savedir/slot0.dsg", O_RDONLY, 0);
    if (gone2 >= 0) {
        ok = 0;
        sys_close((int)gone2);
    }

    sys_write(1, ok ? "savefs: PASS\n" : "savefs: FAIL\n", 13);
    return 0;
}

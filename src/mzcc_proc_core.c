/* mzcc_proc_core.c (maize-278): platform-independent proc helpers. The growable
   ByteBuf used for every in-flight byte buffer (stdin feed, captured
   stdout/stderr, CRLF-strip, normalize) lives here; the OS-specific spawn/pipe
   backends are mzcc_proc_posix.c / mzcc_proc_win32.c. Mirrors the
   presenter_transport_core.cpp split. */
#include "mzcc_proc.h"

#include <stdlib.h>
#include <string.h>

void byte_buf_init(ByteBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int byte_buf_append(ByteBuf *b, const char *bytes, size_t n) {
    if (n == 0) {
        return 0;
    }
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < b->len + n) {
            ncap *= 2;
        }
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) {
            return -1;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    return 0;
}

void byte_buf_free(ByteBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

#include "all.h"

/* Maize data emission (maize-62, hello-world slice).
 *
 * QBE data items are lowered to mazm as a labelled `DATA` byte list (one label
 * per symbol). String literals are decoded from QBE's gas-style escaped form to
 * raw bytes and re-emitted as byte values, which sidesteps any escape-convention
 * mismatch between QBE and mazm's STRING directive. Address references inside
 * initialized data (pointers in data) are not reached by hello world and err()
 * as maize-63 surface.
 */

static void
emitbytes(const uchar *p, int n, FILE *f)
{
	int i;

	if (n <= 0)
		return;
	fputs("\tDATA", f);
	for (i = 0; i < n; i++)
		fprintf(f, " $%02x", p[i]);
	fputc('\n', f);
}

/* Decode a QBE/gas quoted string ("...\NNN...") into raw bytes, writing them as
 * a DATA line. Handles octal escapes (as cproc emits, e.g. "\000") and the
 * common C letter escapes. */
static void
emitstr(char *s, FILE *f)
{
	uchar out[4096];
	int n, v, k;
	char c;

	if (*s == '"')
		s++;
	n = 0;
	while ((c = *s) && c != '"') {
		if (n >= (int)sizeof out - 1) {
			emitbytes(out, n, f);
			n = 0;
		}
		if (c == '\\') {
			s++;
			c = *s;
			if (c >= '0' && c <= '7') {
				v = 0;
				for (k = 0; k < 3 && *s >= '0' && *s <= '7'; k++, s++)
					v = v * 8 + (*s - '0');
				out[n++] = (uchar)v;
				continue;
			}
			switch (c) {
			case 'n': out[n++] = '\n'; break;
			case 't': out[n++] = '\t'; break;
			case 'r': out[n++] = '\r'; break;
			case 'b': out[n++] = '\b'; break;
			case 'f': out[n++] = '\f'; break;
			case 'a': out[n++] = '\a'; break;
			case 'v': out[n++] = '\v'; break;
			case '\\': out[n++] = '\\'; break;
			case '"': out[n++] = '"'; break;
			case '\'': out[n++] = '\''; break;
			case 0: goto done;
			default: out[n++] = (uchar)c; break;
			}
			s++;
			continue;
		}
		out[n++] = (uchar)c;
		s++;
	}
done:
	emitbytes(out, n, f);
}

static void
emitnum(int width, int64_t v, FILE *f)
{
	uchar b[8];
	int i;

	for (i = 0; i < width; i++)
		b[i] = (uchar)(v >> (8 * i));
	emitbytes(b, width, f);
}

void
maize_emitdat(Dat *d, FILE *f)
{
	switch (d->type) {
	case DStart:
	case DEnd:
		break;
	case DAlign:
		/* mazm assembles one flat origin-based image with no alignment
		 * directive; data lands where it is concatenated. Alignment is a
		 * no-op here (documented in the coverage note). */
		break;
	case DName:
		fprintf(f, "%s:\n", maize_sym(d->u.str));
		break;
	case DB:
		if (d->isstr)
			emitstr(d->u.str, f);
		else if (d->isref)
			die("maize data: address references in data are not supported (maize-63)");
		else
			emitnum(1, d->u.num, f);
		break;
	case DH:
		if (d->isref)
			die("maize data: address references in data are not supported (maize-63)");
		emitnum(2, d->u.num, f);
		break;
	case DW:
		if (d->isref)
			die("maize data: address references in data are not supported (maize-63)");
		emitnum(4, d->u.num, f);
		break;
	case DL:
		if (d->isref)
			die("maize data: address references in data are not supported (maize-63)");
		emitnum(8, d->u.num, f);
		break;
	case DZ:
		{
			uchar z[256];
			int64_t left = d->u.num;
			memset(z, 0, sizeof z);
			while (left > 0) {
				int chunk = left < (int64_t)sizeof z ? (int)left : (int)sizeof z;
				emitbytes(z, chunk, f);
				left -= chunk;
			}
		}
		break;
	}
}

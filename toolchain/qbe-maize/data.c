#include "all.h"

/* Maize data emission (maize-77, segmented .mzx pipeline).
 *
 * QBE hands one data object as the call sequence DStart, optional DAlign, DName,
 * the items (DB/DH/DW/DL/DZ), DEnd. This emitter is a deferred section-routing
 * state machine modelled on toolchain/qbe/gas.c:22-90: it accumulates leading
 * zeros without committing to a section, then, on the first non-zero item OR at
 * DEnd-while-still-all-zeros, opens the right section and emits the label plus
 * body. It emits the object-mode mazm directives SECTION / GLOBAL / ALIGN / DREF
 * (delivered by maize-89), which are inert no-ops in mazm's flat mode, so the same
 * text assembles either way.
 *
 * Routing (OQ-e, decision recorded):
 *   - a WHOLLY-zero object            -> SECTION BSS + ZERO N   (NOBITS, zero-at-load);
 *   - a `.L`-prefixed compiler object -> SECTION RODATA         (string literals, tables);
 *   - a named object                  -> SECTION DATA           (mutable file-scope globals);
 *   - an explicit DStart section hint -> honored if present     (forward-compat).
 * A partially-initialized aggregate (int a[8]={1};) stays in DATA with its hole /
 * trailing zeros emitted as real bytes (decision 7166), matching C's data-vs-bss
 * rule.
 *
 * The numeric (emitnum), string (emitstr) and zero-fill emitters are the unchanged
 * maize-62 code; only their call sites and the surrounding framing are new.
 * Address references in initialized data (QBE `l $sym[+off]`) lower to maize-89's
 * `DREF <bytes> <sym>[+off]` directive (R_MAIZE_ABS64/ABS32), replacing the old
 * die().
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

/* Emit `count` real zero bytes as DATA lines (used inside an open DATA/RODATA
 * section for a partially-initialized aggregate's holes and trailing zeros; NOT
 * the NOBITS path). */
static void
emitzeros(int64_t count, FILE *f)
{
	uchar z[256];

	memset(z, 0, sizeof z);
	while (count > 0) {
		int chunk = count < (int64_t)sizeof z ? (int)count : (int)sizeof z;
		emitbytes(z, chunk, f);
		count -= chunk;
	}
}

/* Per-object deferred state (reset at each DStart). */
static char   *cur_section;   /* DStart section hint (usually NULL under pinned cproc) */
static char   *cur_name;      /* object symbol name (retains the `.L` prefix, if any)  */
static int     cur_export;    /* object is exported -> GLOBAL                          */
static int     cur_align;     /* DAlign value (power of two), 1 = none                 */
static int64_t cur_zero;      /* leading zero bytes accumulated before any real item   */
static int     cur_opened;    /* a DATA/RODATA section header + label already emitted   */

/* Route a non-zero object to RODATA vs DATA. An explicit DStart hint wins (forward-
 * compat; never fires under the pinned cproc). Otherwise a `.L`-prefixed compiler-
 * internal object (string literals, switch tables) goes to RODATA and a named
 * object (a mutable file-scope global) goes to DATA. */
static const char *
data_section_kind(void)
{
	if (cur_section) {
		if (strstr(cur_section, "rodata"))
			return "RODATA";
		if (strstr(cur_section, "bss"))
			return "BSS";
		return "DATA";
	}
	if (cur_name && cur_name[0] == '.' && cur_name[1] == 'L')
		return "RODATA";
	return "DATA";
}

/* Emit the SECTION/ALIGN/GLOBAL/label preamble shared by the BSS and DATA/RODATA
 * openers. */
static void
emit_preamble(const char *kind, FILE *f)
{
	fprintf(f, "SECTION %s\n", kind);
	if (cur_align > 1)
		fprintf(f, "ALIGN %d\n", cur_align);
	if (cur_export)
		fprintf(f, "GLOBAL %s\n", maize_sym(cur_name));
	fprintf(f, "%s:\n", maize_sym(cur_name));
}

/* Open the object as an initialized DATA/RODATA section: emit the preamble, flush
 * any accumulated leading zeros as real bytes, and mark the object opened. */
static void
open_data(FILE *f)
{
	emit_preamble(data_section_kind(), f);
	if (cur_zero > 0)
		emitzeros(cur_zero, f);
	cur_zero = 0;
	cur_opened = 1;
}

void
maize_emitdat(Dat *d, FILE *f)
{
	switch (d->type) {
	case DStart:
		cur_section = d->u.str;
		cur_name = 0;
		cur_export = 0;
		cur_align = 1;
		cur_zero = 0;
		cur_opened = 0;
		break;
	case DEnd:
		if (!cur_opened) {
			/* Reached the end still all-zero (or empty): a NOBITS BSS
			 * contribution, zero-filled at load. */
			emit_preamble("BSS", f);
			fprintf(f, "\tZERO $%"PRIx64"\n", (uint64_t)cur_zero);
		}
		break;
	case DAlign:
		cur_align = (int)d->u.num;
		break;
	case DName:
		cur_name = d->u.str;
		cur_export = d->export;
		break;
	case DZ:
		if (!cur_opened)
			cur_zero += d->u.num;   /* still all-zero: accumulate */
		else
			emitzeros(d->u.num, f); /* real zeros inside an open aggregate */
		break;
	case DB:
		if (!cur_opened)
			open_data(f);
		if (d->isstr)
			emitstr(d->u.str, f);
		else if (d->isref)
			die("maize data: byte-width pointer-in-data is not representable");
		else
			emitnum(1, d->u.num, f);
		break;
	case DH:
		if (!cur_opened)
			open_data(f);
		if (d->isref)
			die("maize data: half-width pointer-in-data is not representable");
		emitnum(2, d->u.num, f);
		break;
	case DW:
		if (!cur_opened)
			open_data(f);
		if (d->isref) {
			if (d->u.ref.off)
				fprintf(f, "\tDREF 4 %s%+"PRId64"\n",
					maize_sym(d->u.ref.nam), d->u.ref.off);
			else
				fprintf(f, "\tDREF 4 %s\n", maize_sym(d->u.ref.nam));
		} else
			emitnum(4, d->u.num, f);
		break;
	case DL:
		if (!cur_opened)
			open_data(f);
		if (d->isref) {
			if (d->u.ref.off)
				fprintf(f, "\tDREF 8 %s%+"PRId64"\n",
					maize_sym(d->u.ref.nam), d->u.ref.off);
			else
				fprintf(f, "\tDREF 8 %s\n", maize_sym(d->u.ref.nam));
		} else
			emitnum(8, d->u.num, f);
		break;
	}
}

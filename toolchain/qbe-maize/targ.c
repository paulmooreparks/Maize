#include "all.h"

/* Caller-saved (volatile) allocatable registers: R0..R5 and RV.
 * Callee-saved (non-volatile) allocatable registers: R6..R9.
 * RT/RB/RS are globally reserved (rglob) and never allocated. */
int maize_rsave[] = {
	R0, R1, R2, R3, R4, R5, RV,
	-1
};
int maize_rclob[] = {
	R6, R7, R8, R9,
	-1
};

#define RGLOB (BIT(RT) | BIT(RB) | BIT(RS))

static int
maize_memargs(int op)
{
	(void)op;
	return 0;
}

Target T_maize = {
	.gpr0 = R0,
	.ngpr = NGPR,
	.fpr0 = RS + 1,   /* empty FP file: [fpr0, fpr0+0) */
	.nfpr = 0,
	.rglob = RGLOB,
	.nrglob = 3,
	.rsave = maize_rsave,
	.nrsave = {NGPS, NFPS},
	.retregs = maize_retregs,
	.argregs = maize_argregs,
	.memargs = maize_memargs,
	.abi = maize_abi,
	.isel = maize_isel,
	.emitfn = maize_emitfn,
	.emitdat = maize_emitdat,
};

MAKESURE(globals_are_not_arguments,
	(RGLOB & (BIT(RV+1) - 1)) == 0
);
MAKESURE(rsave_size_ok,
	sizeof maize_rsave == (NGPS + NFPS + 1) * sizeof(int)
);
MAKESURE(rclob_size_ok,
	sizeof maize_rclob == (NCLR + 1) * sizeof(int)
);

/* True when `name`, with any leading underscores stripped and folded to upper
 * case, is a mazm register mnemonic. mazm's is_register() matches operand tokens
 * against the register table case-INSENSITIVELY (mazm.cpp), so a bare emitted
 * symbol that folds to a register name (e.g. a C global `bp`, `sp`, `r0`, `fl`)
 * is assembled as that register instead of as the symbol's address, silently
 * miscompiling `&bp[i]` into `BP + i*8` (maize-193). The mnemonics are the 16
 * reg_map entries (R0..R9, RT, RV, RF, RB, RP, RS) plus the four aliases SP, BP,
 * PC, FL. Leading underscores are stripped first so the escape below stays
 * injective: `bp` and `_bp` must not both fold to the same escaped form. */
static int
is_reg_mnemonic(const char *name)
{
	static const char *regs[] = {
		"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9",
		"RT", "RV", "RF", "RB", "RP", "RS", "SP", "BP", "PC", "FL",
	};
	const char *p = name;
	size_t n, k;

	while (*p == '_')
		p++;
	n = sizeof regs / sizeof regs[0];
	for (k = 0; k < n; k++) {
		const char *a = p, *b = regs[k];
		while (*a && *b) {
			char ca = *a;
			if (ca >= 'a' && ca <= 'z')
				ca = (char)(ca - 'a' + 'A');
			if (ca != *b)
				break;
			a++, b++;
		}
		if (!*a && !*b)
			return 1;
	}
	return 0;
}

/* Translate a QBE symbol name into a mazm-legal label. QBE local symbols carry
 * leading dots and embedded dots (e.g. ".Lstring.2"); mazm labels are the same
 * identifier shape as C (letters, digits, underscore). Map every non-identifier
 * character to '_' and guard a leading digit. The mapping is deterministic and
 * shared by the data emitter (label definitions) and the back-end (address
 * materialization), so a definition and its references always agree.
 *
 * Register-collision escape (maize-193): if the mapped label folds to a mazm
 * register mnemonic, prepend one '_'. mazm resolves a bare operand token to a
 * register whenever it case-insensitively matches the register table, so an
 * un-escaped `bp` / `sp` / `r0` / `fl` global would assemble as the register,
 * not the symbol. Prepending '_' makes the token a non-register while staying a
 * legal label; the leading-underscore strip in is_reg_mnemonic() keeps the
 * escape injective (a pre-existing `_bp` becomes `__bp`, distinct from `bp`'s
 * `_bp`), and the transform is applied identically at every definition and
 * reference so they still agree. */
char *
maize_sym(char *s)
{
	static char buf[NString + 1];
	int i;
	char c;

	i = 0;
	if (s && (*s >= '0' && *s <= '9'))
		buf[i++] = '_';
	for (; s && *s && i < NString; s++) {
		c = *s;
		if ((c >= 'A' && c <= 'Z')
		|| (c >= 'a' && c <= 'z')
		|| (c >= '0' && c <= '9')
		|| c == '_')
			buf[i++] = c;
		else
			buf[i++] = '_';
	}
	buf[i] = 0;

	if (is_reg_mnemonic(buf) && i < NString) {
		memmove(buf + 1, buf, (size_t)i + 1);
		buf[0] = '_';
	}
	return buf;
}

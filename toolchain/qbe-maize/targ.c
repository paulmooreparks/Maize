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

/* Translate a QBE symbol name into a mazm-legal label. QBE local symbols carry
 * leading dots and embedded dots (e.g. ".Lstring.2"); mazm labels are the same
 * identifier shape as C (letters, digits, underscore). Map every non-identifier
 * character to '_' and guard a leading digit. The mapping is deterministic and
 * shared by the data emitter (label definitions) and the back-end (address
 * materialization), so a definition and its references always agree. */
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
	return buf;
}

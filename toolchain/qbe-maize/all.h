#include "../all.h"

/* Maize back-end target for QBE (maize-62, wave 2 of maize-11).
 *
 * Register file, in QBE-internal numbering. Every value here must stay below
 * Tmp0 (== NBit == 64) so it can share the Ref/BSet register space. The layout
 * mirrors the Maize C ABI register partition (maize-11 decision 6416, finalized
 * in this card): R0..R9 pass integer/pointer arguments and are generally
 * allocatable, RV carries the return value, RT is a fixed back-end scratch, and
 * RB/RS are the frame and stack pointers.
 */
enum MaizeReg {
	R0 = RXX + 1,
	R1, R2, R3, R4, R5, R6, R7, R8, R9,
	RV,             /* return value; caller-saved; allocatable */
	RT,             /* back-end scratch; not RA-allocatable (rglob) */
	RB,             /* frame pointer (mazm alias BP); callee-saved (rglob) */
	RS,             /* stack pointer (mazm alias SP); fixed (rglob) */

	NGPR = RS - R0 + 1,             /* full GP range R0..RS = 14 */
	NGPS = (R5 - R0 + 1) + 1,       /* GP caller-saved: R0..R5 + RV = 7 */
	NFPS = 0,                       /* no FP registers this target */
	NCLR = (R9 - R6 + 1),           /* GP callee-saved: R6..R9 = 4 */
};
MAKESURE(reg_not_tmp, RS < (int)Tmp0);

/* targ.c */
extern int maize_rsave[];
extern int maize_rclob[];
char *maize_sym(char *);

/* abi.c */
bits maize_retregs(Ref, int[2]);
bits maize_argregs(Ref, int[2]);
void maize_abi(Fn *);

/* isel.c */
void maize_isel(Fn *);

/* emit.c */
void maize_emitfn(Fn *, FILE *);

/* data.c */
void maize_emitdat(Dat *, FILE *);

/* maize-143: global-symbol + constant-offset address constants (CAddr offset).
 *
 * QBE folds a global-symbol address plus a compile-time byte constant into a
 * single address constant ($sym + K, a Con of type CAddr with bits.i == K). This
 * is what &global_array[K] (const K), &s.field, and "lit" + K fold to, and it is
 * pervasive in real C (static tables, struct-field addressing, the endptr ==
 * base + N idiom from maize-142/strtol). Before this card the qbe -t maize
 * emitter die()d the moment such a Con carried a nonzero offset; the fix routes
 * every nonzero-offset CAddr through a register in isel and materializes it in
 * emitcopy as CP <label> ; LEA $<off> (a FLAG-NEUTRAL add, never ADD/SUB).
 *
 * This fixture FORMS and USES each folded-offset pattern with a CHECKED result,
 * folding into a single "caddroff: PASS" stdout line, so a dropped or wrong
 * offset changes stdout and fails the exact-stdout gate:
 *   (a) &garr[K] (K != 0): as a load address, as an inline ALU source, and as a
 *       folded-con compare operand (the endptr == base + N idiom);
 *   (b) &s.field: a static struct field address at a nonzero field offset;
 *   (c) "lit" + K: a string-literal address folded with a nonzero constant,
 *       materialized and printed on the PASS path;
 *   (d) &garr[K] carried as a loop-carried pointer across a fused conditional
 *       branch (modelled on ctest/addrlocalphi.c), so a wrong offset changes the
 *       loop sum. The flag-neutrality of the LEA lowering (a flag-clobbering
 *       ADD/SUB would corrupt a fused exit test) is exercised at the QBE-IR level
 *       by ctest/caddroff_flag.qbe, which cproc cannot express from C (it keeps
 *       locals in memory, so a bare CAddr-con phi argument never arises); see the
 *       run_qbe_flag runner in scripts/run-ctest.sh.
 *
 * Distinct element values make a wrong element offset (off-by-elt-size or
 * dropped-to-zero) change a checked sub-result. Cross-checked against the host
 * compiler, per the spill.c / ptrdata.c precedent.
 */

int puts(const char *);

/* (a) global array; distinct values so a wrong element offset shifts a result. */
static int garr[8] = { 0, 100, 200, 300, 400, 500, 600, 700 };
/* (b) static struct; nonzero field offsets (x@0, y@4, z@8). */
static struct { int x; int y; int z; } srec = { 11, 22, 33 };

/* Non-inlined optimization barrier: cproc/qbe do no cross-function inlining, so a
 * value returned through opaque() cannot be folded to a compile-time constant.
 * Used to build a runtime pointer that the folded-con compares cannot be proven
 * equal to at compile time. */
static int opaque(int x) { return x; }

/* (d) loop-carried &garr[K] (K != 0) walked across a fused conditional branch. p
 * starts at &garr[2] and is reset to &garr[6] each pass; the do-while latch fuses
 * into a conditional branch. A wrong offset changes the sum. */
static int fused_phi(int n) {
	int *p = &garr[2];
	int i = 0, sum = 0;
	do { sum += *p; p = &garr[6]; i++; } while (i < n);
	return sum;                          /* 200 + 600 * (n - 1) */
}

int
main(void)
{
	int ok = 1;

	/* (a) &garr[K], K != 0, as a load address. */
	int *pa = &garr[5];
	if (*pa != 500) ok = 0;

	/* (a) folded-con compare operand: a runtime-walked pointer compared to the
	 * folded $garr + 20 (endptr == base + N idiom). The walk is opaque so QBE
	 * cannot prove the equality at compile time and fold the compare away. */
	int *walk = garr + opaque(5);
	if (walk != &garr[5]) ok = 0;        /* folded $garr+20 as a CMP source */
	if (walk == &garr[4]) ok = 0;        /* folded $garr+16, must differ */

	/* (a) inline ALU source: the folded &garr[3] added to a runtime index. */
	int idx = opaque(2);
	int *pb = &garr[3] + idx;            /* $garr+12 as an inline add source */
	if (*pb != 500) ok = 0;

	/* (b) &s.field, nonzero field offset, as a load address and a compare. */
	int *pz = &srec.z;
	if (*pz != 33) ok = 0;
	int *zw = (int *)((char *)&srec + opaque(8));
	if (zw != &srec.z) ok = 0;           /* folded $srec+8 as a CMP source */

	/* (d) flag-safety loop: 200 + 600 * 4 == 2600 for n == 5. */
	if (fused_phi(5) != 2600) ok = 0;

	/* (c) string literal + nonzero constant, materialized and USED on the PASS
	 * path: "XXcaddroff: PASS" + 2 prints exactly "caddroff: PASS". A wrong
	 * offset prints the wrong bytes and fails the exact-stdout gate. */
	if (ok)
		puts("XXcaddroff: PASS" + 2);
	else
		puts("caddroff: FAIL");
	return 0;
}

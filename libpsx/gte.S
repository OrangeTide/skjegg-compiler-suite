/* gte.S - libpsx GTE (Geometry Transformation Engine) helper.
 *
 * The GTE is the PlayStation's coprocessor 2: a fixed-point vector and matrix
 * unit that transforms and projects 3D model vertices into the screen-space 2D
 * coordinates the GPU draws. The MIPS backend never emits coprocessor-2
 * instructions, so the GTE is reached through this small assembly library, the
 * way the exception handler reaches coprocessor 0. skj-as-mips encodes the
 * coprocessor-2 moves (mtc2/mfc2/ctc2/cfc2) and the GTE command word (cop2).
 *
 * This is the 3D on-ramp: psx_gte_rtps projects one vertex, and a caller builds
 * a model's screen-space quads from the results, then draws them with the same
 * textured/flat primitives the 2D path already uses.
 *
 * Fixed-point formats: the rotation matrix entries are 1.3.12 (1.0 = 4096), the
 * translation and vertices are plain integers, OFX/OFY are 1.15.16, and H is an
 * unsigned integer projection distance. Default `.set reorder' so the assembler
 * fills the delay slots (including the load delay after an mfc2/cfc2).
 */

	.set reorder

	.text
	.globl psx_gte_set_rot
	.globl psx_gte_set_trans
	.globl psx_gte_set_screen
	.globl psx_gte_rtps
	.globl psx_gte_rtpt
	.globl psx_gte_nclip
	.globl psx_gte_avsz3

/* void psx_gte_set_rot(const short *m)
 * m is the row-major 3x3 rotation matrix [R11 R12 R13 R21 R22 R23 R31 R32 R33]
 * in 1.3.12. The GTE packs two entries per control register. */
psx_gte_set_rot:
	lhu	$t0, 0($a0)		/* R11 */
	lhu	$t1, 2($a0)		/* R12 */
	sll	$t1, $t1, 16
	or	$t0, $t0, $t1
	ctc2	$t0, $0			/* cnt0 = R12<<16 | R11 */
	lhu	$t0, 4($a0)		/* R13 */
	lhu	$t1, 6($a0)		/* R21 */
	sll	$t1, $t1, 16
	or	$t0, $t0, $t1
	ctc2	$t0, $1			/* cnt1 = R21<<16 | R13 */
	lhu	$t0, 8($a0)		/* R22 */
	lhu	$t1, 10($a0)		/* R23 */
	sll	$t1, $t1, 16
	or	$t0, $t0, $t1
	ctc2	$t0, $2			/* cnt2 = R23<<16 | R22 */
	lhu	$t0, 12($a0)		/* R31 */
	lhu	$t1, 14($a0)		/* R32 */
	sll	$t1, $t1, 16
	or	$t0, $t0, $t1
	ctc2	$t0, $3			/* cnt3 = R32<<16 | R31 */
	lhu	$t0, 16($a0)		/* R33 */
	ctc2	$t0, $4			/* cnt4 = R33 */
	jr	$ra

/* void psx_gte_set_trans(int tx, int ty, int tz) */
psx_gte_set_trans:
	ctc2	$a0, $5			/* TRX */
	ctc2	$a1, $6			/* TRY */
	ctc2	$a2, $7			/* TRZ */
	jr	$ra

/* void psx_gte_set_screen(int h, int ofx, int ofy)
 * h is the projection distance (how far the eye is from the screen, in the same
 * units as the model); ofx/ofy are the screen-center pixel, shifted into the
 * GTE's 1.15.16 format. Depth cueing (DQA/DQB) is cleared, and the ordering-table
 * averaging scales (ZSF3/ZSF4) are set so psx_gte_avsz3 returns a usable depth. */
psx_gte_set_screen:
	ctc2	$a0, $26		/* H */
	sll	$a1, $a1, 16
	ctc2	$a1, $24		/* OFX */
	sll	$a2, $a2, 16
	ctc2	$a2, $25		/* OFY */
	ctc2	$zero, $27		/* DQA = 0 */
	ctc2	$zero, $28		/* DQB = 0 */
	li	$t0, 0x0155		/* ZSF3 ~ 1/3 (AVSZ3 scale) */
	ctc2	$t0, $29
	li	$t0, 0x0100		/* ZSF4 = 1/4 (AVSZ4 scale) */
	ctc2	$t0, $30
	jr	$ra

/* void psx_gte_rtps(int vx, int vy, int vz, int *out)
 * Transforms and projects one vertex with the matrix, translation, and screen
 * set above. out[0] = screen x, out[1] = screen y, out[2] = depth (SZ3). */
psx_gte_rtps:
	andi	$t0, $a0, 0xffff
	sll	$t1, $a1, 16
	or	$t0, $t0, $t1
	mtc2	$t0, $0			/* VXY0 = VY0<<16 | VX0 */
	mtc2	$a2, $1			/* VZ0 */
	cop2	0x0180001		/* RTPS: rotate, translate, project (sf=1) */
	mfc2	$t0, $14		/* SXY2 = SY<<16 | SX */
	sll	$t1, $t0, 16
	sra	$t1, $t1, 16		/* sign-extend screen x */
	sw	$t1, 0($a3)
	sra	$t2, $t0, 16		/* screen y */
	sw	$t2, 4($a3)
	mfc2	$t3, $19		/* SZ3 (depth) */
	sw	$t3, 8($a3)
	jr	$ra

/* void psx_gte_rtpt(const short *v9, int *out6)
 * Transform and project three vertices at once (one triangle). v9 is nine
 * shorts x0,y0,z0, x1,y1,z1, x2,y2,z2; out6 receives sx0,sy0, sx1,sy1, sx2,sy2.
 * RTPT leaves SXY0..2 and SZ1..3 in the FIFO, so psx_gte_nclip and
 * psx_gte_avsz3 can read them straight after. */
psx_gte_rtpt:
	/* vertex 0 -> VXY0 (dr0), VZ0 (dr1) */
	lh	$t0, 0($a0)
	andi	$t0, $t0, 0xffff
	lh	$t1, 2($a0)
	sll	$t1, $t1, 16
	or	$t0, $t0, $t1
	mtc2	$t0, $0
	lh	$t2, 4($a0)
	mtc2	$t2, $1
	/* vertex 1 -> VXY1 (dr2), VZ1 (dr3) */
	lh	$t0, 6($a0)
	andi	$t0, $t0, 0xffff
	lh	$t1, 8($a0)
	sll	$t1, $t1, 16
	or	$t0, $t0, $t1
	mtc2	$t0, $2
	lh	$t2, 10($a0)
	mtc2	$t2, $3
	/* vertex 2 -> VXY2 (dr4), VZ2 (dr5) */
	lh	$t0, 12($a0)
	andi	$t0, $t0, 0xffff
	lh	$t1, 14($a0)
	sll	$t1, $t1, 16
	or	$t0, $t0, $t1
	mtc2	$t0, $4
	lh	$t2, 16($a0)
	mtc2	$t2, $5
	cop2	0x0280030		/* RTPT: rotate/translate/project all three */
	mfc2	$t0, $12		/* SXY0 */
	sll	$t1, $t0, 16
	sra	$t1, $t1, 16
	sw	$t1, 0($a1)
	sra	$t2, $t0, 16
	sw	$t2, 4($a1)
	mfc2	$t0, $13		/* SXY1 */
	sll	$t1, $t0, 16
	sra	$t1, $t1, 16
	sw	$t1, 8($a1)
	sra	$t2, $t0, 16
	sw	$t2, 12($a1)
	mfc2	$t0, $14		/* SXY2 */
	sll	$t1, $t0, 16
	sra	$t1, $t1, 16
	sw	$t1, 16($a1)
	sra	$t2, $t0, 16
	sw	$t2, 20($a1)
	jr	$ra

/* int psx_gte_nclip(void)
 * The signed area (cross product) of the three projected points now in the SXY
 * FIFO. Call right after psx_gte_rtpt. A positive result is one winding and a
 * negative result the other, so its sign is the backface test. */
psx_gte_nclip:
	cop2	0x1400006		/* NCLIP: MAC0 = cross product of SXY0,1,2 */
	mfc2	$v0, $24		/* MAC0 */
	jr	$ra

/* int psx_gte_avsz3(void)
 * The average of the three depths SZ1..SZ3 now in the FIFO, as the GTE's OTZ.
 * Call right after psx_gte_rtpt; use it to pick an ordering-table bucket. */
psx_gte_avsz3:
	cop2	0x0158002d		/* AVSZ3: OTZ = (SZ1+SZ2+SZ3)/3 (scaled) */
	mfc2	$v0, $7			/* OTZ */
	jr	$ra

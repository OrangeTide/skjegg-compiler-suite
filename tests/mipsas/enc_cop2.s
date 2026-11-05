# Encoding fixture: the coprocessor-2 (GTE) moves and command word that
# libpsx/gte.S relies on.  The MIPS backend never emits these, so skj-as-mips
# grew mtc2/mfc2/ctc2/cfc2 and the raw cop2 command to reach the GTE.  GNU as is
# the golden master, so a byte mismatch is a skj-as-mips encoding bug.
# .set noreorder so no delay-slot scheduling perturbs the comparison.
	.set noreorder
	.text
	.globl f
f:
	# data-register moves (mtc2/mfc2): the vertex in, the result out
	mtc2 $t0, $0			# VXY0
	mtc2 $a2, $1			# VZ0
	mfc2 $t0, $14			# SXY2
	mfc2 $t3, $19			# SZ3
	mfc2 $t0, $12			# SXY0
	mfc2 $t1, $13			# SXY1
	# control-register moves (ctc2/cfc2): the matrix, translation, screen
	ctc2 $t0, $0			# R12:R11
	ctc2 $t0, $4			# R33
	ctc2 $a0, $5			# TRX
	ctc2 $a2, $7			# TRZ
	ctc2 $a0, $24			# OFX
	ctc2 $a1, $26			# H
	cfc2 $t1, $31			# FLAG
	# GTE command words (raw cop2): the projections and a cross product
	cop2 0x0180001			# RTPS   (rotate, translate, project one)
	cop2 0x0280030			# RTPT   (project three)
	cop2 0x1400006			# NCLIP  (cross product of the projected tri)
	cop2 0x0168002E			# AVSZ4  (average of four Z)

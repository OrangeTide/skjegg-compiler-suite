# Encoding golden-master: single, double and half float (F/D/Zfh).
	.text
	flw	ft0, 0(a0)
	fld	ft1, 8(a0)
	flh	ft2, 4(a0)
	fsw	ft0, 0(a1)
	fsd	ft1, 8(a1)
	fsh	ft2, 4(a1)
	fadd.s	ft0, ft1, ft2
	fsub.s	ft0, ft1, ft2
	fmul.s	ft0, ft1, ft2
	fdiv.s	ft0, ft1, ft2
	fadd.d	fa0, fa1, fa2
	fsub.d	fa0, fa1, fa2
	fmul.d	fa0, fa1, fa2
	fdiv.d	fa0, fa1, fa2
	fadd.h	ft0, ft1, ft2
	fsqrt.s	ft0, ft1
	fsqrt.d	fa0, fa1
	fsgnj.s	ft0, ft1, ft2
	fsgnjn.s	ft0, ft1, ft2
	fsgnjx.s	ft0, ft1, ft2
	fmin.s	ft0, ft1, ft2
	fmax.s	ft0, ft1, ft2
	fmin.d	fa0, fa1, fa2
	fmax.d	fa0, fa1, fa2
	fmv.s	ft0, ft1
	fneg.s	ft0, ft1
	fabs.s	ft0, ft1
	fmv.d	fa0, fa1
	fneg.d	fa0, fa1
	fabs.d	fa0, fa1
	feq.s	a0, ft0, ft1
	flt.s	a0, ft0, ft1
	fle.s	a0, ft0, ft1
	feq.d	a0, fa0, fa1
	flt.d	a0, fa0, fa1
	fle.d	a0, fa0, fa1
	fcvt.s.w	ft0, a0
	fcvt.s.wu	ft0, a0
	fcvt.w.s	a0, ft0, rtz
	fcvt.wu.s	a0, ft0, rtz
	fcvt.d.w	fa0, a0
	fcvt.w.d	a0, fa0, rtz
	fcvt.d.s	fa0, ft0
	fcvt.s.d	ft0, fa0
	fcvt.d.h	fa0, ft0
	fcvt.h.d	ft0, fa0
	fcvt.s.h	ft0, ft1
	fcvt.h.s	ft0, ft1
	fmv.x.w	a0, ft0
	fmv.w.x	ft0, a0
	fmv.x.h	a0, ft0
	fmv.h.x	ft0, a0
	fclass.s	a0, ft0
	fclass.d	a0, fa0
	fmadd.s	ft0, ft1, ft2, ft3
	fmsub.s	ft0, ft1, ft2, ft3
	fnmsub.s	ft0, ft1, ft2, ft3
	fnmadd.s	ft0, ft1, ft2, ft3
	fmadd.d	fa0, fa1, fa2, fa3
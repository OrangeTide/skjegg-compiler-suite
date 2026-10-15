# Encoding golden-master: the call and tail function-call pseudos.  Both expand
# to auipc + jalr with one R_RISCV_CALL_PLT; call links through ra, tail does
# not link and uses t1, so the two differ only in their register fields.
	.text
f:
	call	callee
	tail	callee

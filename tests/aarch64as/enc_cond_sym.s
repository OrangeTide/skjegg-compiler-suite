// condition-code mnemonics are not reserved: a symbol (here an external
// function) may be spelled like one, so `bl eq` is a call to eq, while
// `cset w0, eq` still uses the condition.  GNU as is the golden master.
.text
.globl cond_sym_test
cond_sym_test:
	bl	eq
	bl	ne
	b	hi
	cmp	w0, w1
	cset	w0, eq
	cset	w1, ne
	cset	w2, hi
	cset	w3, lt
	ret

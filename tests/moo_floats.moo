// floats.moo : test float type support
// returns non-zero on first failure, 0 on success

verb check_add(a: float, b: float, expect: float)
	var result: float = a + b;
	var diff: float = result - expect;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff < 0.001)
		return 1;
	endif
	return 0;
endverb

verb check_mul(a: float, b: float, expect: float)
	var result: float = a * b;
	var diff: float = result - expect;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff < 0.001)
		return 1;
	endif
	return 0;
endverb

verb main()
	// 1: basic float literals and assignment
	var x: float = 3.14;
	var diff: float = x - 3.14;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 1;
	endif

	// 2: float addition
	var y: float = 1.5 + 2.5;
	diff = y - 4.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 2;
	endif

	// 3: float subtraction
	var z: float = 10.0 - 3.5;
	diff = z - 6.5;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 3;
	endif

	// 4: float multiplication
	var m: float = 2.5 * 4.0;
	diff = m - 10.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 4;
	endif

	// 5: float division
	var d: float = 7.0 / 2.0;
	diff = d - 3.5;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 5;
	endif

	// 6: float modulo (exact)
	var r: float = 7.5 % 2.5;
	diff = r - 0.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.01)
		return 6;
	endif

	// 7: float modulo (non-exact)
	var r2: float = 10.0 % 3.0;
	diff = r2 - 1.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 7;
	endif

	// 8: unary negation
	var neg: float = -5.25;
	diff = neg + 5.25;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 8;
	endif

	// 9: float comparison ==
	var a: float = 1.0;
	var b: float = 1.0;
	if (!(a == b))
		return 9;
	endif

	// 10: float comparison !=
	if (!(a != 2.0))
		return 10;
	endif

	// 11: float comparison <
	if (!(1.0 < 2.0))
		return 11;
	endif

	// 12: float comparison <=
	if (!(1.0 <= 1.0))
		return 12;
	endif
	if (!(0.5 <= 1.0))
		return 12;
	endif

	// 13: float comparison >
	if (!(3.0 > 2.0))
		return 13;
	endif

	// 14: float comparison >=
	if (!(2.0 >= 2.0))
		return 14;
	endif
	if (!(3.0 >= 2.0))
		return 14;
	endif

	// 15: int-to-float promotion in addition
	var pi: float = 3 + 0.14;
	diff = pi - 3.14;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 15;
	endif

	// 16: int-to-float promotion (int on right)
	var f16: float = 0.5 + 1;
	diff = f16 - 1.5;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 16;
	endif

	// 17: int-to-float promotion in multiplication
	var f17: float = 2 * 3.5;
	diff = f17 - 7.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 17;
	endif

	// 18: float function call (passing and arithmetic)
	if (!check_add(1.5, 2.5, 4.0))
		return 18;
	endif

	// 19: float multiplication via function
	if (!check_mul(3.0, 4.0, 12.0))
		return 19;
	endif

	// 20: toint(float) -- truncation
	var ti: int = toint(3.7);
	if (ti != 3)
		return 20;
	endif

	// 21: toint(float) -- negative truncation
	var ti2: int = toint(-2.9);
	if (ti2 != -2)
		return 21;
	endif

	// 22: tofloat(int)
	var tf: float = tofloat(42);
	diff = tf - 42.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 22;
	endif

	// 23: tofloat(str)
	var tf2: float = tofloat("3.14");
	diff = tf2 - 3.14;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.01)
		return 23;
	endif

	// 24: int:tofloat() method
	var n: int = 100;
	var nf: float = n:tofloat();
	diff = nf - 100.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 24;
	endif

	// 25: float:toint() method
	var ft: float = 9.8;
	var fi: int = ft:toint();
	if (fi != 9)
		return 25;
	endif

	// 26: compound assignment +=
	var ca: float = 1.0;
	ca += 2.5;
	diff = ca - 3.5;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 26;
	endif

	// 27: compound assignment -=
	var cs: float = 10.0;
	cs -= 3.5;
	diff = cs - 6.5;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 27;
	endif

	// 28: zero float literal
	var zf: float = 0.0;
	if (!(zf == 0.0))
		return 28;
	endif

	// 29: small float
	var small: float = 0.001;
	diff = small - 0.001;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.0001)
		return 29;
	endif

	// 30: chained arithmetic
	var chain: float = 1.0 + 2.0 + 3.0 + 4.0;
	diff = chain - 10.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 30;
	endif

	// 31: parenthesized expression
	var paren: float = (2.0 + 3.0) * 4.0;
	diff = paren - 20.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 31;
	endif

	// 32: float in comparison with var
	var cond: float = 5.0;
	if (cond < 1.0)
		return 32;
	endif

	// 33: mixed expression with int promotion
	var vp: float = 5 * 2.0 + 1;
	diff = vp - 11.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 33;
	endif

	// 34: large float
	var big: float = 1000000.0;
	diff = big - 1000000.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 1.0)
		return 34;
	endif

	// 35: float with exponent literal
	var sci: float = 1.5e2;
	diff = sci - 150.0;
	if (diff < 0.0)
		diff = 0.0 - diff;
	endif
	if (diff > 0.001)
		return 35;
	endif

	return 0;
endverb

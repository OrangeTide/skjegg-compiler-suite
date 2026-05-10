verb add(a: int, b: int)
	return a + b;
endverb

verb main()
	var x: int = 5;
	var y: int = 10;
	var sum: int = add(x, y);

	if (sum == 15)
		return 0;
	endif
	return 1;
endverb

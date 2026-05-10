verb main()
	var x: int = 0;

	defer
		if (x != 20)
			x = 99;
		endif
	enddefer

	defer
		var e: err = recover();
		if (e == $E_NONE)
			x = 99;
		endif
		x = 20;
	enddefer

	panic($E_RANGE);
	return 1;
endverb

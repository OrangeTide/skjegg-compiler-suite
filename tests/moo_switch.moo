verb main()
	var x: int = 3;
	var result: int = 0;
	switch (x)
		case 1:
			result = 10;
		case 2, 3:
			result = 20;
		case 4..10:
			result = 30;
		else:
			result = 99;
	endswitch
	if (result != 20)
		return 1;
	endif

	var y: int = 7;
	switch (y)
		case 1..5:
			result = 1;
		case 6..10:
			result = 2;
		else:
			result = 0;
	endswitch
	if (result != 2)
		return 2;
	endif

	var z: int = 99;
	switch (z)
		case 1:
			result = 1;
		else:
			result = 42;
	endswitch
	if (result != 42)
		return 3;
	endif

	return 0;
endverb

verb main()
	var room: obj = $"rooms/lobby";
	var s: str = tostr(42);

	if (length(s) != 2)
		return 1;
	endif

	if (!valid(room))
		return 2;
	endif

	var items: list<obj> = contents(room);
	if (length(items) != 0)
		return 3;
	endif

	var es: str = tostr($E_RANGE);
	if (length(es) != 7)
		return 4;
	endif

	return 0;
endverb

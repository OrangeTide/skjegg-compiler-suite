verb main()
	var s: str = "hello world";

	// toint
	if (toint("42") != 42)
		return 1;
	endif
	if (toint("-7") != -7)
		return 2;
	endif
	if (toint("0") != 0)
		return 3;
	endif

	// index
	if (index(s, "world") != 7)
		return 4;
	endif
	if (index(s, "xyz") != 0)
		return 5;
	endif
	if (index(s, "h") != 1)
		return 6;
	endif

	// substr
	var sub: str = substr(s, 7, 11);
	if (sub != "world")
		return 7;
	endif
	if (substr(s, 1, 5) != "hello")
		return 8;
	endif

	// strsub
	var replaced: str = strsub(s, "world", "there");
	if (replaced != "hello there")
		return 9;
	endif
	if (strsub(s, "xyz", "abc") != "hello world")
		return 10;
	endif

	// method syntax
	if (s:index("world") != 7)
		return 11;
	endif
	if (s:substr(1, 5) != "hello")
		return 12;
	endif
	if (s:strsub("hello", "goodbye") != "goodbye world")
		return 13;
	endif

	// int:tostr() method
	var n: int = 99;
	if (n:tostr() != "99")
		return 14;
	endif

	return 0;
endverb

verb main()
	var s: str = "hello" + " " + "world";
	if (length(s) != 11)
		return 1;
	endif
	if (s == "hello world")
		return 0;
	endif
	return 2;
endverb

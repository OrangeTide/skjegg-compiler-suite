verb main()
	var items: list<int> = {10, 20, 30};
	if (items:length() != 3)
		return 1;
	endif
	var bigger: list<int> = items:append(40);
	if (bigger:length() != 4)
		return 2;
	endif
	var s: str = "hello";
	if (s:length() != 5)
		return 3;
	endif
	return 0;
endverb

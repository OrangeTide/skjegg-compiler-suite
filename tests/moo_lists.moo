verb main()
	var items: list<int> = {10, 20, 30};
	var sum: int = 0;
	for x in items
		sum += x;
	endfor
	if (sum != 60)
		return 1;
	endif
	if (items[2] != 20)
		return 2;
	endif
	if (length(items) != 3)
		return 3;
	endif
	if (!(20 in items))
		return 4;
	endif
	if (99 in items)
		return 5;
	endif
	var longer: list<int> = listappend(items, 40);
	if (length(longer) != 4)
		return 6;
	endif
	items[2] = 99;
	if (items[2] != 99)
		return 7;
	endif
	if (items[1] != 10)
		return 8;
	endif
	return 0;
endverb

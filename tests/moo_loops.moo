verb main()
	var sum: int = 0;
	for i in 1..10
		sum += i;
	endfor
	// 1+2+...+10 = 55
	if (sum == 55)
		return 0;
	endif
	return 1;
endverb

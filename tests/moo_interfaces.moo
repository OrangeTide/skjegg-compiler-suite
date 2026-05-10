interface Named
	name: str;
	description: str;
endinterface

verb main()
	var thing: obj = $"sword";
	if (thing is Named)
		return 1;
	endif
	var caught: int = 0;
	defer
		var e: err = recover();
		if (e != nil)
			caught = 1;
		endif
	enddefer
	var d: Named = thing as Named;
	return 0;
endverb

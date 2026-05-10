verb main()
	var lobby: obj = $"rooms/lobby";

	// typed unbox: prop -> str
	var name: str = lobby.name;
	if (name != "The Lobby")
		return 1;
	endif

	// prop in string concat
	var greeting: str = "Welcome to " + lobby.name;
	if (greeting != "Welcome to The Lobby")
		return 2;
	endif

	// prop used with str method
	var desc: str = lobby.description;
	var idx: int = desc:index("grand");
	if (idx != 3)
		return 3;
	endif

	// prop variable round-trip
	var p: prop = lobby.name;
	var n2: str = p;
	if (n2 != "The Lobby")
		return 4;
	endif

	// prop in comparison
	if (lobby.name != "The Lobby")
		return 5;
	endif

	// typeof on prop variable
	if (typeof(p) != 1)
		return 6;
	endif

	// typeof on static type folds to constant
	if (typeof(name) != 1)
		return 7;
	endif
	var x: int = 42;
	if (typeof(x) != 0)
		return 8;
	endif

	return 0;
endverb

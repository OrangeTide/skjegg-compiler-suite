verb main()
	var lobby: obj = $"rooms/lobby";

	if (!valid(lobby))
		return 1;
	endif

	var name: str = lobby.name;
	if (name == nil)
		return 2;
	endif

	var player: obj = $"players/alice";
	player:tell(name);
	player:tell(lobby.description);

	var here: list<obj> = contents(lobby);
	if (length(here) < 2)
		return 3;
	endif

	player:tell("You see:");
	for item in here
		player:tell("  " + item.name);
	endfor

	return 0;
endverb

interface Describable
	name: str;
	description: str;
endinterface

interface Tellable
	tell(msg: str);
endinterface

interface Magical
	name: str;
	magic_power: str;
endinterface

verb main()
	var lobby: obj = $"rooms/lobby";
	var result: int = 0;

	// lobby has name+description: matches Describable
	// type narrowing: lobby.name returns str inside case body
	switch type (lobby)
		case Describable:
			var dname: str = lobby.name;
			if (dname != "The Lobby")
				return 10;
			endif
			var ddesc: str = lobby.description;
			if (ddesc != "A grand entry hall.")
				return 11;
			endif
			result = 1;
		else:
			return 1;
	endswitch
	if (result != 1)
		return 2;
	endif

	// lobby has no magic_power: falls to else
	switch type (lobby)
		case Magical:
			return 3;
		else:
			result = 2;
	endswitch
	if (result != 2)
		return 4;
	endif

	// first case fails, second matches
	switch type (lobby)
		case Magical:
			return 5;
		case Describable:
			result = 3;
		else:
			return 6;
	endswitch
	if (result != 3)
		return 7;
	endif

	// verb call on narrowed interface
	switch type (lobby)
		case Tellable:
			lobby:tell("narrowed verb call works");
			result = 4;
		else:
			return 8;
	endswitch
	if (result != 4)
		return 9;
	endif

	return 0;
endverb

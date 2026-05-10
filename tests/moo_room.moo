// room_verbs.moo -- verbs for generic room prototype

verb look(this: obj, player: obj)
    player:tell(this.name);
    player:tell(this.description);

    var here: list<obj> = contents(this);
    if (length(here) > 0)
        player:tell("You see:");
        for item in here
            if (item != player)
                player:tell("  " + item.name);
            endif
        endfor
    endif

    var exits: str = this.exits;
    if (exits != nil)
        player:tell("Exits: " + exits);
    endif
endverb

verb say(this: obj, player: obj, text: str)
    var msg: str = player.name + " says, \"" + text + "\"";
    for who in contents(this)
        who:tell(msg);
    endfor
endverb

verb go(this: obj, player: obj, direction: str)
    /// go verb entered
    var dest: str = this.("exit_" + direction);
    if (dest == nil)
        player:tell("You can't go that way.");
        return;
    endif
    trace "go: " + player.name + " heading " + direction;
    player:move_to(dest);
endverb

verb take(this: obj, player: obj, dobj: obj)
    if (dobj.location != this)
        panic($E_RANGE);
    endif
    move(dobj, player);
    player:tell("You pick up " + dobj.name + ".");
endverb

verb fragile_op(player: obj, dobj: obj)
    defer
        player:tell("cleanup done");
    enddefer

    defer
        var e: err = recover();
        if (e != nil)
            player:tell("Error: " + tostr(e));
        endif
    enddefer

    if (!valid(dobj))
        panic($E_INVARG);
    endif

    var items: list<str> = {"sword", "shield", "potion"};
    var first: str = items[1];
    var sub: list<str> = items[1..2];
    var n: int = length(items);
    var with: list<str> = listappend(items, "arrow");

    for i in 1..n
        player:tell(tostr(i) + ": " + items[i]);
    endfor

    var count: int = 10;
    while (count > 0)
        count -= 1;
    endwhile
endverb

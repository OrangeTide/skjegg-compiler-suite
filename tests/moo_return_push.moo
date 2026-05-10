// test: return.. push-back list building

func make_range(n: int) -> list<int>
    var i: int = 1;
    while (i <= n)
        return.. i;
        i += 1;
    endwhile
    return;
endfunc

func squares(n: int) -> list<int>
    var i: int = 1;
    while (i <= n)
        return.. i * i;
        i += 1;
    endwhile
    return;
endfunc

verb main()
    var r: list<int> = make_range(5);
    if (length(r) != 5)
        return 1;
    endif
    if (r[1] != 1)
        return 2;
    endif
    if (r[5] != 5)
        return 3;
    endif

    var s: list<int> = squares(4);
    if (length(s) != 4)
        return 4;
    endif
    if (s[1] != 1)
        return 5;
    endif
    if (s[2] != 4)
        return 6;
    endif
    if (s[3] != 9)
        return 7;
    endif
    if (s[4] != 16)
        return 8;
    endif

    // empty list
    var e: list<int> = make_range(0);
    if (length(e) != 0)
        return 9;
    endif

    return 0;
endverb

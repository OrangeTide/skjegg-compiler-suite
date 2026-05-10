// test: extern verb with link-name aliasing

extern __moo_str_len as func slen(s: str) -> int;

verb main()
    var s: str = "hello";
    if (slen(s) != 5)
        return 1;
    endif
    if (slen("ab") != 2)
        return 2;
    endif
    return 0;
endverb

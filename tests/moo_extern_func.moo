// test: multiple extern funcs with and without aliasing

extern __moo_str_len as func slen(s: str) -> int;
extern __moo_str_concat as func scat(a: str, b: str) -> str;
extern func __moo_toint(s: str) -> int;

verb main()
    var s: str = scat("hel", "lo");
    if (slen(s) != 5)
        return 1;
    endif
    if (s != "hello")
        return 2;
    endif
    var n: int = __moo_toint("42");
    if (n != 42)
        return 3;
    endif
    return 0;
endverb

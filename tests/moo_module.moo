// test: module/import declarations and export prefix

module test_mod;

import utils;

export func double_it(x: int) -> int
    return x + x;
endfunc

export func add(a: int, b: int) -> int
    return a + b;
endfunc

func internal_helper(x: int) -> int
    return x * 3;
endfunc

verb main()
    if (double_it(21) != 42)
        return 1;
    endif
    if (add(10, 20) != 30)
        return 2;
    endif
    if (internal_helper(7) != 21)
        return 3;
    endif
    return 0;
endverb

// test: func (local linkage) with return type annotations

func square(x: int) -> int
    return x * x;
endfunc

func add(a: int, b: int) -> int
    return a + b;
endfunc

func fib(n: int) -> int
    if (n <= 1)
        return n;
    endif
    return add(fib(n - 1), fib(n - 2));
endfunc

verb main()
    if (square(5) != 25)
        return 1;
    endif
    if (add(10, 20) != 30)
        return 2;
    endif
    if (fib(10) != 55)
        return 3;
    endif
    return 0;
endverb

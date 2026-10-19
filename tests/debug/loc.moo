// Fixture for test-moo-loc: debug-info line markers (-g) on the MooScript path.
// Each statement sits on its own line so skj-mooc emits one IR_LOC per line and
// the ColdFire backend records a "| line N" comment.  The test only inspects the
// assembly, so the program need not do anything interesting.

func add(a: int, b: int) -> int
    return a + b;
endfunc

verb main()
    var x: int = add(2, 3);
    x = x + 1;
    return x;
endverb

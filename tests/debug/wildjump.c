/* wildjump.c : call through a garbage function pointer.  The guest faults with
   a wild pc, so skj-run's backtrace must start from a bogus top frame and still
   unwind the intact chain below it (wrap, main) without the host crashing or
   hanging.  Pairs with the dw_backtrace unit test (tests/debug/bt_loop.c). */

typedef void (*fn)(void);

int
wrap(void)
{
    fn f = (fn)0xdeadbee0u;     /* unmapped: fetch fault at a wild address */
    f();
    return 0;
}

int
main(void)
{
    return wrap();
}

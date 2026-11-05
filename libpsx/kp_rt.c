/* kp_rt.c - minimal Kobold runtime entry for the bare-metal PlayStation.
 *
 * The Kobold AOT compiler names a program's entry kp_guest_entry. The
 * freestanding crt (kobold/io_free.c) normally wraps that in main with a
 * syscall-based heap and buffered stdio. A PlayStation game that drives libpsx
 * directly and uses no Kobold heap, string, or I/O features needs none of
 * that, so this is the whole bridge: start_psx calls main, main runs the
 * program. It is pulled from libpsx.a only for a Kobold program, since C and
 * Pascal programs define their own main. It grows to pull in the managed
 * Kobold runtime when a sample needs it (a later milestone).
 */

extern void kp_guest_entry(void);

int
main(void)
{
    kp_guest_entry();
    return 0;
}

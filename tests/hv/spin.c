/* A compute-bound HV guest: an endless loop that issues no syscalls, so it
   never traps to the doorbell.  Without a preemption timer it would hold the
   vcpu forever and ignore --max-insns; with the timer it is bounded, so
   `skj-run --max-insns N` stops it.  The test-hv-preempt target runs it. */

int
main(void)
{
    volatile unsigned long x = 0;

    for (;;)
        x++;
    return 0;
}

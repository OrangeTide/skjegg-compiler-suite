/* hvcore.h : the hooks the HV GDB stub needs into the HV core (hvcore.c). */

#ifndef SKJ_HVCORE_H
#define SKJ_HVCORE_H

#include "vmm.h"
#include "sched.h"

/* The process's vcpu handle (p->cpu is the core-private HV cpu struct). */
vmm_vcpu *hv_vcpu_of(proc *p);

/* Has the process halted (a clean exit, a fault, or FATAL)? */
int hv_is_halted(proc *p);

/* Service one syscall doorbell exit, the same path the run loop uses: the
 * emulator-private selectors or the normal guest syscall, with the result
 * delivered as the load's MMIO data.  Sets g.exited / halted on exit. */
void hv_service_doorbell(proc *p, struct vmm_exit *ex);

#endif /* SKJ_HVCORE_H */

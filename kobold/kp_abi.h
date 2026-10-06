/* kp_abi.h : the freestanding guest-runtime entry ABI version.

   The freestanding tier (libkpaot-*-free.a) and the start file that drives it
   share a seam: kp_set_raw_args, kp_rt_init (the guest heap handed in at entry),
   kp_guest_entry (the program's entry), and kp_sys_exit.  A downstream host such
   as boris links the archive under its own start file, so a change to that seam
   between a vendored archive and a host-written crt would otherwise be silent, a
   crt passing the wrong arguments to a runtime that has moved on.

   This version makes such a skew a link error.  The archive defines the symbol
   kp_guest_abi_v<N> (N = KP_GUEST_ABI_VERSION) and the crt references it, so an
   archive of one version linked against a crt of another leaves kp_guest_abi_v<N>
   undefined and the link fails.  A downstream crt should carry the same reference
   (see kp_crt_rv.S) so its own build is checked the same way.  The archive also
   exports the plain value kp_guest_abi_version for a host that prefers to check it
   at build time or runtime.

   Bump KP_GUEST_ABI_VERSION here and the matching reference in kp_crt_rv.S together
   whenever the entry seam changes. */

#ifndef KP_ABI_H
#define KP_ABI_H

#define KP_GUEST_ABI_VERSION 1

#endif

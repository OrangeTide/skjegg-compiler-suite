/* frontend.h : run the Kobold front end (kpcom) in-process to compile
   Kobold Pascal source to a KPM1 module.  This is what lets skj-kp-jit accept a
   `.pas` source file, not only a prebuilt `.kpm`: kpcom is linked in natively
   beside the C module builder and compiles straight into it in one step. */

#ifndef KP_FRONTEND_H
#define KP_FRONTEND_H

#include <stddef.h>

/* Compile the Kobold Pascal source file at `pas_path` to a KPM1 module.  The
   linked-in front end's CompileModule export is called in-process; it compiles
   straight into the C module builder, and the finished module is serialized once
   with kpm_write.  On success returns 0 and sets *module_text to a malloc'd buffer
   (the caller frees it) and *out_len to its byte length.  On a user source error
   returns 2 (kpcom has already written diagnostics to stderr; err is left empty).
   On an infrastructure failure returns 1 with a one-line reason in err. */
int kp_compile_pas(const char *pas_path, char **module_text, size_t *out_len,
                   char *err, size_t errlen);

#endif /* KP_FRONTEND_H */

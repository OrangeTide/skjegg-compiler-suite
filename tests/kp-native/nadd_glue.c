/* The unit's glue object, archived inside nadd.a beside the .kpm.  It is reached
   because aot-run.sh extracts .o members from a unit archive; it links because the
   sidecar .link adds -lnatextra. */
extern int natextra_double(int x);
int kpnat_calc(int a, int b) { return natextra_double(a + b); }

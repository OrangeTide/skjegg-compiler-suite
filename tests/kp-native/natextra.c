/* A stand-in external library for the native-unit test: the glue reaches it only
   through the unit's sidecar .link flags, so a broken sidecar path is a link error. */
int natextra_double(int x) { return x * 2; }

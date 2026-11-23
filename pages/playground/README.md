# skjegg playground

A browser playground that compiles a single source file to a single WebAssembly
executable and runs it, with a console. The MVP covers TinScheme; the
architecture is built to grow to the other front ends.

## What makes it work

The compiler that runs in the browser is skjegg compiling **itself**. The
TinScheme compiler's own C sources (front end + shared IR + WASM backend + the
WASI libc) are compiled by `skj-cc-wasm` and linked by `skj-ld-wasm` into a
single `skj-sc.wasm`, with no external toolchain. `make check-selfhost-wasm`
proves that module's output is byte-identical to the native cross compiler.

The page loads that module in a Web Worker and drives it through a small WASI
preview1 shim (`wasi-shim.js`) with an in-memory filesystem: it writes the
source as `in.scm`, runs the compiler with `-o out.wasm in.scm`, and reads the
produced module back. A second worker runs that module under the same shim and
streams its stdout to the console.

## Build and serve

```sh
make playground          # builds skj-sc.wasm and copies it next to the page
cd pages/playground && python3 -m http.server 8080
# open http://localhost:8080/
```

Serve over http, not `file://`: a page opened from the filesystem cannot fetch
the `.wasm` module or spawn the workers.

## Files

- `index.html` - editor, console, and the two-worker orchestration.
- `compiler-worker.js` - runs `skj-sc.wasm` (the compiler) over a memfs + argv.
- `run-worker.js` - runs a produced program module and captures its stdout.
- `wasi-shim.js` - the shared, dependency-free WASI preview1 host.
- `skj-sc.wasm` - the self-hosted compiler (a build artifact, not committed).

## Limits of the MVP

- TinScheme only. Each other language is another compiler worker of the same
  shape; the object+link front ends (C, BASIC, Pascal, MooScript) additionally
  need a linker worker and a precompiled runtime-object bundle.
- Console only; no canvas yet (that needs a graphics host-import ABI).
- A program that uses delimited continuations (`shift`/`reset`) emits WasmFX
  stack-switching instructions and needs a WasmFX-capable engine to *run*; it
  still compiles correctly everywhere.

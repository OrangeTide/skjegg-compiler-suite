# skjegg playground

A browser playground that compiles a source file to a WebAssembly executable and
runs it, with a console. It covers four languages: **TinScheme, Pascal, BASIC,
and MooScript**. Pick a language, press Compile, then Run.

## What makes it work

Every compiler that runs in the browser is skjegg compiling **itself**. Each
front end's own C sources (front end + shared IR + WASM backend + the WASI libc)
are compiled by `skj-cc-wasm` and linked by `skj-ld-wasm` into a single
`skj-<tool>.wasm`, with no external toolchain. The WASM linker is self-hosted the
same way into `skj-ld.wasm`.

The page loads a compiler module in a Web Worker and drives it through a small
WASI preview1 shim (`wasi-shim.js`) with an in-memory filesystem: it writes the
source into memfs, runs the compiler with `-o out.<ext> source.<ext>`, and reads
the produced bytes back.

- **TinScheme** emits a complete module directly, which a run worker executes.
- **Pascal, BASIC, MooScript** emit a relocatable WASM object. A link worker then
  runs `skj-ld.wasm` to link that object against the language's runtime objects
  (precompiled once and served as assets) into a complete module, which the run
  worker executes.

So the pipeline is compile, then for the object+link languages link, then run,
all client side with skjegg's own tools.

## Build and serve

```sh
make playground          # builds every compiler wasm + runtime objects into build/playground/
cd build/playground && <any static http server> .
# open http://localhost:<port>/
```

Serve over http, not `file://`: a page opened from the filesystem cannot fetch
the `.wasm` modules or spawn the workers.

`make check-playground-selfhost` drives the whole pipeline on the host (each
self-hosted compiler compiles a sample, `skj-ld.wasm` links it, the module runs
under `wasmtime`), the end-to-end proof that the browser path works.

## Files

- `index.html` - editor, console, language selector, and the worker orchestration.
- `compiler-worker.js` - runs a self-hosted compiler wasm over a memfs + argv;
  returns a complete module (TinScheme) or a relocatable object (the others).
- `link-worker.js` - runs `skj-ld.wasm` to link an object against runtime objects.
- `run-worker.js` - runs a produced program module and captures its stdout.
- `wasi-shim.js` - the shared, dependency-free WASI preview1 host.
- `skj-*.wasm`, `rt/*.o` - the self-hosted compilers, linker, and runtime objects
  (build artifacts, not committed; produced by `make playground`).

## Limits

- No C yet: `skj-cc-wasm` miscompiles one of cc's own float functions to invalid
  WASM, a self-host codegen bug to fix before C can join.
- Console only; no canvas yet (that needs a graphics host-import ABI).
- A TinScheme program that uses delimited continuations (`shift`/`reset`) emits
  WasmFX stack-switching instructions and needs a WasmFX-capable engine to *run*;
  it still compiles correctly everywhere.

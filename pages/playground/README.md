# skjegg playground

A browser playground that compiles a source file to a WebAssembly executable and
runs it, with a console. It covers five languages: **C, TinScheme, Pascal,
BASIC, and MooScript**. Pick a language, press Compile, then Run. The editor
highlights each language's syntax.

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
- **C, Pascal, BASIC, MooScript** emit a relocatable WASM object. A link worker
  then runs `skj-ld.wasm` to link that object against the language's runtime
  objects (precompiled once and served as assets) into a complete module, which
  the run worker executes. C links against the full WASM libc, shipped as one
  gzipped `ar` archive (`rt/c.a.gz`) that the link worker fetches once and
  gunzips with the platform `DecompressionStream`, then hands whole to
  `skj-ld.wasm`, which reads the archive and pulls only the members the program
  references. So `printf` and friends reach the console, in one compressed
  download instead of ~25 object fetches, and the module carries only the libc
  actually used rather than every object.

So the pipeline is compile, then for the object+link languages link, then run,
all client side with skjegg's own tools.

## File bundles

A bundle is the general mechanism for shipping files alongside a language. It is
a JSON document of `{ path, content, editor }` entries (`gen-bundle.js` packs a
directory into one). Before a compile, the compiler worker seeds every bundle
file into its WASI memfs at `path`, so the compiler can open it. An `editor`
file can also be opened read-only in the editor from the **files** menu.

C ships the libc headers as its bundle: `#include <stdio.h>` resolves in the
browser the same way `-Iinclude` resolves it for the host self-host smoke. The
self-hosted `skj-cc.wasm` has no baked-in default include path, so without the
bundle `printf` would be implicitly declared non-variadic and miscompile.

## Syntax highlighting

`highlight.js` is a small, dependency-free tokenizer for the five languages. The
editor is a transparent textarea layered over a highlighted `<pre>`, so comment,
string, number, keyword and preprocessor tokens are colored while the caret and
selection stay native.

## Build and serve

```sh
make playground          # builds every compiler wasm + runtime objects into build/playground/
cd build/playground && node serve.js 8080
# open http://localhost:8080/
```

`make playground` runs `node` once to pack the C header bundle (`gen-bundle.js`);
override the binary with `make playground NODE=/path/to/node` if a directory
named `node` shadows it on `PATH`. Any static http server works; `serve.js` is a
dependency-free one. Serve over http, not `file://`: a page opened from the
filesystem cannot fetch the `.wasm` modules or spawn the workers.

`make check-playground-selfhost` drives the whole pipeline on the host (each
self-hosted compiler compiles a sample, `skj-ld.wasm` links it, the module runs
under `wasmtime`), the end-to-end proof that the browser path works.

## Files

- `index.html` - editor, console, language selector, and the worker orchestration.
- `compiler-worker.js` - runs a self-hosted compiler wasm over a memfs + argv;
  seeds bundle files; returns a complete module (TinScheme) or a relocatable
  object (the others).
- `link-worker.js` - runs `skj-ld.wasm` to link an object against runtime objects.
- `run-worker.js` - runs a produced program module and captures its stdout.
- `wasi-shim.js` - the shared, dependency-free WASI preview1 host.
- `highlight.js` - the dependency-free syntax highlighter.
- `gen-bundle.js` - build helper: packs a directory into a bundle JSON.
- `serve.js` - dependency-free static server for previewing the build.
- `skj-*.wasm`, `rt/*.o`, `rt/c.a.gz`, `bundle/*.json` - the self-hosted
  compilers, linker, runtime objects, the gzipped C runtime archive, and file
  bundles (build artifacts, not committed; produced by `make playground`).

## Limits

- Console only; no canvas yet (that needs a graphics host-import ABI).
- A TinScheme program that uses delimited continuations (`shift`/`reset`) emits
  WasmFX stack-switching instructions and needs a WasmFX-capable engine to *run*;
  it still compiles correctly everywhere.

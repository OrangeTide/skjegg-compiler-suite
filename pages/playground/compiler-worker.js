// compiler-worker.js : runs the self-hosted TinScheme compiler (skj-sc.wasm) in
// a Web Worker.  It loads the compiler module once, then for each {compile,
// source} message writes the source into the WASI memfs, runs the compiler with
// argv ["skj-sc", "-o", "out.wasm", "in.scm"], and posts back the produced
// module bytes (or the stderr diagnostics on a nonzero exit).
//
// This is one "front-end worker".  Adding another language is another worker of
// the same shape pointed at that language's compiler wasm (plus, for the
// object+link front ends, a linker worker and a runtime-object bundle).

/* global createWASI */
importScripts("wasi-shim.js");

var compilerModule = null;
var loadError = null;

// Compile the module as soon as the worker starts, so the first compile is fast.
fetch("skj-sc.wasm")
    .then(function (r) {
        if (!r.ok) throw new Error("skj-sc.wasm: HTTP " + r.status);
        return r.arrayBuffer();
    })
    .then(function (buf) {
        compilerModule = new WebAssembly.Module(buf);
        self.postMessage({ type: "ready" });
    })
    .catch(function (e) {
        loadError = String(e);
        self.postMessage({ type: "load-error", message: loadError });
    });

self.onmessage = function (ev) {
    var msg = ev.data;
    if (msg.type !== "compile") return;
    if (!compilerModule) {
        self.postMessage({
            type: "error",
            stderr: loadError || "compiler still loading",
        });
        return;
    }

    var stderr = "";
    try {
        var wasi = createWASI({
            args: ["skj-sc", "-o", "out.wasm", "in.scm"],
            files: { "in.scm": msg.source },
            onStderr: function (s) { stderr += s; },
        });
        var inst = new WebAssembly.Instance(compilerModule, {
            wasi_snapshot_preview1: wasi.imports,
        });
        var rc = wasi.start(inst);
        var out = wasi.files["out.wasm"];
        if (rc === 0 && out && out.length > 0) {
            // transfer the bytes to avoid a copy
            self.postMessage(
                { type: "compiled", wasm: out.buffer, stderr: stderr },
                [out.buffer]
            );
        } else {
            self.postMessage({
                type: "error",
                code: rc,
                stderr: stderr || ("compiler exited with code " + rc),
            });
        }
    } catch (e) {
        self.postMessage({ type: "error", stderr: stderr + "\n" + String(e) });
    }
};

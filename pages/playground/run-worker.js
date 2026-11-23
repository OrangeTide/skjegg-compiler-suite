// run-worker.js : runs a compiled program module (the .wasm the compiler
// produced) in a Web Worker and streams its stdout/stderr back.  A TinScheme
// program is a complete WASI module that writes through fd_write and exits via
// proc_exit, so the same WASI shim serves it with no memfs -- just stdout and
// stderr capture.
//
// Note: a program that uses delimited continuations (shift/reset) emits the
// WasmFX stack-switching instructions, which not every browser engine supports
// yet; such a module may fail to instantiate here even though it compiled.  A
// plain program runs anywhere WebAssembly does.

/* global createWASI */
importScripts("wasi-shim.js");

self.onmessage = function (ev) {
    var msg = ev.data;
    if (msg.type !== "run") return;

    var outChunks = [];
    var stderr = "";
    try {
        var wasi = createWASI({
            args: ["program"],
            onStdout: function (bytes) { outChunks.push(bytes); },
            onStderr: function (s) { stderr += s; },
        });
        var module = new WebAssembly.Module(new Uint8Array(msg.wasm));
        var inst = new WebAssembly.Instance(module, {
            wasi_snapshot_preview1: wasi.imports,
        });
        var code = wasi.start(inst);

        var total = 0, i;
        for (i = 0; i < outChunks.length; i++) total += outChunks[i].length;
        var all = new Uint8Array(total), off = 0;
        for (i = 0; i < outChunks.length; i++) {
            all.set(outChunks[i], off); off += outChunks[i].length;
        }
        self.postMessage({
            type: "done",
            code: code,
            stdout: new TextDecoder().decode(all),
            stderr: stderr,
        });
    } catch (e) {
        self.postMessage({ type: "run-error", message: String(e), stderr: stderr });
    }
};

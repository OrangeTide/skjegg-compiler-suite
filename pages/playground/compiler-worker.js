// compiler-worker.js : runs a self-hosted skjegg front-end compiler (skj-*.wasm)
// in a Web Worker.  It is language-neutral: each "compile" message names the
// compiler wasm to use, the tool name and argv, and the source.  The worker
// caches each compiler module by URL, writes the source into the WASI memfs,
// runs the compiler with argv [tool, ...args, "-o", outName, srcName], and posts
// back the produced bytes (a complete module for TinScheme, a relocatable object
// for the object+link languages) or the stderr diagnostics on a nonzero exit.
//
// The object+link languages hand their object to link-worker.js next; TinScheme
// emits a complete module that run-worker.js runs directly.

/* global createWASI */
importScripts("wasi-shim.js");

// url -> WebAssembly.Module, or the in-flight Promise while it loads.
var moduleCache = {};

function getModule(url) {
    if (moduleCache[url]) return moduleCache[url];
    var p = fetch(url)
        .then(function (r) {
            if (!r.ok) throw new Error(url + ": HTTP " + r.status);
            return r.arrayBuffer();
        })
        .then(function (buf) {
            var m = new WebAssembly.Module(buf);
            moduleCache[url] = m;
            return m;
        });
    moduleCache[url] = p;
    return p;
}

self.onmessage = function (ev) {
    var msg = ev.data;
    if (msg.type !== "compile") return;

    var stderr = "";
    Promise.resolve(getModule(msg.compilerUrl)).then(function (mod) {
        var files = {};
        files[msg.srcName] = msg.source;
        var argv = [msg.toolName]
            .concat(msg.args || [])
            .concat(["-o", msg.outName, msg.srcName]);
        var wasi = createWASI({
            args: argv,
            files: files,
            onStderr: function (s) { stderr += s; },
        });
        var inst = new WebAssembly.Instance(mod, {
            wasi_snapshot_preview1: wasi.imports,
        });
        var rc = wasi.start(inst);
        var out = wasi.files[msg.outName];
        if (rc === 0 && out && out.length > 0) {
            self.postMessage(
                { type: "compiled", bytes: out.buffer, link: !!msg.link,
                  stderr: stderr },
                [out.buffer]
            );
        } else {
            self.postMessage({
                type: "error",
                code: rc,
                stderr: stderr || ("compiler exited with code " + rc),
            });
        }
    }).catch(function (e) {
        self.postMessage({ type: "error", stderr: stderr + "\n" + String(e) });
    });
};

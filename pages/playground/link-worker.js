// link-worker.js : runs the self-hosted skjegg WASM linker (skj-ld.wasm) in a
// Web Worker.  The object+link languages (BASIC, Pascal, MooScript) compile to a
// relocatable WASM object; this worker links that object against the language's
// runtime objects into a complete module that run-worker.js runs.
//
// A "link" message carries the compiled object bytes and the list of runtime
// objects (url + memfs name).  The worker caches the linker module and each
// runtime object by URL, writes them all into the WASI memfs, runs the linker
// with argv ["skj-ld", "-o", outName, objName, ...runtimeNames], and posts back
// the produced module bytes or the stderr diagnostics on a nonzero exit.

/* global createWASI */
importScripts("wasi-shim.js");

var moduleCache = {};   // url -> WebAssembly.Module | Promise
var bytesCache = {};    // url -> Uint8Array | Promise

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

function getBytes(url) {
    if (bytesCache[url]) return bytesCache[url];
    var p = fetch(url)
        .then(function (r) {
            if (!r.ok) throw new Error(url + ": HTTP " + r.status);
            return r.arrayBuffer();
        })
        .then(function (buf) {
            var u = new Uint8Array(buf);
            bytesCache[url] = u;
            return u;
        });
    bytesCache[url] = p;
    return p;
}

self.onmessage = function (ev) {
    var msg = ev.data;
    if (msg.type !== "link") return;

    var stderr = "";
    var runtime = msg.runtime || [];
    var loads = [Promise.resolve(getModule(msg.linkerUrl))].concat(
        runtime.map(function (rt) {
            return Promise.resolve(getBytes(rt.url)).then(function (b) {
                return { name: rt.name, bytes: b };
            });
        })
    );

    Promise.all(loads).then(function (res) {
        var mod = res[0];
        var rts = res.slice(1);
        var files = {};
        files[msg.objName] = new Uint8Array(msg.objBytes);
        var names = [msg.objName];
        rts.forEach(function (rt) {
            files[rt.name] = rt.bytes;
            names.push(rt.name);
        });
        var argv = [msg.toolName, "-o", msg.outName].concat(names);
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
                { type: "linked", bytes: out.buffer, stderr: stderr },
                [out.buffer]
            );
        } else {
            self.postMessage({
                type: "error",
                code: rc,
                stderr: stderr || ("linker exited with code " + rc),
            });
        }
    }).catch(function (e) {
        self.postMessage({ type: "error", stderr: stderr + "\n" + String(e) });
    });
};

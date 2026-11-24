// link-worker.js : runs the self-hosted skjegg WASM linker (skj-ld.wasm) in a
// Web Worker.  The object+link languages (BASIC, Pascal, MooScript) compile to a
// relocatable WASM object; this worker links that object against the language's
// runtime objects into a complete module that run-worker.js runs.
//
// A "link" message carries the compiled object bytes and either a list of
// runtime objects (url + memfs name) or, for a bulkier runtime like C's libc, a
// single gzipped `ar` archive (msg.archive): one fetch, compressed, that the
// worker gunzips to the `.a` and hands whole to the linker.  skj-ld.wasm reads
// archives and pulls only the members a program actually references, so the
// worker writes the archive as one memfs file and lets the linker dead-strip;
// it no longer splits the archive itself.  Either way the worker runs the linker
// with argv ["skj-ld", "-o", outName, objName, ...inputs] and posts back the
// produced module bytes or the stderr diagnostics on a nonzero exit.

/* global createWASI */
importScripts("wasi-shim.js");

var moduleCache = {};   // url -> WebAssembly.Module | Promise
var bytesCache = {};    // url -> Uint8Array | Promise
var archiveCache = {};  // url -> Uint8Array (the gunzipped .a) | Promise

// Decompress a gzip stream with the platform's built-in DecompressionStream.
function gunzip(u8) {
    var ds = new DecompressionStream("gzip");
    var stream = new Blob([u8]).stream().pipeThrough(ds);
    return new Response(stream).arrayBuffer().then(function (buf) {
        return new Uint8Array(buf);
    });
}

// Fetch and gunzip a `.a.gz`, returning the whole `ar` archive bytes.  The
// linker parses the archive and selects members, so no splitting happens here.
function getArchive(url) {
    if (archiveCache[url]) return archiveCache[url];
    var p = fetch(url).then(function (r) {
        if (!r.ok) throw new Error(url + ": HTTP " + r.status);
        return r.arrayBuffer();
    }).then(function (buf) {
        return gunzip(new Uint8Array(buf));
    }).then(function (ar) {
        archiveCache[url] = ar;
        return ar;
    });
    archiveCache[url] = p;
    return p;
}

// The memfs name for an archive url: its basename without the `.gz` (rt/c.a.gz
// -> c.a).  Matches what index.html prints in the console.
function archiveName(url) {
    return url.replace(/^.*\//, "").replace(/\.gz$/, "");
}

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
    // The runtime inputs come either as one gzipped ar archive (one fetch, the
    // linker pulls what it needs) or as a list of individually-served objects.
    var rtsPromise = msg.archive
        ? Promise.resolve(getArchive(msg.archive)).then(function (ar) {
              return [{ name: archiveName(msg.archive), bytes: ar }];
          })
        : Promise.all(runtime.map(function (rt) {
              return Promise.resolve(getBytes(rt.url)).then(function (b) {
                  return { name: rt.name, bytes: b };
              });
          }));

    Promise.all([Promise.resolve(getModule(msg.linkerUrl)), rtsPromise])
      .then(function (res) {
        var mod = res[0];
        var rts = res[1];
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

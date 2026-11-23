// link-worker.js : runs the self-hosted skjegg WASM linker (skj-ld.wasm) in a
// Web Worker.  The object+link languages (BASIC, Pascal, MooScript) compile to a
// relocatable WASM object; this worker links that object against the language's
// runtime objects into a complete module that run-worker.js runs.
//
// A "link" message carries the compiled object bytes and either a list of
// runtime objects (url + memfs name) or, for a bulkier runtime like C's libc, a
// single gzipped `ar` archive (msg.archive): one fetch, compressed, that the
// worker gunzips and splits into its members.  Either way the worker writes the
// objects into the WASI memfs, runs the linker with argv
// ["skj-ld", "-o", outName, objName, ...memberNames], and posts back the
// produced module bytes or the stderr diagnostics on a nonzero exit.
// skj-ld.wasm does not read archives itself, so the split happens here.

/* global createWASI */
importScripts("wasi-shim.js");

var moduleCache = {};   // url -> WebAssembly.Module | Promise
var bytesCache = {};    // url -> Uint8Array | Promise
var archiveCache = {};  // url -> [{name, bytes}] | Promise

// Decompress a gzip stream with the platform's built-in DecompressionStream.
function gunzip(u8) {
    var ds = new DecompressionStream("gzip");
    var stream = new Blob([u8]).stream().pipeThrough(ds);
    return new Response(stream).arrayBuffer().then(function (buf) {
        return new Uint8Array(buf);
    });
}

// Parse a GNU `ar` archive into its object members.  The `/`-index and the `//`
// long-name table are skipped; a `/<offset>` name is resolved against that table.
function parseAr(b) {
    var dec = new TextDecoder();
    if (dec.decode(b.subarray(0, 8)) !== "!<arch>\n")
        throw new Error("not an ar archive");
    var members = [], longnames = null, p = 8;
    function fld(o, n) { return dec.decode(b.subarray(o, o + n)).trim(); }
    while (p + 60 <= b.length) {
        var raw = fld(p, 16);
        var size = parseInt(fld(p + 48, 10), 10);
        var data = b.subarray(p + 60, p + 60 + size);
        if (raw === "//") {
            longnames = dec.decode(data);
        } else if (raw !== "/" && raw !== "/SYM64/") {
            var nm = raw;
            if (raw[0] === "/" && longnames) {
                var off = parseInt(raw.slice(1), 10);
                nm = longnames.slice(off).split("\n")[0].replace(/\/$/, "");
            } else {
                nm = raw.replace(/\/$/, "");
            }
            members.push({ name: nm, bytes: data.slice() });
        }
        p += 60 + size + (size & 1);   // members are 2-byte aligned
    }
    return members;
}

function getArchive(url) {
    if (archiveCache[url]) return archiveCache[url];
    var p = fetch(url).then(function (r) {
        if (!r.ok) throw new Error(url + ": HTTP " + r.status);
        return r.arrayBuffer();
    }).then(function (buf) {
        return gunzip(new Uint8Array(buf));
    }).then(function (ar) {
        var members = parseAr(ar);
        archiveCache[url] = members;
        return members;
    });
    archiveCache[url] = p;
    return p;
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
    // The runtime objects come either from a gzipped ar archive (one fetch) or
    // from a list of individually-served objects.
    var rtsPromise = msg.archive
        ? Promise.resolve(getArchive(msg.archive))
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

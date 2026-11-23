// wasi-shim.js : a minimal WASI preview1 host with an in-memory filesystem.
//
// It is just enough to run skjegg's own CLI tools (built as WASM modules) in a
// browser or under node: argv, a single preopened directory ".", and seekable
// files held in memory.  The skj-sc.wasm compiler opens its input for reading,
// writes its output module, and prints diagnostics to stderr -- nothing else.
//
// Usage:
//   var wasi = createWASI({
//       args: ["skj-sc", "-o", "out.wasm", "in.scm"],
//       files: { "in.scm": sourceBytesOrString },
//       onStdout: function (bytes) { ... },   // optional
//       onStderr: function (text)  { ... },   // optional
//   });
//   var inst = new WebAssembly.Instance(module, { wasi_snapshot_preview1: wasi.imports });
//   wasi.start(inst);                         // runs _start, returns exit code
//   var out = wasi.files["out.wasm"];         // Uint8Array written by the program
//
// The implementation is deliberately small and dependency-free so it runs the
// same in a Web Worker and in a node test harness.

(function (root) {
    "use strict";

    // WASI errno values we use.
    var ESUCCESS = 0, EBADF = 8, EINVAL = 28, ENOENT = 44, ENOSYS = 52;

    function toBytes(v) {
        if (v == null) return new Uint8Array(0);
        if (typeof v === "string") return new TextEncoder().encode(v);
        if (v instanceof Uint8Array) return v.slice();
        if (v instanceof ArrayBuffer) return new Uint8Array(v.slice(0));
        return new Uint8Array(v);
    }

    function createWASI(opts) {
        opts = opts || {};
        var args = opts.args || ["prog"];
        var onStdout = opts.onStdout || function () {};
        var onStderr = opts.onStderr || function () {};

        // The memfs: name -> Uint8Array.  Seed from opts.files.
        var files = {};
        if (opts.files) {
            for (var k in opts.files)
                if (Object.prototype.hasOwnProperty.call(opts.files, k))
                    files[k] = toBytes(opts.files[k]);
        }

        // Open file descriptors.  0/1/2 are std streams, 3 is the preopen dir,
        // real files start at 4.  Each open file holds a growable byte list and
        // a position; on close it is flushed back into `files`.
        var PREOPEN_FD = 3, DIR_NAME = ".";
        var fds = {};              // fd -> { name, data:[], pos, writable }
        var nextFd = 4;

        var exitCode = 0, exited = false;
        var mem = null;            // set once the instance exists

        function WasiExit(code) { this.code = code; }

        function dv() { return new DataView(mem.buffer); }
        function u8() { return new Uint8Array(mem.buffer); }

        function readStr(ptr, len) {
            return new TextDecoder().decode(u8().subarray(ptr, ptr + len));
        }

        // normalize "./in.scm" / "in.scm" -> "in.scm"
        function norm(p) {
            p = p.replace(/^\.\//, "");
            return p.replace(/^\/+/, "");
        }

        var imports = {
            args_sizes_get: function (argcPtr, bufSizePtr) {
                var size = 0;
                for (var i = 0; i < args.length; i++)
                    size += new TextEncoder().encode(args[i]).length + 1;
                dv().setUint32(argcPtr, args.length, true);
                dv().setUint32(bufSizePtr, size, true);
                return ESUCCESS;
            },
            args_get: function (argvPtr, bufPtr) {
                var d = dv(), m = u8(), p = bufPtr;
                for (var i = 0; i < args.length; i++) {
                    d.setUint32(argvPtr + i * 4, p, true);
                    var b = new TextEncoder().encode(args[i]);
                    m.set(b, p); p += b.length;
                    m[p++] = 0;
                }
                return ESUCCESS;
            },

            fd_prestat_get: function (fd, prestatPtr) {
                if (fd !== PREOPEN_FD) return EBADF;
                var d = dv();
                d.setUint8(prestatPtr, 0);                 // tag: dir
                d.setUint32(prestatPtr + 4,
                    new TextEncoder().encode(DIR_NAME).length, true);
                return ESUCCESS;
            },
            fd_prestat_dir_name: function (fd, pathPtr, pathLen) {
                if (fd !== PREOPEN_FD) return EBADF;
                var b = new TextEncoder().encode(DIR_NAME);
                u8().set(b.subarray(0, pathLen), pathPtr);
                return ESUCCESS;
            },
            fd_fdstat_set_flags: function () { return ESUCCESS; },
            fd_fdstat_get: function () { return ESUCCESS; },

            path_open: function (dirfd, dirflags, pathPtr, pathLen, oflags,
                                 rightsBase, rightsInh, fdflags, openedFdPtr) {
                var name = norm(readStr(pathPtr, pathLen));
                var creat = (oflags & 0x1) !== 0;
                var trunc = (oflags & 0x8) !== 0;
                var have = Object.prototype.hasOwnProperty.call(files, name);
                if (!have && !creat) return ENOENT;
                var bytes;
                if (trunc || (!have && creat)) bytes = [];
                else { bytes = Array.prototype.slice.call(files[name] || []); }
                var fd = nextFd++;
                fds[fd] = { name: name, data: bytes, pos: 0, writable: true };
                dv().setUint32(openedFdPtr, fd, true);
                return ESUCCESS;
            },

            fd_read: function (fd, iovsPtr, iovsLen, nreadPtr) {
                var f = fds[fd];
                if (!f) return EBADF;
                var d = dv(), m = u8(), total = 0;
                for (var i = 0; i < iovsLen; i++) {
                    var base = iovsPtr + i * 8;
                    var buf = d.getUint32(base, true);
                    var len = d.getUint32(base + 4, true);
                    var n = Math.min(len, f.data.length - f.pos);
                    for (var j = 0; j < n; j++) m[buf + j] = f.data[f.pos + j];
                    f.pos += n; total += n;
                    if (n < len) break;             // EOF
                }
                d.setUint32(nreadPtr, total, true);
                return ESUCCESS;
            },

            fd_write: function (fd, iovsPtr, iovsLen, nwrittenPtr) {
                var d = dv(), m = u8(), total = 0, chunks = [];
                for (var i = 0; i < iovsLen; i++) {
                    var base = iovsPtr + i * 8;
                    var buf = d.getUint32(base, true);
                    var len = d.getUint32(base + 4, true);
                    chunks.push(m.slice(buf, buf + len));
                    total += len;
                }
                if (fd === 1 || fd === 2) {
                    for (var c = 0; c < chunks.length; c++) {
                        if (fd === 2) onStderr(new TextDecoder().decode(chunks[c]));
                        else onStdout(chunks[c]);
                    }
                } else {
                    var f = fds[fd];
                    if (!f) return EBADF;
                    for (var k = 0; k < chunks.length; k++) {
                        var ch = chunks[k];
                        for (var j2 = 0; j2 < ch.length; j2++)
                            f.data[f.pos + j2] = ch[j2];
                        f.pos += ch.length;
                    }
                }
                d.setUint32(nwrittenPtr, total, true);
                return ESUCCESS;
            },

            fd_seek: function (fd, offset, whence, newOffsetPtr) {
                var f = fds[fd];
                if (!f) return EBADF;
                var off = typeof offset === "bigint" ? Number(offset) : offset;
                var base = whence === 1 ? f.pos : whence === 2 ? f.data.length : 0;
                var np = base + off;
                if (np < 0) return EINVAL;
                f.pos = np;
                dv().setBigUint64(newOffsetPtr, BigInt(np), true);
                return ESUCCESS;
            },

            fd_close: function (fd) {
                var f = fds[fd];
                if (!f) return EBADF;
                if (f.writable) files[f.name] = Uint8Array.from(f.data);
                delete fds[fd];
                return ESUCCESS;
            },

            fd_filestat_get: function (fd, bufPtr) {
                var f = fds[fd];
                if (!f) return EBADF;
                writeFilestat(bufPtr, f.data.length);
                return ESUCCESS;
            },
            path_filestat_get: function (dirfd, flags, pathPtr, pathLen, bufPtr) {
                var name = norm(readStr(pathPtr, pathLen));
                if (!Object.prototype.hasOwnProperty.call(files, name))
                    return ENOENT;
                writeFilestat(bufPtr, files[name].length);
                return ESUCCESS;
            },

            // not exercised by a single compile; fail loudly rather than corrupt
            fd_readdir: function () { return ENOSYS; },
            path_create_directory: function () { return ENOSYS; },
            path_remove_directory: function () { return ENOSYS; },
            path_rename: function () { return ENOSYS; },
            path_unlink_file: function () { return ENOSYS; },

            // fill a buffer with random bytes (BASIC's RNG runtime calls this).
            random_get: function (bufPtr, bufLen) {
                var m = u8().subarray(bufPtr, bufPtr + bufLen);
                var c = (typeof crypto !== "undefined" && crypto.getRandomValues)
                    ? crypto : (typeof self !== "undefined" ? self.crypto : null);
                if (c && c.getRandomValues) {
                    // getRandomValues caps at 65536 bytes per call
                    for (var off = 0; off < m.length; off += 65536)
                        c.getRandomValues(m.subarray(off, Math.min(off + 65536, m.length)));
                } else {
                    for (var i = 0; i < m.length; i++) m[i] = (Math.random() * 256) | 0;
                }
                return ESUCCESS;
            },

            // benign time/env stubs: present so a program that references them
            // links and runs; none of the playground samples depend on real values.
            clock_time_get: function (id, precision, timePtr) {
                var ns = BigInt(Math.round(Date.now() * 1e6));
                dv().setBigUint64(timePtr, ns, true);
                return ESUCCESS;
            },
            environ_sizes_get: function (countPtr, bufSizePtr) {
                dv().setUint32(countPtr, 0, true);
                dv().setUint32(bufSizePtr, 0, true);
                return ESUCCESS;
            },
            environ_get: function () { return ESUCCESS; },

            proc_exit: function (code) {
                exitCode = code; exited = true;
                throw new WasiExit(code);
            },
        };

        function writeFilestat(bufPtr, size) {
            var d = dv();
            for (var i = 0; i < 64; i++) d.setUint8(bufPtr + i, 0);
            d.setUint8(bufPtr + 16, 4);                 // filetype: regular file
            d.setBigUint64(bufPtr + 32, BigInt(size), true);  // st_size
        }

        return {
            imports: imports,
            files: files,
            start: function (instance) {
                mem = instance.exports.memory;
                try {
                    instance.exports._start();
                } catch (e) {
                    if (!(e instanceof WasiExit)) throw e;
                }
                return exitCode;
            },
            get exitCode() { return exitCode; },
            get exited() { return exited; },
        };
    }

    root.createWASI = createWASI;
    if (typeof module !== "undefined" && module.exports)
        module.exports = { createWASI: createWASI };
})(typeof self !== "undefined" ? self : this);

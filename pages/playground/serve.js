// serve.js : a tiny static file server for previewing the built playground.
//
// The playground must be served over http (file:// cannot spawn the workers or
// fetch the wasm modules).  This has no dependencies so it runs with the same
// node the build already uses.
//
//   node serve.js [port]        # from build/playground/, defaults to 8080

"use strict";

var http = require("http");
var fs = require("fs");
var path = require("path");

var port = parseInt(process.argv[2], 10) || 8080;
var root = process.cwd();
var TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".wasm": "application/wasm",
    ".css": "text/css; charset=utf-8",
    ".o": "application/octet-stream",
};

http.createServer(function (req, res) {
    var rel = decodeURIComponent(req.url.split("?")[0]);
    if (rel.endsWith("/")) rel += "index.html";
    // resolve inside root, reject path escapes
    var full = path.normalize(path.join(root, rel));
    if (full.indexOf(root) !== 0) { res.writeHead(403); res.end(); return; }
    fs.readFile(full, function (err, data) {
        if (err) { res.writeHead(404); res.end("not found\n"); return; }
        res.writeHead(200,
            { "content-type": TYPES[path.extname(full)] || "application/octet-stream" });
        res.end(data);
    });
}).listen(port, function () {
    process.stdout.write("serving " + root + " at http://localhost:" + port + "/\n");
});

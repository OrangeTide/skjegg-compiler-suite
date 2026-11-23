// gen-bundle.js : pack a directory tree of files into a playground bundle.
//
// A bundle is the general mechanism by which the browser playground ships files
// alongside a language: the compiler worker seeds each file into its WASI memfs
// (so the compiler can #include a header), and the editor can open an
// editor-marked file read-only.  One bundle is a JSON document:
//
//   { "files": [ { "path": "include/stdio.h", "content": "...", "editor": true } ] }
//
// "path" is the file's memfs path (what the compiler opens, e.g. with -Iinclude)
// and the key the editor lists it under.  "editor" marks a file the user may
// open read-only in a pane.  Content is inlined so a bundle is a single fetch.
//
// Usage:
//   node gen-bundle.js <out.json> <memfs-prefix> <src-dir>
//
// Every regular file under <src-dir> becomes one entry whose path is
// <memfs-prefix> joined with the file's path relative to <src-dir>.  All entries
// are editor-visible; a header the user never opens is harmless to list.

"use strict";

var fs = require("fs");
var path = require("path");

function walk(dir, rel, out) {
    var ents = fs.readdirSync(dir, { withFileTypes: true });
    ents.sort(function (a, b) { return a.name < b.name ? -1 : 1; });
    ents.forEach(function (e) {
        var full = path.join(dir, e.name);
        var r = rel ? rel + "/" + e.name : e.name;
        if (e.isDirectory()) {
            walk(full, r, out);
        } else if (e.isFile()) {
            out.push({ rel: r, content: fs.readFileSync(full, "utf8") });
        }
    });
}

function main() {
    var argv = process.argv.slice(2);
    if (argv.length !== 3) {
        process.stderr.write(
            "usage: node gen-bundle.js <out.json> <memfs-prefix> <src-dir>\n");
        process.exit(2);
    }
    var outPath = argv[0], prefix = argv[1], srcDir = argv[2];
    var entries = [];
    walk(srcDir, "", entries);
    var files = entries.map(function (e) {
        return {
            path: prefix ? prefix + "/" + e.rel : e.rel,
            content: e.content,
            editor: true,
        };
    });
    fs.mkdirSync(path.dirname(outPath), { recursive: true });
    fs.writeFileSync(outPath, JSON.stringify({ files: files }) + "\n");
    process.stderr.write(
        "gen-bundle: " + files.length + " files -> " + outPath + "\n");
}

main();

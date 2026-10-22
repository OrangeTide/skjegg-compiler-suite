#!/bin/sh
# Generate the Skjegg manual landing page (index.html) to stdout.
#
# The page is organized at the top level by Languages, Tools, and Internals,
# mirroring the doc/ subdirectories.  Each entry links to the rendered HTML,
# the source Markdown, and (when built) the PDF.  Titles come from each
# document's first level-1 heading, so a new doc/<category>/<name>.md appears
# in the manual with no edit here.
#
# Run from the doc/ directory.  Usage: gen-index.sh > <out>/index.html

set -eu

# First "# " heading of a markdown file, with any leading "Skjegg "/"Kobold-"
# prefix left intact (used verbatim as the entry title).
doc_title() {
    sed -n 's/^# //p' "$1" | head -1
}

# One <li> for a single markdown file.  Arg: path relative to doc/.
emit_one() {
    _md=$1
    [ -e "$_md" ] || return 0
    _rel=${_md#./}
    _title=$(doc_title "$_md")
    [ -n "$_title" ] || _title=$(basename "$_md" .md)
    _html=${_rel%.md}.html
    _pdf=${_rel%.md}.pdf
    printf '      <li>\n'
    printf '        <a class="doc-link" href="%s">%s</a>\n' "$_html" "$_title"
    printf '        <span class="formats">'
    printf '<a href="%s">md</a>' "$_rel"
    printf ' <a class="pdf" href="%s">pdf</a>' "$_pdf"
    printf '</span>\n'
    printf '      </li>\n'
}

# One <li> per markdown file in a directory, sorted by name.  Arg: dir.
emit_entries() {
    for _md in "$1"/*.md; do
        emit_one "$_md"
    done
}

# Emit a directory's files in a curated order (the remaining arguments, as
# basenames), then any file not named, so a new doc still appears (at the end).
# Arg 1: dir.  Args 2..: ordered basenames.
emit_ordered() {
    _dir=$1
    shift
    _named=" $* "
    for _b in "$@"; do
        emit_one "$_dir/$_b"
    done
    for _md in "$_dir"/*.md; do
        [ -e "$_md" ] || continue
        case "$_named" in *" $(basename "$_md") "*) continue;; esac
        emit_one "$_md"
    done
}

cat <<'HEAD'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Skjegg manual</title>
  <link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Sans+Condensed:wght@400;600&family=IBM+Plex+Mono:wght@400&display=swap">
  <style>
    :root { color-scheme: light dark; }
    body {
      font-family: "IBM Plex Sans Condensed", -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      max-width: 820px; margin: 56px auto; padding: 0 20px;
      color: #2b2420; background: #faf8f5; line-height: 1.6;
    }
    h1 { font-family: "IBM Plex Sans", sans-serif; margin-bottom: 0.1em; font-size: 2.1rem; }
    .subtitle { color: #8b7a68; margin-top: 0; font-size: 1.1rem; }
    .lede { color: #5a4f45; }
    h2 { font-family: "IBM Plex Sans", sans-serif; margin-top: 2.2rem;
         padding-bottom: 0.3rem; border-bottom: 2px solid #e7ddd0; font-size: 1.4rem; }
    h3 { margin: 1.4rem 0 0.4rem; font-size: 1.05rem; color: #6b5d4f;
         text-transform: uppercase; letter-spacing: 0.04em; }
    ul { list-style: none; padding: 0; margin: 0; }
    li { padding: 0.45em 0; border-bottom: 1px solid #efe8dd; display: flex;
         justify-content: space-between; align-items: baseline; gap: 1em; }
    li:last-child { border-bottom: none; }
    a { color: #b1541a; text-decoration: none; }
    a:hover { text-decoration: underline; }
    .doc-link { font-weight: 600; }
    .formats { font-family: "IBM Plex Mono", monospace; font-size: 0.75rem;
               white-space: nowrap; color: #b7a894; }
    .formats a { color: #8b7a68; }
    .formats a.pdf { color: #c0392b; }
    footer { margin-top: 3rem; color: #a89a88; font-size: 0.85rem; }
    @media (prefers-color-scheme: dark) {
      body { color: #e7ddd0; background: #1c1917; }
      .subtitle, h3 { color: #b7a894; }
      .lede { color: #c9bdae; }
      h2 { border-color: #3a332c; }
      li { border-color: #2c2622; }
      a { color: #e8863f; }
      .formats, .formats a { color: #8b7a68; }
    }
  </style>
</head>
<body>
  <h1>Skjegg</h1>
  <p class="subtitle">A retargetable compiler toolkit</p>
  <p class="lede">
    Skjegg is a retargetable compiler toolkit: a shared three-address
    intermediate representation, multiple code generators, and multiple
    language front ends. This manual is the reference for its languages,
    its tools, and its internals. The source of truth for every page is the
    Markdown in the project's <code>doc/</code> tree.
  </p>
HEAD

printf '\n  <h2>Languages</h2>\n  <ul>\n'
emit_entries languages
printf '  </ul>\n'
printf '\n  <h3>Kobold-Pascal</h3>\n  <ul>\n'
emit_ordered languages/kobold \
    kp-guide.md kp-tutorial.md kp-examples.md kp-grammar.md kp-language.md \
    kp-units.md kp-errors.md kp-diagnostics.md kp-conformance.md \
    kp-module-format.md kp-abi.md kp-embedding.md kp-hardening.md \
    kp-property.md kobold.md
printf '  </ul>\n'

printf '\n  <h2>Tools</h2>\n  <ul>\n'
emit_ordered tools \
    toolchain.md emulator.md debugger.md jit.md libc.md mips.md
printf '  </ul>\n'

printf '\n  <h2>Internals</h2>\n  <ul>\n'
emit_entries internals
printf '  </ul>\n'

cat <<'FOOT'

  <footer>
    <a href="https://github.com/OrangeTide/skjegg-compiler-suite">github.com/OrangeTide/skjegg-compiler-suite</a>
  </footer>
</body>
</html>
FOOT

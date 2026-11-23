// highlight.js : a tiny, dependency-free syntax highlighter for the playground
// editor.  It tokenizes a source string for one of the playground languages and
// returns HTML with <span class="tok-..."> wrappers that the editor overlays
// behind a transparent textarea.  It is deliberately approximate: good enough to
// read code at a glance, not a parser.
//
// Exposes window.SkjHighlight.highlight(code, langId) -> HTML string.
// Token classes (styled in index.html): tok-com, tok-str, tok-num, tok-kw,
// tok-pre (preprocessor / reader directive).

(function () {
    "use strict";

    // Per-language scanner config.  linec: line-comment lead strings.  blockc:
    // [open, close] pairs.  quotes: string delimiters.  kw: keyword set.  ci:
    // case-insensitive keywords.  pre: a RegExp for a whole-line directive
    // (C's #..., or null).  remword: a keyword that comments the rest of a line
    // (BASIC's REM).
    function set(words) {
        var s = {};
        words.forEach(function (w) { s[w] = 1; });
        return s;
    }

    var C_KW = set((
        "auto break case char const continue default do double else enum extern " +
        "float for goto if inline int long register restrict return short signed " +
        "sizeof static struct switch typedef union unsigned void volatile while " +
        "_Bool _Float16 va_list size_t"
    ).split(" "));

    // Pascal and BASIC are case-insensitive, so their sets are stored upper-case
    // to match the upper-cased lookup key.
    var PAS_KW = set((
        "and begin boolean break byte case char const continue div do downto else " +
        "end false for forward function if in integer mod not of or procedure " +
        "program repeat set shl sizeof shr then to true type until var while with " +
        "string writeln write readln read"
    ).toUpperCase().split(" "));

    var BAS_KW = set((
        "ABS AND APPEND AS ASC AT ATN ATN2 BAND BIN BNOT BOR BXOR BYTE CASE CEIL " +
        "CHR CLAMP CLEAR CONST COS COSD COUNT DECLARE DIM DIV EACH ELSE ELSEIF END " +
        "ENDCASE ENDENUM ENDFN ENDIF ENDPROC ENDTYPE ENDWHILE ENUM EXIT EXPORT " +
        "EXTERNAL FIX FLOAT FLOOR FN FOR FROM HEX IF IMPORT IN INPUT INSTR INT " +
        "LCASE LEFT LEN LERP LJUST MAX MID MIN MOD MODULE NEXT NOT OF OR PRINT " +
        "PROC REF REM REMOVE REPEAT RETURN RIGHT RJUST RND ROUND SEED SGN SHL SHR " +
        "SIN SIND SPACE SQR STEP STR STRING TAB TAN TAND THEN TIME TO TYPE UCASE " +
        "UNTIL VAL VALINT VECTOR WAIT WHEN WHILE ZERO"
    ).split(" "));

    var MOO_KW = set((
        "verb endverb func endfunc var const if elseif else endif for in while " +
        "endfor endwhile break continue return defer enddefer interface " +
        "endinterface is as switch endswitch case panic recover trace module " +
        "import export extern true false nil int str obj bool err"
    ).split(" "));

    var SCM_KW = set((
        "define define-syntax lambda let let* letrec cond case else if when unless " +
        "and or not begin do delay set! quote quasiquote unquote named-lambda " +
        "display newline write write-string"
    ).split(" "));

    var LANGS = {
        c:      { linec: ["//"], blockc: [["/*", "*/"]], quotes: ['"', "'"],
                  kw: C_KW, ci: false, pre: /^[ \t]*#[^\n]*/ },
        pascal: { linec: ["//"], blockc: [["{", "}"], ["(*", "*)"]], quotes: ["'"],
                  kw: PAS_KW, ci: true, pre: null },
        basic:  { linec: ["'"], blockc: [], quotes: ['"'],
                  kw: BAS_KW, ci: true, pre: null, remword: "REM" },
        moo:    { linec: ["//"], blockc: [["/*", "*/"]], quotes: ['"'],
                  kw: MOO_KW, ci: false, pre: null },
        scheme: { linec: [";"], blockc: [["#|", "|#"]], quotes: ['"'],
                  kw: SCM_KW, ci: false, pre: /^#[tf]\b/ },
    };

    function esc(s) {
        return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }
    function span(cls, s) { return '<span class="' + cls + '">' + esc(s) + "</span>"; }

    function isIdentStart(c) { return /[A-Za-z_$?!]/.test(c); }
    function isIdent(c) { return /[A-Za-z0-9_$?!.\-]/.test(c); }

    function highlight(code, langId) {
        var L = LANGS[langId];
        if (!L) return esc(code);
        var out = "", i = 0, n = code.length;

        function startsWith(s) { return code.substr(i, s.length) === s; }

        while (i < n) {
            var c = code[i];

            // whitespace / newlines pass through verbatim
            if (c === "\n" || c === "\r" || c === "\t" || c === " ") {
                out += esc(c); i++; continue;
            }

            // preprocessor / reader directive (whole match)
            if (L.pre) {
                L.pre.lastIndex = 0;
                var pm = L.pre.exec(code.slice(i));
                if (pm && pm.index === 0) {
                    out += span("tok-pre", pm[0]); i += pm[0].length; continue;
                }
            }

            // block comments
            var handled = false;
            for (var b = 0; b < L.blockc.length; b++) {
                var open = L.blockc[b][0], close = L.blockc[b][1];
                if (startsWith(open)) {
                    var end = code.indexOf(close, i + open.length);
                    var stop = end < 0 ? n : end + close.length;
                    out += span("tok-com", code.slice(i, stop)); i = stop;
                    handled = true; break;
                }
            }
            if (handled) continue;

            // line comments
            for (var k = 0; k < L.linec.length; k++) {
                if (startsWith(L.linec[k])) {
                    var nl = code.indexOf("\n", i);
                    var to = nl < 0 ? n : nl;
                    out += span("tok-com", code.slice(i, to)); i = to;
                    handled = true; break;
                }
            }
            if (handled) continue;

            // strings (with \-escapes; BASIC's "" is handled as two strings, fine)
            if (L.quotes.indexOf(c) >= 0) {
                var j = i + 1;
                while (j < n && code[j] !== c && code[j] !== "\n") {
                    if (code[j] === "\\") j++;
                    j++;
                }
                if (j < n && code[j] === c) j++;
                out += span("tok-str", code.slice(i, j)); i = j; continue;
            }

            // numbers (decimal, hex, float, with a radix/suffix tail)
            if (/[0-9]/.test(c) ||
                (c === "." && i + 1 < n && /[0-9]/.test(code[i + 1]))) {
                var m = /^(0[xXbB][0-9a-fA-F_]+|[0-9][0-9_]*\.?[0-9_]*([eE][-+]?[0-9]+)?[fFlLuU]*)/
                        .exec(code.slice(i));
                var num = m ? m[0] : c;
                out += span("tok-num", num); i += num.length; continue;
            }

            // identifiers / keywords
            if (isIdentStart(c)) {
                var s = i + 1;
                while (s < n && isIdent(code[s])) s++;
                var word = code.slice(i, s);
                var lookup = L.ci ? word.toUpperCase() : word;
                // case-insensitive keyword sets (Pascal/BASIC) are stored
                // upper-case; the case-sensitive sets keep the written form.
                if (L.remword && word.toUpperCase() === L.remword) {
                    var rnl = code.indexOf("\n", s);
                    var rto = rnl < 0 ? n : rnl;
                    out += span("tok-com", code.slice(i, rto)); i = rto; continue;
                }
                if (L.kw[lookup]) out += span("tok-kw", word);
                else out += esc(word);
                i = s; continue;
            }

            // anything else: one char verbatim
            out += esc(c); i++;
        }
        return out;
    }

    self.SkjHighlight = { highlight: highlight, langs: LANGS };
})();

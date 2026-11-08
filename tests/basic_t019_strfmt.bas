REM string/format helpers and the PI constant: VAL VALINT SPACE$ STRING$
REM HEX$ BIN$ FIX$ ZERO$ RJUST$ LJUST$

PRINT VAL("3.5xyz")
PRINT VALINT("  -42abc")
PRINT "[" + SPACE$(3) + "]"
PRINT "[" + STRING$(3, "ab") + "]"
PRINT HEX$(255)
PRINT HEX$(16)
PRINT BIN$(10)
PRINT FIX$(3.14159, 2)
PRINT FIX$(-2.5, 0)
PRINT ZERO$(42, 5)
PRINT ZERO$(-7, 4)
PRINT "[" + RJUST$("hi", 5) + "]"
PRINT "[" + LJUST$("hi", 5) + "]"
PRINT "[" + RJUST$("toolong", 3) + "]"
PRINT FIX$(PI, 4)
END

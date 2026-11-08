REM string built-in functions
DIM s AS STRING
s = "Hello, World"
PRINT "len="; LEN(s)
PRINT "asc="; ASC(s)
PRINT "chr="; CHR$(65)
PRINT "left="; LEFT$(s, 5)
PRINT "right="; RIGHT$(s, 5)
PRINT "mid="; MID$(s, 8, 3)
PRINT "mid2="; MID$(s, 8)
PRINT "uc="; UCASE$(s)
PRINT "lc="; LCASE$(s)
PRINT "instr="; INSTR(s, "World")
PRINT "instr0="; INSTR(s, "xyz")
PRINT "stri="; STR$(42)
PRINT "strf="; STR$(3.5)
PRINT "cat="; LEFT$(s, 5) + "!"
END

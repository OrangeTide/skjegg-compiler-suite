DIM a AS STRING
DIM b AS STRING
a = "hello"
b = "world"
PRINT a; " "; b
PRINT a + ", " + b + "!"
PRINT "eq="; a = a
PRINT "ne="; a <> b
PRINT "lt="; a < b
PRINT "ge="; b >= a

FN greet(who AS STRING) AS STRING
  RETURN "hi " + who
ENDFN

PROC say(s AS STRING)
  PRINT "say: "; s
ENDPROC

PRINT greet("bob")
say(greet("sue"))
b = a
PRINT "copy="; b
END

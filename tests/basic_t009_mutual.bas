PROC a(n AS INT)
  PRINT "a"; n
  IF n > 0 THEN b(n - 1)
ENDPROC
PROC b(n AS INT)
  PRINT "b"; n
  IF n > 0 THEN a(n - 1)
ENDPROC
a(3)
END

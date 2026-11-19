REM VECTOR OF a record: APPEND, FOR EACH alias, indexed field, whole-element copy
TYPE Bullet
  x AS INT
  y AS INT
  live AS BYTE
ENDTYPE

DIM bullets AS VECTOR OF Bullet
DIM b AS Bullet
DIM i AS INT

REM build five bullets via a scratch record
FOR i = 0 TO 4
  b.x = i
  b.y = i * 10
  b.live = 1
  APPEND bullets, b
NEXT i
PRINT "count="; COUNT(bullets)

REM indexed field read
PRINT "b2="; bullets(2).x; " "; bullets(2).y; " "; bullets(2).live

REM indexed field write
bullets(2).x = 99
PRINT "b2x="; bullets(2).x

REM FOR EACH binds a record alias; field writes reach the element in place
FOR EACH e IN bullets
  e.y = e.y + 1
NEXT
PRINT "ys="; bullets(0).y; " "; bullets(1).y; " "; bullets(4).y

REM whole-element copy: v(i) = r and r = v(i)
b.x = 500
b.y = 600
b.live = 0
bullets(0) = b
PRINT "copy-in="; bullets(0).x; " "; bullets(0).y; " "; bullets(0).live

DIM out AS Bullet
out = bullets(4)
PRINT "copy-out="; out.x; " "; out.y
END

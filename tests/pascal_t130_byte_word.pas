{ Test byte and word types }
program TestByteWord;
var
  b: byte;
  w: word;
begin
  b := 255;
  writeln(b);
  writeln(lo(b));
  writeln(hi(b));

  w := 4660;
  writeln(w);
  writeln(lo(w));
  writeln(hi(w));

  { swap bytes }
  writeln(swap(w));

  { byte arithmetic }
  b := 200;
  writeln(b + 50);

  { word arithmetic }
  w := 60000;
  writeln(w + 5000);
end.

{ Test shift operations }
program TestShift;
var
  x: integer;
begin
  x := 1;
  writeln(x shl 0);
  writeln(x shl 1);
  writeln(x shl 4);
  writeln(x shl 8);
  writeln(x shl 16);

  x := 256;
  writeln(x shr 1);
  writeln(x shr 4);
  writeln(x shr 8);

  { combined }
  x := 170;
  writeln((x shl 1) shr 2);
end.

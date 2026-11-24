{ pi is the built-in real constant, usable unless the program defines its own.
  It feeds ordinary real arithmetic and formatted writes.  This test needs
  host libc math for the real formatter, so it runs on host backends only. }
program PiDemo;
var
  r, area: real;
begin
  writeln(pi:8:4);
  r := 2.0;
  area := pi * r * r;
  writeln(area:8:4)
end.

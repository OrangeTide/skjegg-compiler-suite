#!/usr/bin/env cpas
program comments;
// This is a line comment
var x: integer; // inline comment
begin
  x := 10; // assign ten
  (* parenthesis-star comment *)
  { brace comment }
  writeln(x)
end.

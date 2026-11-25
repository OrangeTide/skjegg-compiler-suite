program pcgc;

{ Two functions, only one reached from the program body.  Built with
  -ffunction-sections each lands in its own .text.<fn>, so a --gc-sections
  link drops Unused while keeping Used. }

function Used(x: Integer): Integer;
begin
  Used := x + 1
end;

function Unused(x: Integer): Integer;
begin
  Unused := x * 7
end;

begin
  WriteLn(Used(41))
end.

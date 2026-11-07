unit dx.base;
{ a leaf unit with a dotted name, whose const and routine an umbrella
  re-exports. The dot in the name resolves to a dx/base.a archive. }

interface

const Half = 2;

function Scale(x: integer): integer;

implementation

function Scale(x: integer): integer;
begin
  Scale := x * Half;
end;

end.

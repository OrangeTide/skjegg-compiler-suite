unit calcu;

interface

const
  Base = 100;

function Add(a, b: integer): integer;
function Mul(a, b: integer): integer;

implementation

function Add(a, b: integer): integer;
begin
  Add := a + b;
end;

function Mul(a, b: integer): integer;
begin
  Mul := a * b;
end;

end.

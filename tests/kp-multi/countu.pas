unit countu;

interface

var
  Counter: integer;
shared var
  Total: int64;

procedure Bump;
function Current: integer;

implementation

procedure Bump;
begin
  Counter := Counter + 1;
  Total := Total + Counter;
end;

function Current: integer;
begin
  Current := Counter;
end;

end.

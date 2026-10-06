program mu_state;
{ a unit's mutable global and a shared var, mutated across the unit boundary. }
uses countu as k;
var
  i: integer;
begin
  for i := 1 to 5 do
    k.Bump;
  WriteLn('Counter=', k.Counter);
  WriteLn('Current=', k.Current);
  WriteLn('Total=', k.Total);
end.

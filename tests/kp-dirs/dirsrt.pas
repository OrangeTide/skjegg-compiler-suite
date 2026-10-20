program dirsrt;
{ Directory reads reached through the `dirs` unit: no per-program external
  declarations, just `uses dirs`.  Lists the directory named by the first
  argument, counting entries and summing their name lengths.  Both totals are
  order-independent, so the result is deterministic whatever order the
  filesystem returns entries in. }
uses dirs;
var
  d: dirs.Directory;
  name: string;
  n, total: integer;
begin
  n := 0;
  total := 0;
  if dirs.OpenDir(ParamStr(1), d) then
  begin
    while dirs.NextEntry(d, name) do
    begin
      n := n + 1;
      total := total + length(name);
    end;
    dirs.CloseDir(d);
  end;
  WriteLn('entries=', n);
  WriteLn('namelen=', total);
end.

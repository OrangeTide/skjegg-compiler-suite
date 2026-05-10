{ Test set membership tests }
program TestSetMembership;
var
  vowels: set of char;
  s: string[20];
  i, count: integer;
begin
  vowels := ['A', 'E', 'I', 'O', 'U'];
  s := 'PROGRAMMING';
  count := 0;
  for i := 1 to length(s) do begin
    if s[i] in vowels then
      count := count + 1;
  end;
  writeln(count);

  { Test with integer set }
  if 5 in [1, 3, 5, 7, 9] then writeln(1) else writeln(0);
  if 4 in [1, 3, 5, 7, 9] then writeln(1) else writeln(0);
end.

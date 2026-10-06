program mu_chain;
{ a transitive dependency: the program uses enumuse, which itself uses enumdep,
  so the whole closure must be found, compiled, and linked in order. }
uses enumuse as u;
begin
  WriteLn('Go=', u.Go);
end.

program mu_overload;
{ Call three same-named overloads exported from a unit. Each resolves by argument
  type to its own body across the unit interface, so a wrong resolution (or a
  symbol collision between the overloads) would print the wrong tag or fail to
  link. Both branches of the integer and boolean bodies run, and the string body
  echoes its argument, so the argument reaches the selected body too.

  The unit is brought in qualified (`as ov`, then `ov.Tag`), so a dotted reference
  must resolve overloads across the unit interface by argument type, the same as a
  bare call does. Each overload emits as ovldep.Tag$N and the consumer calls the
  right symbol. }
uses ovldep as ov;

begin
  writeln(ov.Tag(5));
  writeln(ov.Tag(-3));
  writeln(ov.Tag('hi'));
  writeln(ov.Tag(true));
  writeln(ov.Tag(false));
end.

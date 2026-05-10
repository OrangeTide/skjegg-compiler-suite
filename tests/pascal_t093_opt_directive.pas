program t093_opt_directive;

(* OPT+ / OPT- directives are accepted and do not affect codegen when
   the compiler is built without PEEPHOLE. ON/OFF spellings also work. *)

{$OPT+}
{$OPT-}
{$OPT ON}
{$OPT OFF}

var x: integer;
begin
  {$OPT+}
  x := 1 + 2;
  {$OPT-}
  writeln(x);
end.

unit nadd;
{ A minimal native-backed unit for the optional-unit build mechanism: its glue
  object lives inside nadd.a and its external dependency comes from nadd.link. }
interface
function Calc(a, b: integer): integer;
implementation
function kpnat_calc(a, b: integer): integer; external cdecl;
function Calc(a, b: integer): integer;
begin
  Calc := kpnat_calc(a, b);
end;
end.

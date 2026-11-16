unit ovldep;

interface

{ Three routines share the name Tag, overloaded on parameter type, all exported
  across the unit interface. Unit routine symbols are namespaced by the unit name,
  and an overloaded one takes the disambiguation suffix after the qualifier, so
  these emit as ovldep.Tag$0 / ovldep.Tag$1 / ovldep.Tag$2 and the interface
  carries each as the .routine line's `link` token. The consumer resolves each
  call to its own body by argument type, which checks that the qualified,
  disambiguated symbols round-trip through the module. }
function Tag(n: integer): string;
function Tag(s: string): string;
function Tag(b: boolean): string;

implementation

{ The result is set with exit(value) rather than the function name, since the
  name Tag is overloaded and so ambiguous as a result target. }
function Tag(n: integer): string;
begin
  if n < 0 then exit('int:neg');
  exit('int:nonneg');
end;

function Tag(s: string): string;
begin
  exit('str:' + s);
end;

function Tag(b: boolean): string;
begin
  if b then exit('bool:true');
  exit('bool:false');
end;

end.

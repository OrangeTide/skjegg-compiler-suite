{ `external cdecl` host imports (P6 stage A): a routine declared with no body
  links by the exact source spelling of its name (not lowercased), so it binds
  to a C symbol.  Here the imports are runtime routines every backend provides,
  including a string-returning one (the shape of kpcom's ReadFileStr). }
program ExtImport;
function __pascal_str_from_int(v: LongInt): string; external cdecl;
function __pascal_str_length(s: string): LongInt; external cdecl;
var
  s: string;
begin
  s := __pascal_str_from_int(1234);
  writeln(s);
  writeln(__pascal_str_length(s));
  writeln(__pascal_str_length('hello'));
end.

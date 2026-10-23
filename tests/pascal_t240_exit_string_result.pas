program t240_exit_string_result;

{ Exit(v) sets the function result and must maintain reference counts exactly
  like `funcname := v`.  Returning a borrowed string (an array element or a
  plain variable) through Exit used to store the pointer without retaining it,
  so the caller released a reference it never owned.  Repeated calls then drove
  the shared buffer's reference count to zero and freed a string still held by
  the array, corrupting later reads.  Each getter is called several times with
  the source still live to exercise that path. }

var
  names: array of string;

function nameOf(i: integer): string;
begin
  Exit(names[i]);
end;

function firstOr(i: integer; alt: string): string;
begin
  if i < 0 then Exit(alt);
  Exit(names[i]);
end;

var
  i: integer;
  acc: string;

begin
  SetLength(names, 2);
  names[0] := 'alpha';
  names[1] := 'bravo';

  acc := '';
  for i := 1 to 4 do
    acc := acc + nameOf(0) + '/';
  writeln(acc);

  acc := '';
  for i := 1 to 4 do
    acc := acc + firstOr(1, 'none') + '/';
  writeln(acc);

  writeln(firstOr(-1, 'fallback'));

  { the array elements must be intact after all the borrowed returns }
  writeln(names[0]);
  writeln(names[1]);
end.

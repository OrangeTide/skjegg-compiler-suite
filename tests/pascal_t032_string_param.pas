program t032_string_param;

procedure PrintStr(s: string);
begin
  writeln(s);
end;

procedure PrintLen(s: string);
begin
  writeln(length(s));
end;

procedure ChangeStr(var s: string);
begin
  s := 'changed';
end;

var
  a: string;
begin
  a := 'hello';
  PrintStr(a);
  PrintLen(a);

  ChangeStr(a);
  PrintStr(a);

  a := 'world';
  PrintStr(a);
  PrintLen(a);
end.

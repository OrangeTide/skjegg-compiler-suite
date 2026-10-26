program defertest;
{ `defer STMT` registers STMT for LIFO teardown at the end of its lexical block,
  run on every orderly exit: fall-through, break, continue, and exit (the result
  is computed before the teardown). A trap would skip it, but none occurs here.
  Run under kprun it prints the sequence in defer.expected. }
procedure emit(v: integer); external;

procedure basic;
begin
  defer emit(1);
  defer emit(2);
  emit(3);
end;
{ fall-through runs the block's defers LIFO: 3, 2, 1 }

procedure loopbody;
var i: integer;
begin
  for i := 1 to 3 do begin
    defer emit(i);       { the body is a block, so this runs at each iteration's end }
    emit(10 + i);
  end;
end;
{ 11, 1, 12, 2, 13, 3 }

procedure withbreak;
var i: integer;
begin
  for i := 1 to 5 do begin
    defer emit(100 + i);
    if i = 2 then break; { break runs the body's defers first }
    emit(i);
  end;
  emit(999);
end;
{ 1, 101, 102, 999 }

procedure withcontinue;
var i: integer;
begin
  for i := 1 to 3 do begin
    defer emit(300 + i);
    if i = 2 then continue;  { continue runs the body's defers first }
    emit(i);
  end;
end;
{ 1, 301, 302, 3, 303 }

function withexit: integer;
var i: integer;
begin
  defer emit(777);           { a routine-scope defer }
  for i := 1 to 3 do begin
    defer emit(200 + i);
    if i = 2 then exit(i * 10);  { exit runs ALL defers, innermost first, after the result }
    emit(i);
  end;
  withexit := 0;
end;
{ 1, 201, 202, 777, returns 20 }

begin
  basic;
  loopbody;
  withbreak;
  withcontinue;
  emit(withexit);
end.

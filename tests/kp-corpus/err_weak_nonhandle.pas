{ a `weak` field must be an interface or process (a handle); any other type is rejected }
program err_weak_nonhandle;
type
  TBad = record
    weak n: integer;
  end;
var
  b: TBad;
begin
end.

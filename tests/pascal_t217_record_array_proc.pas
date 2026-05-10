{ Test array of records with procedures }
program TestRecordArrayProc;
type
  Student = record
    grade: integer;
    passed: boolean;
  end;
var
  students: array[1..5] of Student;
  i, total, passCount: integer;
begin
  students[1].grade := 85; students[1].passed := true;
  students[2].grade := 42; students[2].passed := false;
  students[3].grade := 73; students[3].passed := true;
  students[4].grade := 91; students[4].passed := true;
  students[5].grade := 38; students[5].passed := false;

  total := 0;
  passCount := 0;
  for i := 1 to 5 do begin
    total := total + students[i].grade;
    if students[i].passed then
      passCount := passCount + 1;
  end;

  writeln(total div 5);
  writeln(passCount);
end.

program incexpr;
{ The dollar-I include directive splices the named file's text in at the point of
  the directive, resolved against the including file's directory. Each include here
  stands in for a whole expression, a string constant and a number constant. }
const
  Ver = {$I incver.inc};
  Num = {$I incnum.inc};
begin
  WriteLn('Ver=', Ver);
  WriteLn('len=', Length(Ver));
  WriteLn('Num=', Num);
  WriteLn('Num+8=', Num + 8);
end.

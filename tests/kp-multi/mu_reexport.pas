program mu_reexport;
{ one `uses rxfacade` fans out rxbase's Vec, MakeVec, and SumVec through the
  facade's re-export; this program never `uses rxbase` directly. }
uses rxfacade as f;
var
  v, d: f.Vec;
begin
  v := f.MakeVec(3, 4);
  d := f.Doubled(v);
  WriteLn('sum=', f.SumVec(v));
  WriteLn('dsum=', f.SumVec(d));
end.

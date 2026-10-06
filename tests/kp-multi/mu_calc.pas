program mu_calc;
{ cross-unit function calls and a unit const, qualified through `as`. }
uses calcu as c;
begin
  WriteLn('Add(3,4)=', c.Add(3, 4));
  WriteLn('Mul(6,7)=', c.Mul(6, 7));
  WriteLn('Base=', c.Base);
  WriteLn('combo=', c.Add(c.Mul(2, 5), c.Base));
end.

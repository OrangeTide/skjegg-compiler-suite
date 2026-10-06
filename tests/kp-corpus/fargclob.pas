program fargclob;
{ Regression: float register arguments must not clobber one another. Their
  source temps live in xmm1-7, which overlap the xmm0-7 argument registers, so
  loading them one at a time destroys a source a later argument still needs.
  The eight distinct doubles must sum to 36. Run under kprun it prints 36. }
procedure emitf(v: real); external;

function s8(a, b, c, d, e, f, g, h: real): real;
begin
  s8 := a + b + c + d + e + f + g + h;
end;

begin
  emitf(s8(1, 2, 3, 4, 5, 6, 7, 8));
end.

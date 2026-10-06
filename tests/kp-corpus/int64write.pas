program int64write;
{ Regression guard for three Int64 truncations that only bit when the compiler
  ran its own emitted code. Write/WriteLn of an Int64 once truncated through the
  32-bit io_write_int; Val into an Int64 field once stored with a 32-bit SW; and
  a literal above 2^31 whose low word is zero (2^52) was mis-scanned as guest
  code. Run under kprun it prints
  4503599627370496 4503599627370497 9007199254740992. }
type
  TBox = record v: int64; end;
var
  box: TBox;
  x: int64;
  code: integer;
begin
  x := 4503599627370496;                    { 2^52, low 32 bits zero }
  WriteLn(x);                               { full value, not 0 }

  Val('4503599627370497', box.v, code);     { store all 64 bits into a field }
  WriteLn(box.v);                           { 4503599627370497 }

  box.v := box.v - 1;                       { 2^52 }
  box.v := box.v + box.v;                   { 2^53 }
  WriteLn(box.v);                           { 9007199254740992 }
end.

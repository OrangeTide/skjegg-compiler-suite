program mu_subword;
{ a record with sub-word integer fields (byte/word/smallint) exported from a
  unit: the consumer declares a var of the type and reads the fields, so the
  module round-trips the narrow field widths and their signedness. }
uses subworddep as sw;
var
  p: sw.Pixel;
begin
  p := sw.MakePixel(10, 20, 30, 255, 60000, -1000);
  WriteLn('rgba=', p.r, ' ', p.g, ' ', p.b, ' ', p.a);
  WriteLn('depth=', p.depth, ' bias=', p.bias);
end.

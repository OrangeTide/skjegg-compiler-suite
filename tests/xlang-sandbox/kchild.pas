{ The Kobold-Pascal child of the cross-language sandbox demo.

  Compiled by skj-kpc (front end) -> skj-kp-rv (RV32 codegen) -> skj-as-rv ->
  skj-ld-rv over the freestanding crt (kp_crt_rv.S) and libkpaot-rv-free.a,
  whose only host needs are write/read/exit. It sums 1..99, the same
  computation the C child does, so the two markers line up: the point is that a
  Pascal process and a C process are the same kind of thing to the sandbox. }

program kchild;
var
    i, s: integer;
begin
    WriteLn('child[kobold]: running');
    s := 0;
    for i := 1 to 99 do
        s := s + i;
    WriteLn('child[kobold]: sum=', s);
    WriteLn('child[kobold]: 6*7=', 6 * 7);
    WriteLn('child[kobold]: done');
end.

program AlignDirective;
(* Verify the ALIGN directive controls record field alignment.  A byte
   followed by an 8-byte Int64 is laid out at offset 4 under {$ALIGN 4} and
   forced to offset 8 under {$ALIGN 8}, so the record size is observable. *)
type
  {$ALIGN 4}
  TAligned4 = record
    a: byte;
    b: int64;
  end;
  {$ALIGN 8}
  TAligned8 = record
    a: byte;
    b: int64;
  end;
begin
  writeln(sizeof(TAligned4));
  writeln(sizeof(TAligned8))
end.

program AlignDirective;
(* Verify ALIGN directive controls record field alignment.
   string[1] has size 2 so packing is observable. *)
type
  {$ALIGN 1}
  TPacked = record
    a: string[1];
    b: string[1];
    c: string[1];
  end;
  {$ALIGN 4}
  TAligned = record
    a: string[1];
    b: string[1];
    c: string[1];
  end;
  {$ALIGN 8}
  TEight = record
    a: string[1];
    b: string[1];
    c: string[1];
  end;
begin
  writeln(sizeof(TPacked));
  writeln(sizeof(TAligned));
  writeln(sizeof(TEight))
end.

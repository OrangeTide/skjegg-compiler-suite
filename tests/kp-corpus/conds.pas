program conds;
{ Conditional compilation. kpcom defines KOBOLD, so an $IFDEF KOBOLD branch is taken
  and its $ELSE dropped; $IFNDEF is the reverse. $DEFINE and $UNDEF adjust symbols mid
  file, and conditionals nest. Run under kprun it prints 1 2 3 4 5 6. }
procedure emit(v: integer); external;
begin
  {$IFDEF KOBOLD}
  emit(1);
  {$ELSE}
  emit(91);
  {$ENDIF}

  {$IFNDEF KOBOLD}
  emit(92);
  {$ELSE}
  emit(2);
  {$ENDIF}

  {$IFDEF NOPE}
  emit(93);
  {$ENDIF}
  emit(3);

  {$DEFINE FEATURE}
  {$IFDEF FEATURE}
  emit(4);
  {$ENDIF}

  {$UNDEF FEATURE}
  {$IFDEF FEATURE}
  emit(94);
  {$ELSE}
  emit(5);
  {$ENDIF}

  {$IFDEF KOBOLD}
    {$IFDEF FEATURE}
    emit(95);
    {$ELSE}
    emit(6);
    {$ENDIF}
  {$ENDIF}
end.

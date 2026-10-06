program constrec;
{ A constant array of records. Each element is a record literal, whose fields
  match the record type in declared order. A string field is created at main
  start and stored as a handle; an ordinal field is stored as its immediate.
  The array is read through ordinary index and field access. Run under kprun it
  prints 3 1 5 2 5 3. }
procedure emit(v: integer); external;

type
  TTok = (tkNone, tkAnd, tkArray, tkBegin);
  TKeyword = record
    Name: string;
    Kind: TTok;
  end;

const
  Count = 3;
  Keywords: array[1..Count] of TKeyword = (
    (Name: 'and';   Kind: tkAnd),
    (Name: 'array'; Kind: tkArray),
    (Name: 'begin'; Kind: tkBegin)
  );

var
  i: integer;

begin
  for i := 1 to Count do begin
    emit(Length(Keywords[i].Name));   { 3, 5, 5 }
    emit(Ord(Keywords[i].Kind));      { 1, 2, 3 }
  end;
end.

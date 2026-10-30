program propertyagg;
{ Aggregate-typed properties (doc/languages/kobold/kp-property.md): a record value carried
  through getter/setter accessors, which return it through caller storage (sret). Covers a
  read/write record property whose record has a string field (so the aggregate copy exercises
  ARC), reading a field and a nested field of a record property, assigning a sub-record and the
  whole record property to variables, and a computed record field that is itself a record. Run
  under kprun it prints, one per line:
    origin
    3
    4
    7
    origin
    6 }

procedure emit(v: integer); external;

type
  Vec = record
    x, y: integer;
  end;
  Tagged = record
    name: string;
    v: Vec;
  end;

var
  backing: Tagged;

function getTag: Tagged;
begin
  getTag := backing;
end;

procedure setTag(const t: Tagged);
begin
  backing := t;
end;

{ a read/write record property }
property tag: Tagged read getTag write setTag;

type
  Holder = record
    base: Vec;
    doubled: Vec read getDoubled;   { computed record field: base with both axes doubled }
  end;

function getDoubled for (const h: Holder): Vec;
begin
  getDoubled.x := h.base.x * 2;
  getDoubled.y := h.base.y * 2;
end;

var
  t: Tagged;
  v: Vec;
  h: Holder;

begin
  t.name := 'origin';
  t.v.x := 3;
  t.v.y := 4;
  tag := t;              { setter fed a record with a string field }

  writeln(tag.name);     { field read of a record property }
  emit(tag.v.x);         { nested field read }
  emit(tag.v.y);

  v := tag.v;            { assign a sub-record of a record property to a variable }
  emit(v.x + v.y);       { 7 }

  t := tag;              { assign the whole record property to a variable }
  writeln(t.name);       { origin, string field copied }

  h.base.x := 1;
  h.base.y := 2;
  v := h.doubled;        { computed record field that is itself a record: (2, 4) }
  emit(v.x + v.y);       { 6 }
end.

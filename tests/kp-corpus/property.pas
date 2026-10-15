program propertytest;
{ Properties (uniform access): a declaration-level read/write property, a read-only
  property, and a computed record field with read and write accessor methods. Run under
  kprun it prints 5, 7, 11, 212, 0, 100, one per line. }
procedure emit(v: integer); external;

var
  backingVol: integer;

function getVolume: integer;
begin
  getVolume := backingVol;
end;

procedure setVolume(v: integer);
begin
  backingVol := v;
end;

function getMax: integer;
begin
  getMax := 11;
end;

{ read/write and read-only declaration-level properties; the accessors are named before
  the property, but resolution tolerates either order }
property volume: integer read getVolume write setVolume;
property maxVolume: integer read getMax;

type
  Temp = record
    c: integer;
    f: integer read getF write setF;   { computed: Fahrenheit over a Celsius cell }
  end;

function getF for (t: Temp): integer;
begin
  getF := t.c * 9 div 5 + 32;
end;

procedure setF for (t: Temp)(v: integer);
begin
  t.c := (v - 32) * 5 div 9;
end;

var
  tp: Temp;

begin
  volume := 5;
  emit(volume);
  volume := volume + 2;
  emit(volume);
  emit(maxVolume);

  tp.c := 100;
  emit(tp.f);
  tp.f := 32;
  emit(tp.c);
  tp.f := 212;
  emit(tp.c);
end.

unit wkdep;

interface

{ A record exported with a weak field. The `weak` qualifier must survive the KPM
  interface round-trip, so a consumer gives the field the same non-retaining
  lowering (kp_weak_set / kp_weak_get) the defining unit would. }
type THolder = record
  weak w: process;
  tag: integer;
end;

implementation

end.

unit std.random;
{ Random numbers as a unit, over the generators in runtime/rng.c reached through
  the kp_random_* glue (runtime/rng_glue.c).  In the std tier because Randomize
  seeds from host entropy; a seed-only core.random subset usable on bare metal is
  a later addition.  The fast generator is xoshiro128**.  SecureRandom (the
  ChaCha20 CSPRNG) is a later addition: its buffer-filling surface needs more
  glue than the scalar draws here. }
interface

procedure SeedRandom(seed: Integer);          { pin a reproducible sequence }
procedure Randomize;                           { seed from host entropy }
function RandomBelow(bound: Integer): Integer;  { uniform in [0, bound); 0 if bound <= 0 }
{ RandomReal (a uniform real in [0, 1), over kp_random_real) is deferred: a
  real-returning `external cdecl` is not yet marshalled on the rv soft-float
  tier, which carries a double as its 64-bit bits through a bridge (the brv_*
  path in io_free.c) rather than in a float register.  Wiring that bridge
  through the FFI is a follow-up; the integer draws below work on every tier. }

implementation

procedure kp_random_seed(seed: Integer); external cdecl;
procedure kp_randomize; external cdecl;
function kp_random_below(bound: Integer): Integer; external cdecl;

procedure SeedRandom(seed: Integer);
begin
  kp_random_seed(seed);
end;

procedure Randomize;
begin
  kp_randomize;
end;

function RandomBelow(bound: Integer): Integer;
begin
  RandomBelow := kp_random_below(bound);
end;

end.

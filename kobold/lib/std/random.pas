unit std.random;
{ Random numbers as a unit, over the generators in runtime/rng.c reached through
  the kp_random_* glue (runtime/rng_glue.c).  In the std tier because Randomize
  seeds from host entropy; a seed-only core.random subset usable on bare metal is
  a later addition.  The fast generator is xoshiro128**.  SecureRandom draws
  from the ChaCha20 CSPRNG (seeded once from host entropy, distinct from the
  fast generator), rejection-sampled to stay unbiased. }
interface

procedure SeedRandom(seed: Integer);          { pin a reproducible sequence }
procedure Randomize;                           { seed from host entropy }
function RandomBelow(bound: Integer): Integer;  { uniform in [0, bound); 0 if bound <= 0 }
function RandomReal: real;                      { uniform real in [0, 1) }
function SecureRandom(bound: Integer): Integer; { secure uniform in [0, bound); 0 if bound <= 0 }

implementation

procedure kp_random_seed(seed: Integer); external cdecl;
procedure kp_randomize; external cdecl;
function kp_random_below(bound: Integer): Integer; external cdecl;
function kp_random_real: real; external cdecl;
function kp_secure_below(bound: Integer): Integer; external cdecl;

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

function RandomReal: real;
begin
  RandomReal := kp_random_real;
end;

function SecureRandom(bound: Integer): Integer;
begin
  SecureRandom := kp_secure_below(bound);
end;

end.

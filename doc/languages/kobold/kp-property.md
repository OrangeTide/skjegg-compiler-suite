# Kobold-Pascal Properties (uniform access)

This note describes `property`: an accessor-backed value that looks like a variable at
every use site but is implemented by routines. It follows the uniform access principle,
so a consumer cannot tell, and does not need to know, whether it is reading a stored cell
or a computed value.

> Status: proposed, deferred, not yet built. This is an independent language feature,
> judged on its own merits. It was split out of the units design (`kp-units.md`), where
> earlier drafts folded it in. Units do not require it: a unit exports plain `var` and
> `const`, and can use a `property` once this feature exists. The `property`, `read`, and
> `write` words are already reserved in the lexer (see `kp-grammar.md`).

## Why a separate feature

A `property` is not specific to units. It is a general type-system feature: a member that
presents as a value but is backed by accessors. It applies to a unit's exported surface
and to record fields alike. Keeping it separate lets units land with a small, clear
export model (`var` for state, `const` for compile-time values) and lets the property
feature be designed, scheduled, and justified on its own.

## The surface

A `property` declares a `read` accessor and an optional `write` accessor:

```pascal
property volume: integer read getVolume write setVolume;  { read and write }
property level: integer read getLevel;                    { read-only to consumers }
property mixDown: Sample read computeMix;                  { computed, no storage }
```

Access control falls out of which accessors exist: a property with only `read` is
read-only across the boundary, which is the read-shared, write-owned pattern. There is no
separate access-control keyword; the presence or absence of `write` is the control.

## Uniform access and the ABI

At the use site a property reads and writes exactly like a variable. At the ABI a property
access is a call to its accessor by default, resolved at link the same way a routine import
is. Because the use site does not change, a unit can start with a plain `var` and later
replace it with a `property` of the same name: the use-site source is unchanged, only the
ABI changes, so consumers recompile but do not edit. This is the payoff of uniform access,
and it is why `var` and `property` are deliberately interchangeable at the call site.

A call per read is not the whole story. For a trivial accessor, a getter that just returns
a field, or the deferred lambda form below, the object format can carry the accessor body
in the module's interface so the consumer inlines it across the unit boundary. A property
read then lowers to the same load a `var` access would, with no call. This is the reason
property was originally designed alongside units: it reuses the interface inlining
mechanism units already need for an inline `const`, where the value rides the interface and
the consumer bakes it in (see the Mapping to KPM1 section of `kp-units.md`). An inlined
accessor body is consumer-observable, so it is part of the interface fingerprint: change
the body and the consumers that inlined it must recompile, exactly like a changed inline
const.

The accessor bodies live in the implementation part of the exporting unit, or on the owning
unit for a record field. Inline accessor bodies are a deferred future form, likely
lambda-shaped (for example `read () => hardwareVolume()`); the routine-reference form above
is the surface for now.

## Computed record fields

A `property` may also be a record field, giving computed fields:

```pascal
type Sprite = record
    x, y: real;
    bounds: Rect read computeBounds;   { computed field }
end;
```

A computed field reads like a stored field but runs its accessor. The same read/write
rule applies: a field with only `read` is read-only.

## Relationship to `const` and link-time values

A `property` is for computed or validated access, backed by a routine and a call per read.
It is not the way to expose a plain changeable constant. A scalar value that a foundation
library wants to change with a relink is a link-time constant symbol, a `const` resolved
at link, not a property (see the Constants section of `kp-units.md`). Reserve `property`
for the cases that genuinely need an accessor.

## Open questions

- **Inline accessor syntax.** The routine-reference form (`read getX write setX`) is the
  surface now; an inline, likely lambda-shaped form is a later addition.
- **Scheduling.** This feature is unscheduled and independent of the units milestone.

## See also

- `kp-units.md` the unit and separate-compilation design (exports `var` and `const`).
- `kp-grammar.md` the reserved words and the deferred property productions.
- `kobold.md` the language and runtime design, and the decision log.

#!/bin/sh
# aot-run.sh : ahead-of-time compile a KPM1 module and run it, printing its
# stdout, so it can stand in for kprun in the Kobold runcheck (make runcheck
# KPRUN=...).  The first argument is the module; any further arguments are the
# guest program's own, passed through so ParamStr/ParamCount see them (the module
# path fills the program-name slot, as it does under kprun).
#
# A program that `uses` units carries a `.uses NAME ...` line per dependency; the
# dependency module is NAME.kpm, found beside the program or under a KOBOLD_UNITS
# directory, exactly as kprun resolves it.  Every reached module is compiled and the
# whole set linked together, the system linker standing in for the JIT's in-memory
# link.  The program's own entry is renamed to kp_guest_entry by the driver; a unit
# has no entry, so its symbols keep their names and resolve across the link.
#
# Driven by the environment so one script serves every target:
#   KPAOT      the AOT driver (build/skj-kp, skj-kp-rv, skj-kp-arm64)   [required]
#   KPAOTLIB   the runtime archive to link (build/libkpaot*.a)          [required]
#   KPAOTCC    the compiler driver that assembles the .s and links      [required]
#   KPAOTAS    an explicit assembler step (x86 nasm); empty elsewhere
#   KPAOTQEMU  an emulator prefix for a cross target; empty on the host
#   KOBOLD_UNITS  colon-separated extra directories to find unit .kpm files
set -e

: "${KPAOT:?set KPAOT to the AOT driver}"
: "${KPAOTLIB:?set KPAOTLIB to the runtime archive}"
: "${KPAOTCC:?set KPAOTCC to the link driver}"

mod="$1"
[ -n "$mod" ] || { echo "usage: aot-run.sh module [args...]" >&2; exit 2; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

moddir=$(dirname "$mod")

# Find a unit's module file: beside the program first, then each KOBOLD_UNITS dir.
find_unit() {
    name=$1
    if [ -f "$moddir/$name.kpm" ]; then echo "$moddir/$name.kpm"; return 0; fi
    oldifs=$IFS; IFS=:
    for d in $KOBOLD_UNITS; do
        if [ -n "$d" ] && [ -f "$d/$name.kpm" ]; then IFS=$oldifs; echo "$d/$name.kpm"; return 0; fi
    done
    IFS=$oldifs
    echo "aot-run.sh: cannot find unit '$name'" >&2
    return 1
}

# Compile one module to an object, collecting it in $objs; then recurse into its
# own `.uses` dependencies.  $seen dedups by unit name.
objs=""
seen=""
compile_module() {
    file=$1
    o="$tmp/m$objn.o"
    objn=$((objn + 1))
    if [ -n "$KPAOTAS" ]; then
        "$KPAOT" "$file" -o "$tmp/m.s"
        $KPAOTAS "$tmp/m.s" -o "$o"
    else
        "$KPAOT" "$file" -o "$tmp/m.s"
        $KPAOTCC -no-pie -c "$tmp/m.s" -o "$o"
    fi
    objs="$objs $o"

    for dep in $(grep '^\.uses ' "$file" 2>/dev/null | awk '{print $2}'); do
        case " $seen " in *" $dep "*) continue ;; esac
        seen="$seen $dep"
        compile_module "$(find_unit "$dep")"
    done
}

objn=0
compile_module "$mod"

$KPAOTCC -no-pie $objs "$KPAOTLIB" -o "$tmp/p" -lm

exec $KPAOTQEMU "$tmp/p" "$@"

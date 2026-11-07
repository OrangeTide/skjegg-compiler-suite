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
#   KPAOT      the single-step compiler used as the AOT driver, lowering a .kpm
#              (build/skj-kpc, skj-kpc-rv, skj-kpc-arm64)                [required]
#   KPAOTLIB   the runtime archive to link (build/libkpaot*.a)          [required]
#   KPAOTCC    the compiler driver that assembles the .s and links      [required]
#   KPAOTAS    an explicit assembler step (x86 host as, or the in-tree
#              skj-as-aarch64); empty lets KPAOTCC assemble the .s
#   KPAOTQEMU  an emulator prefix for a cross target; empty on the host
#   KPAOTNOPIE the link driver's no-PIE flag; default -no-pie, empty on a
#              PIE-only host (macOS), where clang rejects it
#   KPAR       skj-ar, to read a unit's interface member    [default build/skj-ar]
#   KOBOLD_UNITS  colon-separated extra directories to find unit .a archives
set -e

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/.." && pwd)

: "${KPAOT:?set KPAOT to the AOT driver}"
: "${KPAOTLIB:?set KPAOTLIB to the runtime archive}"
: "${KPAOTCC:?set KPAOTCC to the link driver}"
KPAR=${KPAR:-"$ROOT/build/skj-ar"}
NOPIE=${KPAOTNOPIE--no-pie}

mod="$1"
[ -n "$mod" ] || { echo "usage: aot-run.sh module [args...]" >&2; exit 2; }
# The module is compiled into the program, so it is not a guest argument: drop it
# here.  The AOT program keeps its own argv[0] as ParamStr(0) (standard Pascal), so
# the remaining "$@" are the guest's ParamStr(1..ParamCount), matching kprun.
shift

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

moddir=$(dirname "$mod")

# Find a unit's archive: beside the program first, then each KOBOLD_UNITS dir.
# A unit is a `.a` archive; a dotted unit name maps its dots to path separators
# (io.files -> io/files.a).
find_unit() {
    name=$1
    rel=$(echo "$name" | tr . /)
    if [ -f "$moddir/$rel.a" ]; then echo "$moddir/$rel.a"; return 0; fi
    oldifs=$IFS; IFS=:
    for d in $KOBOLD_UNITS; do
        if [ -n "$d" ] && [ -f "$d/$rel.a" ]; then IFS=$oldifs; echo "$d/$rel.a"; return 0; fi
    done
    IFS=$oldifs
    echo "aot-run.sh: cannot find unit '$name'" >&2
    return 1
}

# Compile one KPM module (a .kpm text file: the program root, or a unit's interface
# member extracted from its archive) to an object, collecting it in $objs; then
# recurse into its own `.uses` dependencies.  $seen dedups by unit name.
objs=""
seen=""
depn=0
compile_module() {
    file=$1
    o="$tmp/m$objn.o"
    objn=$((objn + 1))
    if [ -n "$KPAOTAS" ]; then
        "$KPAOT" "$file" -o "$tmp/m.s"
        $KPAOTAS "$tmp/m.s" -o "$o"
    else
        "$KPAOT" "$file" -o "$tmp/m.s"
        $KPAOTCC $NOPIE -c "$tmp/m.s" -o "$o"
    fi
    objs="$objs $o"

    for dep in $(grep '^\.uses ' "$file" 2>/dev/null | awk '{print $2}'); do
        case " $seen " in *" $dep "*) continue ;; esac
        seen="$seen $dep"
        a=$(find_unit "$dep") || exit 1
        depn=$((depn + 1))
        k="$tmp/dep$depn.kpm"
        "$KPAR" p "$a" .kpm > "$k"
        compile_module "$k"
    done
}

objn=0
compile_module "$mod"

$KPAOTCC $NOPIE $objs "$KPAOTLIB" -o "$tmp/p" -lm

exec $KPAOTQEMU "$tmp/p" "$@"

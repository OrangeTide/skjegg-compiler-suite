#!/bin/sh
# Exercise the cooperative blocking-read path of skj-run end to end.
#
# For each arch's echo guest, feed stdin two ways:
#   1. already buffered  (the fd polls ready: the plain read path)
#   2. written after a delay (the fd is not ready at first read: the guest
#      parks, the scheduler waits on the fd and re-runs the read when data
#      arrives, then the echo completes)
# Both must reproduce the input exactly.  A wrong PC rewind on the re-run, or a
# poll that never wakes, shows up here as bad output or a timeout.

RUN="$1"; shift
fail=0

check() {
    name="$1" bin="$2" data="$3" delay="$4"
    if [ "$delay" = "0" ]; then
        out=$( printf '%s' "$data" | timeout 10 "$RUN" "$bin" )
    else
        out=$( { sleep "$delay"; printf '%s' "$data"; } | timeout 10 "$RUN" "$bin" )
    fi
    if [ "$out" = "$data" ]; then
        echo "PASS  $name"
    else
        echo "FAIL  $name (got '$out', want '$data')"
        fail=1
    fi
}

for a in "$@"; do
    arch=${a%%=*}
    bin=${a#*=}
    check "$arch ready" "$bin" "coop-ready-$arch" 0
    check "$arch delayed" "$bin" "coop-park-$arch" 0.3
done

[ "$fail" = 0 ] && echo "coop-read: all passed"
exit $fail

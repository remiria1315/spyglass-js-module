#!/bin/sh
preset="${1:-relwithdebinfo-android}"
log="build/$preset.log"

if [ -z "${ANDROID_NDK_ROOT:-}" ]; then
    echo "ANDROID_NDK_ROOT is not set"
    exit 1
fi

mkdir -p build
status=0
{ [ -d "build/$preset" ] || cmake --preset "$preset" ${CLIENTS:+-DMINECRAFT_CLIENTS="$CLIENTS"}; } >"$log" 2>&1 && cmake --build --preset "$preset" >>"$log" 2>&1 || status=$?

if [ "$status" -ne 0 ]; then
    grep -i error "$log" | head -10
    echo
    tail -20 "$log"
    echo
    echo "failed, full log: $log"
    exit "$status"
fi

tail -20 "$log"
echo
echo "built: build/$preset"

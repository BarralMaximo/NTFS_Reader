#!/usr/bin/env bash
#
# Replay every fixture manifest against the built binary: each directory
# must list, each file's content must hash to what was recorded when the
# image was generated. The ground truth is the data we put in the image
# ourselves, so no reference implementation is needed.
set -u

cd "$(dirname "$0")"
BIN=../build/ntfsread

[ -x "$BIN" ] || { echo "run.sh: build first (make)" >&2; exit 1; }

shopt -s nullglob
manifests=(fixtures/*.manifest)
if [ ${#manifests[@]} -eq 0 ]; then
    echo "run.sh: no fixtures found — generate them with tests/mkimages.sh" >&2
    echo "run.sh: SKIPPED" >&2
    exit 0
fi

pass=0 fail=0

check() { # description, command...
    local desc=$1
    shift
    if "$@" >/dev/null 2>&1; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $desc" >&2
    fi
}

for m in "${manifests[@]}"; do
    img="${m%.manifest}.img"
    [ -f "$img" ] || { echo "FAIL: missing $img" >&2; fail=$((fail+1)); continue; }

    check "$img: info" "$BIN" "$img" info

    while read -r kind a b; do
        case "$kind" in
        D)
            check "$img: ls $a" "$BIN" "$img" ls "$a"
            ;;
        F)
            got=$("$BIN" "$img" cat "$b" 2>/dev/null | sha256sum | cut -d' ' -f1)
            if [ "$got" = "$a" ]; then
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
                echo "FAIL: $img: cat $b (hash mismatch)" >&2
            fi
            ;;
        esac
    done <"$m"
done

echo "tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

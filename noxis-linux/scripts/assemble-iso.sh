#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Assemble the NoxisOS ISO from the split parts published in a release, and
# verify every byte on the way:
#
#   * every part is checked against SHA256SUMS (when it is next to the parts),
#   * the numbering has no gaps and the sizes match a `split` of one image,
#   * the joined image is checked against noxis-*.iso.sha256,
#   * the result is compared with the size the parts must add up to.
#
# A truncated join is the usual cause of "no bootable device" after writing the
# stick, and this script refuses to produce one silently.
#
# Usage: scripts/assemble-iso.sh [--parts DIR] [--out FILE] [--expect-bytes N]
set -euo pipefail

parts_dir="."
out=""
expect_bytes=""

usage() {
    sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --parts)        parts_dir="${2:?--parts needs a directory}"; shift 2 ;;
        --out)          out="${2:?--out needs a file name}"; shift 2 ;;
        --expect-bytes) expect_bytes="${2:?--expect-bytes needs a number}"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *)              echo "assemble-iso: unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

cd "$parts_dir" || { echo "assemble-iso: no such directory: $parts_dir" >&2; exit 1; }
shopt -s nullglob

fail() { echo "assemble-iso: $*" >&2; exit 1; }

parts=( *.iso.part* )   # nullglob: an empty array when nothing matched

# ---- collect the parts, in split order ---------------------------------------
mapfile -t parts < <(printf '%s\n' "${parts[@]}" | sort)

if (( ${#parts[@]} == 0 )); then
    cat >&2 <<'MSG'
assemble-iso: no *.iso.part* files here.

Download *every* part of the release (part01, part02, …) plus SHA256SUMS and
noxis-*.iso.sha256 into the same directory, then run this script again.
MSG
    exit 1
fi

if (( ${#parts[@]} == 1 )); then
    fail "only one part found (${parts[0]}) — the image is bigger than that.
       The release publishes several parts; download them all into one directory.
       Joining a single part gives a truncated image, which is exactly the
       'no bootable device' case."
fi

iso_name=$(printf '%s\n' "${parts[0]}" | sed -E 's/\.part[0-9]+$//')
out="${out:-$iso_name}"

echo "assemble-iso: ${#parts[@]} part(s) -> $out"
printf '  %s\n' "${parts[@]}"

# ---- numbering must have no gaps --------------------------------------------
expected=1
for p in "${parts[@]}"; do
    n=${p##*.part}
    n=$((10#$n))
    if (( n != expected )); then
        fail "part numbering jumps: expected part$(printf '%02d' "$expected"), got $p
       One of the parts is missing from this directory."
    fi
    (( expected++ ))
done

# ---- sizes must look like one image that was cut in pieces -------------------
size_of() { stat -c '%s' -- "$1" 2>/dev/null || stat -f '%z' -- "$1"; }

first_size=$(size_of "${parts[0]}")
total=0
prev_size=""
for p in "${parts[@]}"; do
    s=$(size_of "$p")
    (( s == 0 )) && fail "$p is empty — the download did not finish."
    if [[ -n $prev_size ]] && (( s > prev_size )); then
        fail "$p ($s bytes) is bigger than the part before it ($prev_size bytes).
       The parts are probably in the wrong order or one of them is unrelated."
    fi
    prev_size=$s
    total=$(( total + s ))
done

last_size=$(size_of "${parts[-1]}")
if (( last_size == first_size )); then
    echo "assemble-iso: warning: the last part is exactly as big as the previous ones."
    echo "             If the release publishes more parts, one of them is still missing."
fi

# ---- per-part checksums ------------------------------------------------------
if [[ -f SHA256SUMS ]]; then
    echo "assemble-iso: checking the parts against SHA256SUMS"
    if ! out_sha=$(sha256sum -c --quiet SHA256SUMS 2>&1); then
        printf '%s\n' "$out_sha" >&2
        if grep -q 'FAILED open or read' <<<"$out_sha"; then
            fail "one of the parts listed in SHA256SUMS is not in this directory.
       Download the missing part(s) and run this script again."
        fi
        fail "a downloaded part does not match SHA256SUMS — it is incomplete or
       damaged. Download that part again (and check the size is identical)."
    fi
    echo "  parts OK"
else
    echo "assemble-iso: note: SHA256SUMS is not here, skipping the per-part check"
fi

# ---- join --------------------------------------------------------------------
echo "assemble-iso: joining $(numfmt --to=iec "$total" 2>/dev/null || echo "$total bytes")"
cat -- "${parts[@]}" > "$out"

joined=$(size_of "$out")
(( joined == total )) || fail "the joined file is $joined bytes, expected $total — disk full?"

if [[ -n $expect_bytes ]] && (( joined != expect_bytes )); then
    fail "the joined image is $joined bytes, but the release expects $expect_bytes.
       At least one part is still missing — download the rest and try again."
fi

# ---- whole-image checksum ----------------------------------------------------
shafile=""
for f in ./*.iso.sha256; do shafile=$f; break; done
if [[ -n $shafile ]]; then
    echo "assemble-iso: checking the image against $(basename "$shafile")"
    expected_hash=$(grep -oiE '[0-9a-f]{64}' "$shafile" | head -n1)
    actual_hash=$(sha256sum -- "$out" | cut -d' ' -f1)
    if [[ -z $expected_hash ]]; then
        echo "  note: no SHA-256 in that file, skipping the check"
    elif [[ ${expected_hash,,} != "${actual_hash,,}" ]]; then
        fail "checksum mismatch!
       expected $expected_hash
       actual   $actual_hash
       The image is not the one that was published — parts are missing, mixed up
       or damaged. Delete $out and start over with fresh downloads."
    else
        echo "  sha256 $actual_hash"
    fi
else
    echo "assemble-iso: note: no *.iso.sha256 here, skipping the whole-image check"
fi

echo
echo "assemble-iso: done — $out, $joined bytes ($(numfmt --to=iec "$joined" 2>/dev/null || echo "$joined"))"
echo
echo "Write it raw to the whole device (the whole disk, not a partition, and never"
echo "a plain file copy — that alone can cause 'no bootable device'):"
echo "  sudo dd if=$out of=/dev/sdX bs=4M status=progress oflag=sync"
echo "Then, to be sure the stick holds the image:"
echo "  sudo cmp -n $joined $out /dev/sdX && echo 'stick OK'"

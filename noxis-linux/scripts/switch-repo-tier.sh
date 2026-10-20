#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# switch-repo-tier.sh — build an ISO whose *base* packages come from the
# x86-64-v3 / v4 / znver4 CachyOS repositories instead of plain x86-64.
#
#   ./scripts/switch-repo-tier.sh v3      # Haswell/Excavator and newer
#   ./scripts/switch-repo-tier.sh v4      # AVX-512 CPUs
#   ./scripts/switch-repo-tier.sh znver4  # AMD Zen 4/5
#   ./scripts/switch-repo-tier.sh base    # back to x86-64 (default)
#
# NOTE: an ISO built from a higher tier only boots on CPUs that support it.
# ---------------------------------------------------------------------------
set -euo pipefail

tier=${1:-}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
conf="${script_dir}/../configs/noxis/pacman.conf"

case "$tier" in
    base|v3|v4|znver4) ;;
    *) echo "usage: $0 {base|v3|v4|znver4}" >&2; exit 1 ;;
esac

python3 - "$conf" "$tier" <<'PY'
import re, sys, pathlib
conf, tier = sys.argv[1], sys.argv[2]
path = pathlib.Path(conf)
text = path.read_text()

header = "# --- CachyOS optimized repositories ---------------------------------------\n"
tiers = {
    "v3": ("[cachyos-v3]", "[cachyos-core-v3]", "[cachyos-extra-v3]", "cachyos-v3-mirrorlist"),
    "v4": ("[cachyos-v4]", "[cachyos-core-v4]", "[cachyos-extra-v4]", "cachyos-v4-mirrorlist"),
    "znver4": ("[cachyos-znver4]", "[cachyos-core-znver4]", "[cachyos-extra-znver4]", "cachyos-v4-mirrorlist"),
}

for name, (a, b, c, mirror) in tiers.items():
    block = (f"[{a[1:-1]}]\nServer = https://mirror.cachyos.org/repo/x86_64/{a[1:-1]}\n"  # unused, keeps lint happy
             if False else f"{a}\nServer = https://mirror.cachyos.org/repo/$arch/{a[1:-1]}\n")
    block = f"{a}\nServer = https://mirror.cachyos.org/repo/$arch/{a[1:-1]}\n{b}\nServer = https://mirror.cachyos.org/repo/$arch/{b[1:-1]}\n{c}\nServer = https://mirror.cachyos.org/repo/$arch/{c[1:-1]}\n"
    pattern = re.compile(rf"^{re.escape(a)}\n(?:.*\n)*?{re.escape(c)}\n.*\n", re.M)
    if name == tier:
        if pattern.search(text):
            text = pattern.sub(block, text)
        else:
            text = text.replace(header, header + "\n" + block, 1)
    else:
        text = pattern.sub("", text)

# make sure the header comment still matches reality
text = re.sub(r"^# Switch to \[cachyos.*\n", "", text, flags=re.M)
path.write_text(text)
print(f"==> pacman.conf now builds from the '{tier}' tier")
PY

#!/bin/sh
# Run as root. Installs the latest GitHub release (bridge and stick firmware) when it is new; vartovyi-update.timer runs it hourly.
# The stick then finds the firmware in Settings > Update firmware.
set -eu
REPO=${REPO:-Skerpsh/vartovyi}
SEEN=/opt/peace-stick/release

tag=$(curl -fsS "https://api.github.com/repos/$REPO/releases/latest" | python3 -c 'import json, sys; print(json.load(sys.stdin)["tag_name"])')
[ "$tag" = "$(cat "$SEEN" 2>/dev/null)" ] && exit 0
echo "Installing $tag"

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
base=https://github.com/$REPO/releases/download/$tag
curl -fsSL -o "$dir/server.tar.gz" "$base/server.tar.gz"
curl -fsSL -o "$dir/firmware.bin" "$base/firmware.bin"
tar -xzf "$dir/server.tar.gz" -C "$dir"

sh "$dir/install.sh"
bash "$dir/add-caddy-routes.sh"

# The bin first and the md5 last: a stick that sees the new md5 finds the matching file.
out=/var/lib/peace-stick
install -m 644 "$dir/firmware.bin" "$out/firmware.bin.tmp" && mv "$out/firmware.bin.tmp" "$out/firmware.bin"
md5sum "$out/firmware.bin" | cut -d' ' -f1 >"$out/firmware.md5.tmp" && chmod 644 "$out/firmware.md5.tmp" && mv "$out/firmware.md5.tmp" "$out/firmware.md5"
echo "$tag" >"$SEEN"
echo "Installed $tag"

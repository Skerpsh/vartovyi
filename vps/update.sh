#!/bin/sh
# Run as root. Installs the latest GitHub release (bridge and stick firmware) when it is new; vartovyi-update.timer runs it hourly.
# Nothing is installed unless both files carry a valid signature by the release key already on this server
# (/etc/peace-stick/release-public.pem): a release published by someone else is ignored. See sign-release.sh.
set -eu
REPO=${REPO:-Skerpsh/vartovyi}
SEEN=/opt/peace-stick/release
KEY=/etc/peace-stick/release-public.pem
[ -s "$KEY" ] || { echo "No release key in $KEY: install once with deploy.sh" >&2; exit 1; }

tag=$(curl -fsS "https://api.github.com/repos/$REPO/releases/latest" | python3 -c 'import json, sys; print(json.load(sys.stdin)["tag_name"])')
[ "$tag" = "$(cat "$SEEN" 2>/dev/null)" ] && exit 0
echo "Installing $tag"

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
base=https://github.com/$REPO/releases/download/$tag
curl -fsSL -o "$dir/server.tar.gz" "$base/server.tar.gz"
curl -fsSL -o "$dir/firmware.bin" "$base/firmware.bin"
# Signatures come after the build, from the owner's machine; until then, wait for the next run.
for f in server.tar.gz firmware.bin; do
  curl -fsSL -o "$dir/$f.sig" "$base/$f.sig" 2>/dev/null || { echo "$tag is not signed yet"; exit 0; }
  base64 -d "$dir/$f.sig" >"$dir/$f.der" && openssl dgst -sha256 -verify "$KEY" -signature "$dir/$f.der" "$dir/$f" >/dev/null ||
    { echo "$tag: bad signature on $f, not installed" >&2; exit 1; }
done
mkdir "$dir/server"
tar -xzf "$dir/server.tar.gz" -C "$dir/server"

sh "$dir/server/install.sh"
bash "$dir/server/add-caddy-routes.sh"

# The bin and its signature first, the md5 last: a stick that sees the new md5 finds the matching files.
out=/var/lib/peace-stick
install -m 644 "$dir/firmware.bin" "$out/firmware.bin.tmp" && mv "$out/firmware.bin.tmp" "$out/firmware.bin"
install -m 644 "$dir/firmware.bin.sig" "$out/firmware.sig.tmp" && mv "$out/firmware.sig.tmp" "$out/firmware.sig"
md5sum "$out/firmware.bin" | cut -d' ' -f1 >"$out/firmware.md5.tmp" && chmod 644 "$out/firmware.md5.tmp" && mv "$out/firmware.md5.tmp" "$out/firmware.md5"
echo "$tag" >"$SEEN"
echo "Installed $tag"

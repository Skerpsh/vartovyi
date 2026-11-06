#!/bin/bash
# Signs a release's server bundle and firmware with your private key, which never leaves this machine, and attaches
# the signatures to the release. The server and the sticks install nothing without them.
# Run it right after GitHub Actions has published a release you tagged yourself.
set -eu
cd "$(dirname "$0")"
TAG=${1:?usage: ./sign-release.sh vX.Y.Z}
KEY=${VARTOVYI_KEY:-$HOME/.config/vartovyi/release-key.pem}
REPO=${REPO:-Skerpsh/vartovyi}

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
gh release download "$TAG" -R "$REPO" -p server.tar.gz -p firmware.bin -D "$dir"
for f in server.tar.gz firmware.bin; do
  openssl dgst -sha256 -sign "$KEY" -out "$dir/$f.der" "$dir/$f"
  openssl dgst -sha256 -verify keys/release-public.pem -signature "$dir/$f.der" "$dir/$f" >/dev/null  # the right key
  base64 -w0 "$dir/$f.der" >"$dir/$f.sig"
done
gh release upload "$TAG" -R "$REPO" --clobber "$dir/server.tar.gz.sig" "$dir/firmware.bin.sig"
echo "Signed $TAG. The server installs it within an hour."

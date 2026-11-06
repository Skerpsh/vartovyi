#!/bin/bash
# Run as root. Adds the stick's routes that are missing from the Caddyfile; they are public, like the alerts themselves.
# Restores the previous file if Caddy rejects the result. Safe to run again.
set -eu
FILE=/etc/caddy/Caddyfile

line=$(grep -n 'nip.io' "$FILE" | head -1 | cut -d: -f1)
[ -n "$line" ] || { echo "No site with nip.io found in $FILE" >&2; exit 1; }

route() {  # name, path, file
  cat <<EOF
    @$1 path $2
    handle @$1 {
        root * /var/lib/peace-stick
        rewrite * /$3
        file_server
    }

EOF
}

new=$(mktemp)
trap 'rm -f "$new"' EXIT
grep -q '@peacestick ' "$FILE" || route peacestick /peace-stick alert.txt >>"$new"
grep -q '@peacestickhist ' "$FILE" || route peacestickhist /peace-stick/history history.txt >>"$new"
grep -q '@peacestickver ' "$FILE" || route peacestickver /peace-stick/version firmware.md5 >>"$new"
grep -q '@peacestickfw ' "$FILE" || route peacestickfw /peace-stick/firmware firmware.bin >>"$new"
grep -q '@peacestickrel ' "$FILE" || route peacestickrel /peace-stick/release release.txt >>"$new"
grep -q '@peacestickrelsig ' "$FILE" || route peacestickrelsig /peace-stick/release-sig release.txt.sig >>"$new"
# Every oblast's file and the list of them, for the firmware that covers all of Ukraine.
grep -q 'handle_path /peace-stick/v2/' "$FILE" || cat >>"$new" <<EOF
    handle_path /peace-stick/v2/* {
        root * /var/lib/peace-stick/v2
        file_server
    }

EOF
# The bridge checks each webhook's RSA signature.
grep -q '@peacestickhook ' "$FILE" || cat >>"$new" <<EOF
    @peacestickhook path /peace-stick/webhook
    handle @peacestickhook {
        reverse_proxy 127.0.0.1:8787
    }

EOF
# Older installs asked the stick for its own token; the public firmware has none. Other sites' tokens stay.
OLD=$(cat /etc/peace-stick/token 2>/dev/null || true)
[[ $OLD =~ ^[0-9a-f]{64}$ ]] || OLD=none
tokened=$(grep -c "header Authorization \"Bearer $OLD\"" "$FILE" || true)
[ -s "$new" ] || [ "$tokened" != 0 ] || { echo 'Routes already there'; exit 0; }

cp "$FILE" "$FILE.bak-peace-stick"
sed -i "${line}r $new" "$FILE"
sed -i "/header Authorization \"Bearer $OLD\"/d" "$FILE"
if caddy validate --config "$FILE" >/dev/null 2>&1 && systemctl reload caddy; then
  echo ROUTES-OK
else
  cp "$FILE.bak-peace-stick" "$FILE"
  echo 'Caddy rejected the routes: the previous file is restored' >&2
  exit 1
fi

#!/bin/sh
# Run as root from this folder. Safe to run again. Touches neither /opt/Peace nor the Caddyfile (see add-caddy-routes.sh).
set -eu
cd "$(dirname "$0")"

[ -s /etc/peace-stick/api-key ] || { echo "No API key in /etc/peace-stick/api-key" >&2; exit 1; }

id peace-stick >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin peace-stick
install -d -m 755 /etc/peace-stick /opt/peace-stick
install -d -m 755 -o peace-stick -g peace-stick /var/lib/peace-stick
install -d -m 700 -o peace-stick -g peace-stick /var/lib/peace-stick/private
# The key from its file; other lines, like WEBHOOK_URL on a self-hosted server, stay.
(umask 077; { printf 'UA_API_KEY=%s\n' "$(cat /etc/peace-stick/api-key)"; grep -v '^UA_API_KEY=' /etc/peace-stick/env 2>/dev/null || true; } >/etc/peace-stick/env.new)
mv /etc/peace-stick/env.new /etc/peace-stick/env

[ -x /opt/peace-stick/venv/bin/python ] || python3 -m venv /opt/peace-stick/venv
/opt/peace-stick/venv/bin/pip install -q cryptography==50.0.2
/opt/peace-stick/venv/bin/pip uninstall -q -y telethon 2>/dev/null || true
install -m 644 peace_bridge.py regions.json /opt/peace-stick/
install -m 644 peace-stick-bridge.service /etc/systemd/system/
install -m 755 update.sh /opt/peace-stick/update.sh
install -m 644 vartovyi-update.service vartovyi-update.timer /etc/systemd/system/

# Telegram is no longer in the chain. This removes the file; Telegram itself still lists the session until it is ended there.
rm -f /var/lib/peace-stick/private/bridge.session /var/lib/peace-stick/private/bridge.session-journal

# Published by the API on its site, which is closed to non-browsers, so it travels with this folder.
# If they ever change the key, webhooks get rejected (see the journal) and alerts come only from the poll.
install -m 644 webhook-public.pem /etc/peace-stick/webhook-public.pem
# This server's own key for signing alert files; made once, it never leaves the server. The sticks need its public
# half (feed-public.pem) built in: copy it to keys/feed-public.pem in the repository.
if [ ! -s /etc/peace-stick/feed-key.pem ]; then
  (umask 077; openssl ecparam -name prime256v1 -genkey -noout -out /etc/peace-stick/feed-key.pem)
  openssl ec -in /etc/peace-stick/feed-key.pem -pubout -out /etc/peace-stick/feed-public.pem 2>/dev/null
fi
chgrp peace-stick /etc/peace-stick/feed-key.pem && chmod 640 /etc/peace-stick/feed-key.pem
# The key releases must be signed with (update.sh) and that sticks check firmware against.
install -m 644 release-public.pem /etc/peace-stick/release-public.pem

systemctl daemon-reload
systemctl enable peace-stick-bridge
systemctl enable --now vartovyi-update.timer
systemctl restart peace-stick-bridge
sleep 8
echo "--- Alert file now:"
cat /var/lib/peace-stick/alert.txt

#!/bin/bash
# Installs this working copy on the server, for testing before a release. Run in a real terminal: ssh asks for the key's passphrase.
set -eu
cd "$(dirname "$0")"
SERVER=${SERVER:-root@62.238.111.55}

python3 test/test_bridge.py
tar -C vps -cz peace_bridge.py regions.json peace-stick-bridge.service install.sh add-caddy-routes.sh update.sh vartovyi-update.service vartovyi-update.timer webhook-public.pem -C ../keys release-public.pem |
  ssh "$SERVER" 'rm -rf /tmp/peace-stick-install && mkdir /tmp/peace-stick-install && tar -xz -C /tmp/peace-stick-install'
ssh -t "$SERVER" 'sh /tmp/peace-stick-install/install.sh && bash /tmp/peace-stick-install/add-caddy-routes.sh'

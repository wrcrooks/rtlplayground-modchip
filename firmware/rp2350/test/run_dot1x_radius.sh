#!/bin/sh
# dot1x.c against a real FreeRADIUS 3.2 in Docker (host network, its default
# localhost client and secret testing123), with two users: alice / secret for
# EAP-MD5, and 020000000003 for MAB (user name and password the MAC).
#
#   sh run_dot1x_radius.sh build/test_dot1x_radius
set -eu
cd "$(dirname "$0")"
IMAGE=freeradius/freeradius-server:3.2.7
NAME=dot1x-test-radius
BIN=${1:-build/test_dot1x_radius}

USERS=$(mktemp -d "$PWD/build/radius.XXXXXX")
cat > "$USERS/authorize" <<'EOF'
alice	Cleartext-Password := "secret"
020000000003	Cleartext-Password := "020000000003"
EOF
chmod -R a+rX "$USERS"

docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" --network host \
	-v "$USERS/authorize:/etc/freeradius/mods-config/files/authorize:ro" "$IMAGE" -X >/dev/null
trap 'docker logs "$NAME" > build/freeradius.log 2>&1; docker rm -f "$NAME" >/dev/null; rm -rf "$USERS"' EXIT
for i in $(seq 30); do
	docker logs "$NAME" 2>&1 | grep -q "Ready to process requests" && break
	sleep 1
done
"$BIN"

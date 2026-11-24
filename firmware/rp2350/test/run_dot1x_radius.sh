#!/bin/sh
# dot1x.c against a real FreeRADIUS 3.2 in Docker (host network, its default
# localhost client and secret testing123), with two users: alice / secret for
# EAP-MD5, and 020000000003 for MAB (user name and password the MAC).
#
#   sh run_dot1x_radius.sh build/test_dot1x_radius
#   [TLS_MAX=1.2] sh run_dot1x_radius.sh - build/test_radsec     over TLS
set -eu
cd "$(dirname "$0")"
IMAGE=freeradius/freeradius-server:3.2.7
NAME=dot1x-test-radius
BIN=${1:-build/test_dot1x_radius}

USERS=$(mktemp -d "$PWD/build/radius.XXXXXX")
cat > "$USERS/authorize" <<'EOF'
alice	Cleartext-Password := "secret"
bob	Cleartext-Password := "secret"
	Tunnel-Type = VLAN, Tunnel-Medium-Type = IEEE-802, Tunnel-Private-Group-Id = "42"
020000000003	Cleartext-Password := "020000000003"
EOF
chmod -R a+rX "$USERS"

# Over TLS: our own certificate, for FreeRADIUS to trust, and its own one's fingerprint
RADSEC=${2:-}
VOLS=
if [ -n "$RADSEC" ]; then
	"$RADSEC" cert "$USERS"
	docker run --rm --entrypoint sh "$IMAGE" -c 'cat /etc/freeradius/sites-available/tls' > "$USERS/tls"
	docker run --rm --entrypoint sh "$IMAGE" -c 'cat /etc/freeradius/certs/ca.pem' > "$USERS/ca.pem"
	docker run --rm --entrypoint sh "$IMAGE" -c 'cat /etc/freeradius/certs/server.pem' > "$USERS/server.pem"
	sed -i "/^home_server /,\$d" "$USERS/tls"	# its client side, with a key it may not read
	openssl x509 -inform der -in "$USERS/cert.der" >> "$USERS/ca.pem"
	FP=$(openssl x509 -in "$USERS/server.pem" -outform der | sha256sum | cut -d' ' -f1)
	[ -z "${TLS_MAX:-}" ] || sed -i 's/tls_max_version = "1.3"/tls_max_version = "'"$TLS_MAX"'"/' "$USERS/tls"
	sed -i 's/cipher_server_preference = no/cipher_server_preference = yes/' "$USERS/tls"
	chmod -R a+rX "$USERS"
	VOLS="-v $USERS/tls:/etc/freeradius/sites-enabled/tls:ro -v $USERS/ca.pem:/etc/freeradius/certs/ca.pem:ro"
fi

docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" --network host \
	-v "$USERS/authorize:/etc/freeradius/mods-config/files/authorize:ro" $VOLS "$IMAGE" -fxx -l stdout >/dev/null
trap 'docker logs "$NAME" > build/freeradius.log 2>&1; docker rm -f "$NAME" >/dev/null; rm -rf "$USERS"' EXIT
for i in $(seq 30); do
	docker logs "$NAME" 2>&1 | grep -q "Ready to process requests" && break
	sleep 1
done
if [ -n "$RADSEC" ]; then
	"$RADSEC" "$USERS" "$FP"
	"$RADSEC" "$USERS" "$(printf %064d 7)" wrong
else
	"$BIN"
fi

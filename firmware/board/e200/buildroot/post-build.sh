#!/bin/sh
set -eu

TARGET_DIR="$1"

mkdir -p "${TARGET_DIR}/etc" "${TARGET_DIR}/etc/default"

sed -i \
	-e 's|^ttyPS[01]::respawn:.*$|ttyPS0::respawn:/sbin/getty -L ttyPS0 115200 vt100|' \
	"${TARGET_DIR}/etc/inittab"

if ! grep -q '^ttyPS0::respawn:' "${TARGET_DIR}/etc/inittab"; then
	printf 'ttyPS0::respawn:/sbin/getty -L ttyPS0 115200 vt100\n' >> "${TARGET_DIR}/etc/inittab"
fi

cat > "${TARGET_DIR}/etc/network/interfaces" <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
	address 192.168.1.10
	netmask 255.255.255.0
	gateway 192.168.1.1
EOF

cat > "${TARGET_DIR}/etc/default/dropbear" <<'EOF'
DROPBEAR_ARGS=""
EOF

#!/bin/sh
set -eu

if [ "$#" -lt 3 ]; then
	echo "usage: $0 <active-board-file> <board> [reuse] <repo>..." >&2
	exit 1
fi

active_file="$1"
board="$2"
shift 2

# A board may intentionally reuse already-patched kernel/U-Boot/Buildroot
# trees when its PS and Linux support are identical to the active board.  The
# caller must opt in explicitly; this never changes the active-board marker.
reuse_shared_sources="${REUSE_SHARED_SOURCES:-0}"
if [ "$#" -gt 0 ] && { [ "$1" = "0" ] || [ "$1" = "1" ]; }; then
	reuse_shared_sources="$1"
	shift
fi

if [ -f "$active_file" ]; then
	active_board="$(cat "$active_file")"
	if [ "$active_board" != "$board" ] && [ "$reuse_shared_sources" != "1" ]; then
		cat >&2 <<EOF
error: shared source trees are marked for board '$active_board', not '$board'.

Review and preserve the existing board patches before switching boards.
Use a separate clean checkout for another board. Do not discard source changes.
The active-board marker is: $active_file
EOF
		exit 1
	fi
	if [ "$active_board" != "$board" ]; then
		echo "warning: reusing shared source trees patched for '$active_board' while building '$board'" >&2
	fi
	exit 0
fi

dirty_repos=""
for repo in "$@"; do
	if [ -d "$repo/.git" ] && [ -n "$(git -C "$repo" status --short)" ]; then
		dirty_repos="${dirty_repos}
  $repo"
	fi
done

if [ -n "$dirty_repos" ]; then
	cat >&2 <<EOF
error: shared source trees are dirty and no active board marker exists.

Dirty repositories:${dirty_repos}

Review and save these changes before preparing a clean locked source checkout.
Then run this project's fetch-locked entry and build '$board'. No reset or
cleanup is performed automatically.
EOF
	exit 1
fi

mkdir -p "$(dirname "$active_file")"
printf '%s\n' "$board" > "$active_file"

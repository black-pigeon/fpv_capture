#!/bin/sh
set -eu
# Compatibility entry for the copied component Makefile; the manifest is the
# sole source of URLs/refs and the Python implementation never force-resets.
if [ "$#" -ne 3 ]; then
    echo 'usage: fetch_repo.sh <url> <ref> <component-directory>' >&2
    exit 2
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname -- "$script_dir")
component=$(basename -- "$3")
exec python3 "$script_dir/fetch_sources.py" --manifest "$root/versions.mk" \
    --root "$root" --component "$component" --locked --expected-url "$1" --expected-ref "$2"

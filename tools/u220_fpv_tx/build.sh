#!/usr/bin/env bash
set -euo pipefail
src=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out=${1:-"$src/../../build/u220-fpv-source"}
mkdir -p "$out"
read -r -a flags <<< "$(pkg-config --cflags uhd libcrypto)"
read -r -a libs <<< "$(pkg-config --libs uhd libcrypto)"
sender_tmp=$(mktemp "$out/.u220_fpv_tx.XXXXXX")
trap 'rm -f "$sender_tmp"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror -DBOOST_BIND_GLOBAL_PLACEHOLDERS \
    "${flags[@]}" "$src/u220_fpv_tx.cpp" "${libs[@]}" -pthread -o "$sender_tmp"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror \
    "$src/test_replay.cpp" -o "$out/test_replay"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror "${flags[@]}" \
    "$src/test_tx_events.cpp" "${libs[@]}" -o "$out/test_tx_events"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror -DBOOST_BIND_GLOBAL_PLACEHOLDERS \
    "$src/test_live_control.cpp" -pthread -o "$out/test_live_control"
"$out/test_replay"
"$out/test_tx_events"
"$out/test_live_control"
"$sender_tmp" --help >/dev/null
mv -f "$sender_tmp" "$out/u220_fpv_tx"
echo "Built $out/u220_fpv_tx (no RF device opened)"

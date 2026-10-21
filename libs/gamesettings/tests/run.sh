#!/bin/zsh
# GameSettings tests: default keys, a saved binding, the newer copy (this device / iCloud) wins, graphics applied.
set -eu
here=${0:A:h}; lib=${here:h}
T=$(mktemp -d); trap 'rm -rf $T' EXIT
xcrun clang -Wall -Wextra -Werror $here/settings_test.c -fobjc-arc $lib/gamesettings.m -framework Foundation -framework AppKit -framework CoreText -o $T/t
export GAMESETTINGS_LOCAL_DIR=$T/local GAMESETTINGS_CLOUD_DIR=$T/cloud
mkdir -p $T/local $T/cloud
$T/t defaults
# this device has an older binding (A = K), iCloud a newer one (A = L) and a graphics choice: iCloud's must win
echo '{"time": 100, "bindings": {"a": [40]}}' > $T/local/keyboard.json
echo '{"time": 200, "bindings": {"a": [37]}}' > $T/cloud/keyboard.json
echo '{"time": 200, "values": {"mode": 2}}' > $T/cloud/0100000000000000.json
$T/t rebound
echo "all passed"

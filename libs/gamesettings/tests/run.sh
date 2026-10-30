#!/bin/zsh
# GameSettings tests: default keys, a saved binding and graphics choice from this device, a cloud copy ignored.
set -eu
here=${0:A:h}; lib=${here:h}
T=$(mktemp -d); trap 'rm -rf $T' EXIT
xcrun clang -Wall -Wextra -Werror $here/settings_test.c -fobjc-arc $lib/gamesettings.m -framework Foundation -framework AppKit -framework CoreText -o $T/t
export GAMESETTINGS_LOCAL_DIR=$T/local SAVESYNC_DIR=$T/cloud
mkdir -p $T/local $T/cloud
$T/t defaults
# this device binds A = L with graphics choice 2; a newer copy in a cloud folder (as earlier builds wrote) must not count
echo '{"time": 100, "bindings": {"a": [37]}}' > $T/local/keyboard.json
echo '{"time": 100, "values": {"mode": 2}}' > $T/local/0100000000000000.json
echo '{"time": 200, "bindings": {"a": [40]}}' > $T/cloud/keyboard.json
echo '{"time": 200, "values": {"mode": 1}}' > $T/cloud/0100000000000000.json
$T/t rebound
echo "all passed"

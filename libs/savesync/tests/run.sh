#!/bin/zsh
# SaveSync tests: the decision table, then two devices (a "mac" and an "ipad") sharing one cloud folder through every
# case in ARCHITECTURE.md. Needs only Xcode's command line tools. Usage: libs/savesync/tests/run.sh
set -eu
here=${0:A:h}; lib=${here:h}
T=$(mktemp -d); trap 'rm -rf $T' EXIT
xcrun clang -Wall -Wextra -Werror $here/decide_test.c -fobjc-arc $lib/savesync.m -framework Foundation -o $T/decide
xcrun clang -Wall -Wextra -Werror $here/device.c -fobjc-arc $lib/savesync.m -framework Foundation -o $T/device
$T/decide
export SAVESYNC_DIR=$T/cloud
fails=0
dev() { $T/device $T/$1 $T/$1/save "$2" "${@:3}" 2>&1 | sed "s/^\[test\] iCloud saves: /  $1: /"; }
check() { if [[ $(cat $T/$1/save/slot/progress.sav 2>/dev/null) == "$2" ]]; then echo "ok   $3"; else echo "FAIL $3: $1 has '$(cat $T/$1/save/slot/progress.sav 2>/dev/null)', want '$2'"; fails=$((fails+1)); fi; }
cloud() { [[ $(cat $T/cloud/files/slot/progress.sav) == "$1" ]] && echo "ok   $2" || { echo "FAIL $2: cloud has '$(cat $T/cloud/files/slot/progress.sav)'"; fails=$((fails+1)); }; }

mkdir -p $T/mac/save/slot $T/ipad/save; echo A > $T/mac/save/slot/progress.sav; echo O > $T/mac/save/option.sav
dev mac 1.0;                                   cloud A "first device sends"
dev ipad 1.0;                                  check ipad A "an empty device takes the cloud's saves"
dev ipad 1.0;                                  check ipad A "in step"
echo B > $T/ipad/save/slot/progress.sav; dev ipad 1.0 commit; cloud B "a save committed during play is uploaded"
dev mac 1.0;                                   check mac B "the other device takes it at its next start"
ls $T/mac/save-backups | grep -q before-icloud && echo "ok   what was replaced is backed up" || { echo "FAIL no backup"; fails=$((fails+1)); }
echo C > $T/mac/save/slot/progress.sav; sleep 1.1; echo D > $T/ipad/save/slot/progress.sav
dev ipad 1.0 commit;                           cloud D "the newer device uploads"
dev mac 1.0;                                   check mac D "both changed: the newer save wins"
# the mac is playing (started in step) when the ipad uploads E; the mac's own save F during play must not replace it
dev mac 1.0 commit "echo F > $T/mac/save/slot/progress.sav; echo E > $T/ipad/save/slot/progress.sav; $T/device $T/ipad $T/ipad/save 1.0 commit >/dev/null 2>&1"
cloud E "an upload during play never overwrites another device's newer upload"
dev mac 1.0;                                   check mac E "...and is settled at the next start: the newer (E) wins"
grep -rqx F $T/mac/save-backups && echo "ok   ...and the older (F) is kept in save-backups" || { echo "FAIL F was lost"; fails=$((fails+1)); }
python3 -c "import json,sys;p=sys.argv[1];m=json.load(open(p));m['version']='9.9';json.dump(m,open(p,'w'))" $T/cloud/manifest.json
echo G > $T/mac/save/slot/progress.sav; dev mac 1.0; check mac G "a save from a newer game version is not taken"
rm -rf $T/fresh; mkdir -p $T/fresh/save; SAVESYNC=0 $T/device $T/fresh $T/fresh/save 1.0 2>/dev/null && echo "FAIL SAVESYNC=0" || echo "ok   SAVESYNC=0 turns it off"
[[ $fails == 0 ]] && echo "all passed" || { echo "$fails failed"; exit 1; }

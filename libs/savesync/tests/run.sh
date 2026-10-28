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
# storage/ is a cache the game rewrites at every start (savesync_config.exclude): not a save, never synced or removed
mkdir -p $T/mac/save/storage; echo key1 > $T/mac/save/storage/CacheStorageKey.dat
out=$(dev mac 1.0); [[ $out == *"in step"* ]] && echo "ok   a cache change alone is no save (in step, nothing sent)" || { echo "FAIL a cache change was taken for a save: $out"; fails=$((fails+1)); }
grep -q storage $T/cloud/manifest.json && { echo "FAIL the cache went to the cloud"; fails=$((fails+1)); } || echo "ok   the cache stays off the cloud"
mkdir -p $T/ipad/save/storage; echo ipadkey > $T/ipad/save/storage/CacheStorageKey.dat
echo H > $T/mac/save/slot/progress.sav; dev mac 1.0 commit >/dev/null; dev ipad 1.0 >/dev/null
check ipad H "the other device takes the new save"
[[ $(cat $T/ipad/save/storage/CacheStorageKey.dat 2>/dev/null) == ipadkey ]] && echo "ok   ...and keeps its own cache" || { echo "FAIL taking the cloud's saves removed the cache"; fails=$((fails+1)); }
# a quit through _exit (the Mac window) waits while savesync_busy: 1 right after a commit, 0 once the upload has run
$T/device $T/mac $T/mac/save 1.0 busy "echo I > $T/mac/save/slot/progress.sav" 2>&1 | sed "s/^\[test\] iCloud saves: /  mac: /"
cloud I "busy until the delayed upload has run, then the cloud has it"
python3 -c "import json,sys;p=sys.argv[1];m=json.load(open(p));m['version']='9.9';json.dump(m,open(p,'w'))" $T/cloud/manifest.json
echo G > $T/mac/save/slot/progress.sav; dev mac 1.0; check mac G "a save from a newer game version is not taken"
# a cloud copy sent before storage/ was excluded lists the cache: still read (its hash covers it), the cache never taken
export SAVESYNC_DIR=$T/oldcloud; mkdir -p $T/om/save/slot $T/om/save/storage $T/oi/save
echo P > $T/om/save/slot/progress.sav; echo key > $T/om/save/storage/CacheStorageKey.dat
DEVICE_OLD=1 dev om 1.0 >/dev/null; DEVICE_OLD=1 dev oi 1.0 >/dev/null
grep -q storage $T/oldcloud/manifest.json && echo "ok   (an old build sent the cache)" || { echo "FAIL the old build did not send the cache"; fails=$((fails+1)); }
echo key2 > $T/om/save/storage/CacheStorageKey.dat
out=$(dev om 1.0); [[ $out == *"in step"* ]] && echo "ok   an old cloud copy with the cache in it reads (in step)" || { echo "FAIL old cloud copy: $out"; fails=$((fails+1)); }
echo Q > $T/om/save/slot/progress.sav; dev om 1.0 commit >/dev/null
[[ $(cat $T/oldcloud/files/slot/progress.sav) == Q ]] && echo "ok   ...a new save goes up" || { echo "FAIL the new save was not sent"; fails=$((fails+1)); }
grep -q storage $T/oldcloud/manifest.json || [[ -e $T/oldcloud/files/storage ]] && { echo "FAIL the old cache stayed in the cloud"; fails=$((fails+1)); } || echo "ok   ...and the old cache leaves the cloud"
echo ipadkey > $T/oi/save/storage/CacheStorageKey.dat; dev oi 1.0 >/dev/null
check oi Q "a device that last agreed on the old copy takes the new save"
[[ $(cat $T/oi/save/storage/CacheStorageKey.dat) == ipadkey ]] && echo "ok   ...and keeps its own cache" || { echo "FAIL its cache was replaced"; fails=$((fails+1)); }
export SAVESYNC_DIR=$T/cloud
rm -rf $T/fresh; mkdir -p $T/fresh/save; SAVESYNC=0 $T/device $T/fresh $T/fresh/save 1.0 2>/dev/null && echo "FAIL SAVESYNC=0" || echo "ok   SAVESYNC=0 turns it off"
[[ $fails == 0 ]] && echo "all passed" || { echo "$fails failed"; exit 1; }

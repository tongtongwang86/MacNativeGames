#!/bin/zsh
# The boot screen in the Simulator: builds BootPreview and runs it on a booted (or the named) simulator.
#   games/totk/ios/tools/boot_preview/run.sh <launch pattern folder> [simulator name or UDID] [style] [step]
# The pattern folder is what ios/tools/launch_pattern.py wrote. Tap the screen to switch the contrast style.
set -eu
here=${0:A:h}
pattern=${1:?the launch pattern folder (ios/tools/launch_pattern.py)}
device=${2:-booted}
derived=${TMPDIR:-/tmp}/totk-boot-preview
# a folder reference keeps its own name in the app: stage the pattern as "launch", as build_ipa.py names it in GameData
rm -rf $derived/launch; mkdir -p $derived; cp -R ${pattern:A} $derived/launch
export LAUNCH_PATTERN_DIR=$derived/launch
cd $here
mkdir -p Sources
[[ -f Sources/Info.plist ]] || echo '<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict/></plist>' > Sources/Info.plist
xcodegen generate --quiet
xcodebuild -project BootPreview.xcodeproj -scheme BootPreview -configuration Debug -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath $derived -quiet build
app=$derived/Build/Products/Debug-iphonesimulator/BootPreview.app
[[ $device == booted ]] || xcrun simctl boot "$device" 2>/dev/null || true
xcrun simctl install "$device" $app
env SIMCTL_CHILD_TOTK_BOOT_STYLE=${3:-} SIMCTL_CHILD_TOTK_BOOT_STEP=${4:-} xcrun simctl launch --terminate-running-process "$device" org.macnativegames.bootpreview

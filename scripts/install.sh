#!/bin/sh
# Copy the built plugins into the user plugin folders, where Ableton Live finds them.
set -e
cd "$(dirname "$0")/.."
OUT=build/MeowTune_artefacts/Release
VST3=~/Library/Audio/Plug-Ins/VST3
AU=~/Library/Audio/Plug-Ins/Components
mkdir -p "$VST3" "$AU"
rm -rf "$VST3/MeowTune.vst3" "$AU/MeowTune.component"
cp -R "$OUT/VST3/MeowTune.vst3" "$VST3/"
cp -R "$OUT/AU/MeowTune.component" "$AU/"
killall -9 AudioComponentRegistrar 2>/dev/null || true
echo "> INSTALLED $VST3/MeowTune.vst3"
echo "> INSTALLED $AU/MeowTune.component"

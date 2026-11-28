#!/bin/bash
# usage: run_route.sh <log> <route dir name under task68_rt> <decomp mode> <seconds> <maxsec> [capture_every]  (task-68; route dir holds save/, options.ini, input.txt)
# Copies <src>/save fresh to a per-run dir so every run starts from the same save.
cd "F:/Super Mario RPG"
B='/a/AI/Recomps/smrpg-work/task68_rt'
WB='A:\AI\Recomps\smrpg-work\task68_rt'
rm -rf "$B/run_$1"; mkdir -p "$B/run_$1"; cp -r "$B/$2/save" "$B/run_$1/save"
cp "$B/$2/options.ini" "$B/$2/input.txt" "$B/run_$1/"
CAP=()
if [ -n "$6" ]; then CAP=(--env "SMRPG_CAPTURE_DIR=$WB\run_$1\frames" --env "SMRPG_CAPTURE_EVERY=$6"); fi
python -I tools/rtprep/run_game.py --seconds "$4" --log "$1" --env SMRPG_DECOMP="$3" \
  --env "SMRPG_SAVE=$WB\run_$1\save" --env "SMRPG_OPTIONS=$WB\run_$1\options.ini" --env "SMRPG_INPUT_FILE=$WB\run_$1\input.txt" \
  "${CAP[@]}" --env SDL_AUDIO_DRIVER=dummy --env SDL_AUDIODRIVER=dummy -- --max-seconds "$5" > "$B/$1.out" 2>&1
echo done >> "$B/$1.out"

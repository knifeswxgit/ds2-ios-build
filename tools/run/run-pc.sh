#!/usr/bin/env bash
# PC run, headless (offscreen SDL, lavapipe, null ALSA). Logs: out/run/logs/<tag>.*
# Usage: tools/run/run-pc.sh <tag> <seconds> [extra --cvar=value ...]
# Typical extras (same as the Switch TOML): see tools/run/pc-args-example.txt
set -uo pipefail
. "$(dirname "$0")/../build/env.sh"
T=$1; S=$2; shift 2
RUN="$DS2_ROOT/out/run"; mkdir -p "$RUN/logs" "$RUN/user" "$RUN/cache"
B="$DS2_ROOT/sdk/out/linux-amd64/ds2"
echo "CMD: $B --game_data_root=$GAME_ROOT ... $*" > "$RUN/logs/$T.console"
timeout -s KILL "$S" "$B" --game_data_root="$GAME_ROOT" --user_data_root="$RUN/user" --cache_root="$RUN/cache" \
  --video_driver=offscreen --headless=true --log_file="$RUN/logs/$T.log" \
  --ds2_install_dlc="$DS2_DLC" "$@" >> "$RUN/logs/$T.console" 2>&1
echo "EXIT=$?" >> "$RUN/logs/$T.console"

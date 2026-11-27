#!/usr/bin/env bash
# Starts a throwaway eosd with a small generated library and runs the live tests
# against it.   usage: live.sh <eosd> <eos-admin> <libeos_live_tests> [gtest args]
# Exits 77 (skipped) when ffmpeg is missing.
set -euo pipefail
eosd=$1 admin=$2 tests=$3
shift 3

command -v ffmpeg > /dev/null || { echo "ffmpeg not installed; skipping"; exit 77; }

dir=$(mktemp -d)
pid=""
cleanup() {
  [ -n "$pid" ] && kill "$pid" 2> /dev/null && wait "$pid" 2> /dev/null
  rm -rf "$dir"
}
trap cleanup EXIT

# flac <path> <freq> <tag=value>...
flac() {
  local path=$1 freq=$2 meta=()
  shift 2
  for kv in "$@"; do meta+=(-metadata "$kv"); done
  mkdir -p "$(dirname "$path")"
  ffmpeg -nostdin -loglevel error -f lavfi -i "sine=frequency=$freq:duration=2" "${meta[@]}" "$path"
}

music=$dir/music
flac "$music/A/01.flac" 440 TITLE=Opening "ALBUM=Album A" ARTIST=Alice ALBUMARTIST=Alice COMPOSER=Carol \
  TRACKNUMBER=1 DISCNUMBER=1 GENRE=Soundtrack DATE=2020-04-01 "WORK=Main Theme" "COLLECTION_WORK=Game Z" \
  "COLLECTION=Soundtracks/Series A" "COLLECTION_ALBUM=Best Of" "REPLAYGAIN_TRACK_GAIN=-6.50 dB"
flac "$music/A/02.flac" 550 TITLE=Battle "ALBUM=Album A" "ARTIST=Alice; Bob" ALBUMARTIST=Alice \
  TRACKNUMBER=2 DISCNUMBER=1 GENRE=Soundtrack DATE=2020-04-01 "COLLECTION_WORK=Game Z" \
  "COLLECTION=Soundtracks/Series A"
# The same WORK and COMPOSER as A/01: one composition, two arrangements.
flac "$music/B/01.flac" 660 "TITLE=メインテーマ (Piano)" "ALBUM=Album B" ARTIST=Erin "ALBUMARTIST=The Erin Band" \
  COMPOSER=Carol \
  TRACKNUMBER=1 GENRE=Rock DATE=1999 "WORK=Main Theme" "ARRANGEMENT=Piano Ver."
ffmpeg -nostdin -loglevel error -f lavfi -i "color=c=red:s=64x64" -frames:v 1 "$music/A/cover.png"

port=$((20000 + RANDOM % 20000))
cat > "$dir/eos.toml" << EOF
[server]
listen = "127.0.0.1:$port"
data_dir = "$dir/data"
log_level = "warn"

[[library]]
name = "Music"
path = "$music"

[scanner]
interval = "0"
EOF

printf 'pw-alice\n' | "$admin" --config "$dir/eos.toml" user add alice --admin > /dev/null
printf 'pw-bob\n' | "$admin" --config "$dir/eos.toml" user add bob > /dev/null
"$admin" --config "$dir/eos.toml" scan > /dev/null

"$eosd" --config "$dir/eos.toml" &
pid=$!
for _ in $(seq 100); do
  (exec 3<> "/dev/tcp/127.0.0.1/$port") 2> /dev/null && break
  kill -0 "$pid" 2> /dev/null || { echo "eosd exited"; exit 1; }
  sleep 0.1
done

EOS_TEST_URL="http://127.0.0.1:$port" EOS_TEST_USER=alice EOS_TEST_PASSWORD=pw-alice \
  EOS_TEST_USER2=bob EOS_TEST_PASSWORD2=pw-bob "$tests" "$@"

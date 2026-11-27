#!/usr/bin/env bash
# Runs a command inside the eos-dev container with the repository mounted at /src.
#   scripts/dev.sh build     configure + build (build/ in the repo)
#   scripts/dev.sh test      build, then run the test suite
#   scripts/dev.sh <cmd...>  any other command
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"

if ! docker image inspect eos-dev > /dev/null 2>&1; then
  docker build -f "$root/docker/dev.Dockerfile" -t eos-dev "$root/docker"
fi

build='cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build'
case "${1:-build}" in
  build) cmd="$build" ;;
  test)  cmd="$build && ctest --test-dir build --output-on-failure" ;;
  *)     cmd="$*" ;;
esac

tty_flag=""
[ -t 0 ] && tty_flag="-it"
exec docker run --rm $tty_flag -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$root:/src" -w /src eos-dev bash -c "$cmd"

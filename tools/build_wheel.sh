#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/bazel/versions.bzl"

check_repo() {
  local name="$1" path="$2" expected="${3:-}"
  local actual dirty
  actual="$(git -C "$path" rev-parse HEAD)"
  if [[ -n "$expected" && "$actual" != "$expected" ]]; then
    echo "$name is at $actual; expected $expected" >&2
    exit 1
  fi
  dirty="$(git -C "$path" status --porcelain)"
  if [[ -n "$dirty" ]]; then
    echo "$name has uncommitted files; release inputs must be clean" >&2
    exit 1
  fi
}

modular_root="${XMOJO_MODULAR_ROOT:?export XMOJO_MODULAR_ROOT to the pinned Modular worktree}"
if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
  echo "the current wheel target is only defined for Apple Silicon" >&2
  exit 1
fi

check_repo Modular "$modular_root" "$MODULAR_REVISION"
check_repo nlohmann-json "$root/../nlohmann-json" "$NLOHMANN_JSON_REVISION"
check_repo xeus "$root/../xeus" "$XEUS_REVISION"
check_repo xeus-zmq "$root/../xeus-zmq" "$XEUS_ZMQ_REVISION"
check_repo libzmq "$root/../libzmq" "$LIBZMQ_REVISION"
check_repo cppzmq "$root/../cppzmq" "$CPPZMQ_REVISION"
if [[ "${XMOJO_ALLOW_DIRTY:-0}" != "1" ]]; then check_repo xmojo "$root"; fi

grep -Fqx "max-core = \"==${MAX_CORE_VERSION}\"" "$root/pixi.toml" || {
  echo "pixi.toml does not pin max-core==$MAX_CORE_VERSION" >&2
  exit 1
}
grep -Fq "max-core-${MAX_CORE_VERSION}-release.conda" "$root/pixi.lock" || {
  echo "pixi.lock does not contain max-core==$MAX_CORE_VERSION" >&2
  exit 1
}

wrapper="${XMOJO_BAZEL_WRAPPER:-$root/bazelw}"
if [[ "$wrapper" != /* ]]; then wrapper="$root/${wrapper#./}"; fi
"$wrapper" build -c opt @xmojo//:wheel

wheel="$modular_root/bazel-bin/external/+local_repository+xmojo/xmojo-${XMOJO_VERSION}-py3-none-${WHEEL_PLATFORM}.whl"
[[ -f "$wheel" ]] || { echo "wheel output not found at $wheel" >&2; exit 1; }
checksum="$(shasum -a 256 "$wheel")"
echo "$wheel"
echo "sha256 ${checksum%% *}"

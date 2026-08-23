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

check_modular() {
  local path="$1" actual dirty changes
  actual="$(git -C "$path" rev-parse HEAD)"
  if [[ "$actual" != "$MODULAR_REVISION" ]]; then
    echo "Modular is at $actual; expected $MODULAR_REVISION" >&2
    exit 1
  fi
  dirty="$(git -C "$path" status --porcelain --untracked-files=no)"
  changes="$(git -C "$path" diff --numstat -- MODULE.bazel)"
  if [[ "$dirty" != " M MODULE.bazel" || "$changes" != $'1\t0\tMODULE.bazel' ]] || \
      ! grep -Fqx 'llvm_configure.configure(extra_targets = ["SPIRV"])' \
        "$path/MODULE.bazel"; then
    echo "Modular must differ from $MODULAR_REVISION only by enabling LLVM's SPIRV target in MODULE.bazel" >&2
    exit 1
  fi
}

deps_root="${XMOJO_DEPS_ROOT:-$(dirname "$root")}" 
modular_root="${XMOJO_MODULAR_ROOT:-$deps_root/modular}"
case "$(uname -s):$(uname -m)" in
  Darwin:arm64) wheel_platform="$WHEEL_PLATFORM_MACOS_ARM64" ;;
  Linux:aarch64) wheel_platform="$WHEEL_PLATFORM_LINUX_AARCH64" ;;
  Linux:x86_64) wheel_platform="$WHEEL_PLATFORM_LINUX_X86_64" ;;
  *) echo "xmojo wheels do not support $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac

check_modular "$modular_root"
check_repo nlohmann-json "$deps_root/nlohmann-json" "$NLOHMANN_JSON_REVISION"
check_repo xeus "$deps_root/xeus" "$XEUS_REVISION"
check_repo xeus-zmq "$deps_root/xeus-zmq" "$XEUS_ZMQ_REVISION"
check_repo libzmq "$deps_root/libzmq" "$LIBZMQ_REVISION"
check_repo cppzmq "$deps_root/cppzmq" "$CPPZMQ_REVISION"
if [[ "${XMOJO_ALLOW_DIRTY:-0}" != "1" ]]; then check_repo xmojo "$root"; fi

grep -Fqx "max-core = \"==${MAX_CORE_VERSION}\"" "$root/pixi.toml" || {
  echo "pixi.toml does not pin max-core==$MAX_CORE_VERSION" >&2
  exit 1
}
grep -Fqx "version = \"${XMOJO_VERSION}\"" "$root/pyproject.toml" || {
  echo "pyproject.toml does not declare xmojo==$XMOJO_VERSION" >&2
  exit 1
}
grep -Fqx "modular-gpu = [\"max-core==${MAX_CORE_VERSION}\"]" "$root/pyproject.toml" || {
  echo "pyproject.toml does not pin max-core==$MAX_CORE_VERSION" >&2
  exit 1
}
export XMOJO_DEPS_ROOT="$deps_root"
export XMOJO_MODULAR_ROOT="$modular_root"
uv build --wheel

wheel="$root/dist/xmojo-${XMOJO_VERSION}-py3-none-${wheel_platform}.whl"
[[ -f "$wheel" ]] || { echo "wheel output not found at $wheel" >&2; exit 1; }
if command -v sha256sum >/dev/null; then checksum="$(sha256sum "$wheel")"
else checksum="$(shasum -a 256 "$wheel")"
fi
echo "$wheel"
echo "sha256 ${checksum%% *}"

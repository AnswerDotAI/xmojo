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

deps_root="${XMOJO_DEPS_ROOT:-$(dirname "$root")}" 
modular_root="${XMOJO_MODULAR_ROOT:-$deps_root/modular}"
case "$(uname -s):$(uname -m)" in
  Darwin:arm64) wheel_platform="$WHEEL_PLATFORM_MACOS_ARM64" ;;
  Linux:aarch64) wheel_platform="$WHEEL_PLATFORM_LINUX_AARCH64" ;;
  Linux:x86_64) wheel_platform="$WHEEL_PLATFORM_LINUX_X86_64" ;;
  *) echo "xmojo wheels do not support $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac

check_repo modular "$modular_root" "$MODULAR_REVISION"
if [[ "${XMOJO_ALLOW_DIRTY:-0}" != "1" ]]; then check_repo xmojo "$root"; fi

grep -Fqx "version = \"${XMOJO_VERSION}\"" "$root/pyproject.toml" || {
  echo "pyproject.toml does not declare xmojo==$XMOJO_VERSION" >&2
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

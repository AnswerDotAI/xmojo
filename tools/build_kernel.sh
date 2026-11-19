#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${1:?usage: build_kernel.sh BUILD_ROOT}"
if [[ "$(uname -s)" == "Darwin" ]]; then export MACOSX_DEPLOYMENT_TARGET=11.0; fi

cargo build --release --manifest-path "$root/kernel/Cargo.toml" \
  --target-dir "$build_root/target"
python "$root/tools/rust_licenses.py" "$build_root/install"
cp "$root/bazel/kernel/BUILD.bazel" "$root/bazel/kernel/REPO.bazel" "$build_root/install/"
archive="$build_root/target/release/libxmojo_kernel.a"
if ! cmp -s "$archive" "$build_root/install/libxmojo_kernel.a"; then
  cp "$archive" "$build_root/install/libxmojo_kernel.a"
fi

#!/usr/bin/env bash

set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_root="${1:?usage: build_xeus_deps.sh DEPS_ROOT}"
output_user_root="${2:?usage: build_xeus_deps.sh DEPS_ROOT OUTPUT_USER_ROOT [prepare]}"
mode="${3:-build}"
modular_root="${XMOJO_MODULAR_ROOT:?export XMOJO_MODULAR_ROOT to the Modular checkout to use}"
build_root="$deps_root/build"
install_root="$deps_root/install"

mkdir -p "$install_root"
cp "$script_root/cmake/dependencies/BUILD.bazel" "$install_root/BUILD.bazel"
cp "$script_root/cmake/dependencies/REPO.bazel" "$install_root/REPO.bazel"
if [[ "$mode" == "prepare" ]]; then exit 0; fi

output_base="$("$modular_root/bazelw" --output_user_root="$output_user_root" info output_base)"
boringssl_include="$output_base/external/boringssl+/include"

if [[ ! -f "$boringssl_include/openssl/hmac.h" ]]; then
  "$modular_root/bazelw" --output_user_root="$output_user_root" \
    query '@@boringssl+//:crypto' >/dev/null
fi

if [[ ! -f "$build_root/build.ninja" ]] || \
    ! grep -Fqx "BORINGSSL_INCLUDE_DIR:PATH=$boringssl_include" \
      "$build_root/CMakeCache.txt"; then
  cmake -S "$script_root/cmake/dependencies" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_root" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DBORINGSSL_INCLUDE_DIR="$boringssl_include"
fi

cmake --build "$build_root"
cmake --install "$build_root" >/dev/null

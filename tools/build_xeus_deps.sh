#!/usr/bin/env bash

set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="$script_root/.xmojo-deps/build"
install_root="$script_root/.xmojo-deps/install"
modular_root="$script_root/../modular"
boringssl_include="$modular_root/bazel-modular/external/boringssl+/include"

if [[ ! -f "$boringssl_include/openssl/hmac.h" ]]; then
  # `bazelw info` creates the output tree but does not fetch external repos.
  "$modular_root/bazelw" query '@@boringssl+//:crypto' >/dev/null
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

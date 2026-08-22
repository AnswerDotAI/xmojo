#!/usr/bin/env bash

set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_deps_root="${XMOJO_DEPS_ROOT:-$(dirname "$script_root")}"
deps_root="${1:?usage: build_xeus_deps.sh DEPS_ROOT}"
output_user_root="${2:?usage: build_xeus_deps.sh DEPS_ROOT OUTPUT_USER_ROOT [prepare]}"
mode="${3:-build}"
modular_root="${XMOJO_MODULAR_ROOT:-$source_deps_root/modular}"
build_root="$deps_root/build"
install_root="$deps_root/install"
deployment_target=11.0
cmake_platform_args=()
cache_platform_checks=()

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

case "$(uname -s)" in
Darwin)
  cmake_platform_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=$deployment_target")
  cache_platform_checks+=("CMAKE_OSX_DEPLOYMENT_TARGET:$deployment_target")
  ;;
Linux)
  case "$(uname -m)" in
  aarch64) target_arch=aarch64 ;;
  x86_64) target_arch=x86_64 ;;
  *) echo "Unsupported Linux architecture: $(uname -m)" >&2; exit 2 ;;
  esac
  sysroot_repo="sysroot-jammy-$target_arch"
  clang_repo="clang-linux-$target_arch"
  if [[ ! -d "$output_base/external/$sysroot_repo+/sysroot" ]]; then
    "$modular_root/bazelw" --output_user_root="$output_user_root" \
      query "@$sysroot_repo//sysroot:directory" >/dev/null
  fi
  if [[ ! -x "$output_base/external/+http_archive+$clang_repo/bin/clang++" ]]; then
    "$modular_root/bazelw" --output_user_root="$output_user_root" \
      query "@$clang_repo//:bin/clang++" >/dev/null
  fi
  sysroot="$output_base/external/$sysroot_repo+/sysroot"
  clang_root="$output_base/external/+http_archive+$clang_repo"
  cmake_platform_args+=(
    "-DCMAKE_SYSROOT=$sysroot"
    "-DCMAKE_C_COMPILER=$clang_root/bin/clang"
    "-DCMAKE_CXX_COMPILER=$clang_root/bin/clang++"
  )
  cache_platform_checks+=(
    "CMAKE_SYSROOT:$sysroot"
    "CMAKE_C_COMPILER:$clang_root/bin/clang"
    "CMAKE_CXX_COMPILER:$clang_root/bin/clang++"
  )
  ;;
*)
  echo "Unsupported host platform: $(uname -s)" >&2
  exit 2
  ;;
esac

cache_matches_platform=1
for check in "${cache_platform_checks[@]}"; do
  name="${check%%:*}"
  value="${check#*:}"
  cached_value=""
  if [[ -f "$build_root/CMakeCache.txt" ]]; then
    cached_value="$(sed -n "s|^$name:[^=]*=||p" "$build_root/CMakeCache.txt")"
  fi
  if [[ "$cached_value" != "$value" ]]; then
    cache_matches_platform=0
    break
  fi
done

if [[ ! -f "$build_root/build.ninja" || ! -f "$build_root/CMakeFiles/rules.ninja" ]] || \
    ! grep -Fqx "BORINGSSL_INCLUDE_DIR:PATH=$boringssl_include" \
      "$build_root/CMakeCache.txt" || \
    ! grep -Fqx "XMOJO_DEPS_ROOT:PATH=$source_deps_root" \
      "$build_root/CMakeCache.txt" || (( ! cache_matches_platform )); then
  cmake -S "$script_root/cmake/dependencies" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_root" \
    -DBORINGSSL_INCLUDE_DIR="$boringssl_include" \
    -DXMOJO_DEPS_ROOT="$source_deps_root" \
    "${cmake_platform_args[@]}"
fi

cmake --build "$build_root"
cmake --install "$build_root" >/dev/null

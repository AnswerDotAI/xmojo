#!/usr/bin/env bash

set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_root="${XMOJO_DEPS_ROOT:-$(dirname "$script_root")}"
source "$script_root/bazel/versions.bzl"

clone_revision() {
  local name="$1" url="$2" revision="$3" destination="$deps_root/$1"
  if [[ -e "$destination" ]]; then
    if [[ ! -d "$destination/.git" ]]; then
      echo "$destination exists but is not a git worktree" >&2
      exit 2
    fi
    echo "Using existing $destination"
    return
  fi

  echo "Cloning $name at $revision"
  local temporary
  temporary="$(mktemp -d "$deps_root/.xmojo-$name.XXXXXX")"
  if git init -q "$temporary" &&
      git -C "$temporary" remote add origin "$url" &&
      git -C "$temporary" fetch -q --depth 1 origin "$revision" &&
      git -C "$temporary" checkout -q --detach FETCH_HEAD; then
    mv "$temporary" "$destination"
  else
    rm -rf "$temporary"
    return 1
  fi
}

mkdir -p "$deps_root"
clone_revision modular https://github.com/modular/modular.git "$MODULAR_REVISION"
clone_revision nlohmann-json https://github.com/nlohmann/json.git "$NLOHMANN_JSON_REVISION"
clone_revision xeus https://github.com/jupyter-xeus/xeus.git "$XEUS_REVISION"
clone_revision xeus-zmq https://github.com/jupyter-xeus/xeus-zmq.git "$XEUS_ZMQ_REVISION"
clone_revision libzmq https://github.com/zeromq/libzmq.git "$LIBZMQ_REVISION"
clone_revision cppzmq https://github.com/zeromq/cppzmq.git "$CPPZMQ_REVISION"

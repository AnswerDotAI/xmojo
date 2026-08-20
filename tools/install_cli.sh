#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin_dir="${1:-${HOME}/.local/bin}"
mkdir -p "$bin_dir"
bin_dir="$(cd "$bin_dir" && pwd)"

for cli in mojoorc xmojo; do
  "$repo_root/bazelw" run "--script_path=$bin_dir/$cli" "@xmojo//:$cli"
done

echo "Installed mojoorc and xmojo in $bin_dir"

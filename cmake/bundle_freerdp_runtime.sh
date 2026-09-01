#!/usr/bin/env bash
# Recursively copy MinGW DLL dependencies for FreeRDP next to clientosh.exe.
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: bundle_freerdp_runtime.sh <freerdp_bin_dir> <dest_dir>" >&2
  exit 1
fi

bin_dir="$1"
dest_dir="$2"
mkdir -p "$dest_dir"

entries=()
for name in libfreerdp-client3.dll libfreerdp-client2.dll; do
  if [[ -f "${bin_dir}/${name}" ]]; then
    entries+=("${bin_dir}/${name}")
    break
  fi
done
if [[ ${#entries[@]} -eq 0 ]]; then
  for name in libfreerdp3.dll libfreerdp2.dll; do
    if [[ -f "${bin_dir}/${name}" ]]; then
      entries+=("${bin_dir}/${name}")
      break
    fi
  done
fi

if [[ ${#entries[@]} -eq 0 ]]; then
  echo "no FreeRDP DLL found in ${bin_dir}" >&2
  exit 1
fi

declare -A seen=()
queue=("${entries[@]}")
while ((${#queue[@]} > 0)); do
  dll="${queue[0]}"
  queue=("${queue[@]:1}")
  [[ -f "$dll" ]] || continue
  key=$(basename "$dll" | tr '[:upper:]' '[:lower:]')
  [[ -n "${seen[$key]:-}" ]] && continue
  seen[$key]=1
  cp -f "$dll" "$dest_dir/"
  while IFS= read -r dep; do
    [[ -n "$dep" && -f "$dep" ]] && queue+=("$dep")
  done < <(ldd "$dll" 2>/dev/null | awk '/=> \// {print $3}' | grep -E '/(mingw64|ucrt64)/bin/' || true)
done

echo "${#seen[@]}"

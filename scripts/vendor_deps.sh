#!/usr/bin/env bash
# vendor_deps.sh - Vendor third-party dependencies (simdjson, googletest)
# Usage: vendor_deps.sh <dep> <tag>     (online: clone and vendor)
#        vendor_deps.sh --verify         (offline: verify both trees)

set -euo pipefail

# Dependency configuration table (one place, reused for all ops)
# Format: NAME|UPSTREAM_URL|ALLOWLIST|LICENSE
declare -a DEPS=(
  "simdjson|https://github.com/simdjson/simdjson.git|simdjson.h simdjson.cpp LICENSE LICENSE-MIT|Apache-2.0/MIT"
  "googletest|https://github.com/google/googletest.git|CMakeLists.txt googletest/CMakeLists.txt googletest/cmake/ googletest/include/ googletest/src/ LICENSE|BSD-3-Clause"
)

# Parse dep config by name, return the field
dep_config() {
  local dep_name="$1" field="$2"
  local name url allowlist license
  for row in "${DEPS[@]:-}"; do
    IFS='|' read -r name url allowlist license <<<"$row"
    if [[ "$name" == "$dep_name" ]]; then
      case "$field" in
        name) echo "$name" ;;
        url) echo "$url" ;;
        allowlist) echo "$allowlist" ;;
        license) echo "$license" ;;
      esac
      return 0
    fi
  done
  return 1
}

# List all known dependencies
list_deps() {
  local name
  for row in "${DEPS[@]:-}"; do
    IFS='|' read -r name _ _ _ <<<"$row"
    echo "$name"
  done
}

# Hash algorithm selection: sha256sum if available, else shasum -a 256
hash_cmd() {
  if command -v sha256sum >/dev/null 2>&1; then
    echo "sha256sum"
  else
    echo "shasum -a 256"
  fi
}

# Compute deterministic tree checksum (sorted, no paths leading with ./)
# Input: tree root directory
# Output: SHA-256 of sorted "<file_sha>  <rel_path>" lines for all files except VENDOR.md
tree_checksum() {
  local tree_root="$1"
  local hashcmd rel_path file_sha
  hashcmd=$(hash_cmd)
  
  # Find all files except VENDOR.md, print as "<sha>  <rel_path>", sort, then hash
  (
    cd "$tree_root" || return 1
    find . -type f ! -name VENDOR.md -print0 | \
      sort -z | \
      while IFS= read -r -d '' file; do
        rel_path="${file#./}"
        file_sha=$($hashcmd < "$file" | awk '{print $1}')
        echo "$file_sha  $rel_path"
      done | \
      LC_ALL=C sort
  ) | $hashcmd | awk '{print $1}'
}

# Generate VENDOR.md in the given directory
vendor_md() {
  local target_dir="$1" dep_name="$2" tag="$3" commit="$4"
  local url allowlist license tree_sha
  
  url=$(dep_config "$dep_name" url)
  allowlist=$(dep_config "$dep_name" allowlist)
  license=$(dep_config "$dep_name" license)
  tree_sha=$(tree_checksum "$target_dir")
  
  cat > "$target_dir/VENDOR.md" <<EOF
# Vendored: $dep_name

- Upstream: $url
- Tag: $tag
- Commit: $commit
- Paths: $allowlist
- Tree-SHA256: $tree_sha
- License: $license
- Local patches: none
EOF
}

# Vendor a dependency: clone, copy allowlist, generate VENDOR.md, atomically swap
vendor() {
  local dep_name="$1" tag="$2"
  local url allowlist commit clone_dir staging old_dir new_dir
  
  # Validate input
  if [[ -z "$dep_name" ]] || [[ -z "$tag" ]]; then
    echo "Usage: vendor_deps.sh <dep> <tag>" >&2
    echo "Known dependencies: $(list_deps | tr '\n' ' ')" >&2
    return 2
  fi
  
  url=$(dep_config "$dep_name" url) || {
    echo "Error: unknown dependency '$dep_name'" >&2
    echo "Known dependencies: $(list_deps | tr '\n' ' ')" >&2
    return 2
  }
  allowlist=$(dep_config "$dep_name" allowlist)
  
  # Ensure repo root and third_party dir exist
  local repo_root
  repo_root="$(cd "$(dirname "$0")/.." && pwd)"
  if [[ ! -d "$repo_root" ]]; then
    echo "Error: cannot determine repository root" >&2
    return 1
  fi
  mkdir -p "$repo_root/third_party"
  
  # Create temporary workspace on same filesystem as third_party
  # Use ${TMPDIR:-/tmp} for cloning, but staging on repo filesystem for atomic mv
  clone_dir=$(mktemp -d "${TMPDIR:-/tmp}/tmp_rovo_vendor.XXXXXXXX") || {
    echo "Error: failed to create temp directory for clone" >&2
    return 1
  }
  
  # Create staging on repo filesystem (same fs as third_party for atomic rename)
  staging=$(mktemp -d "$repo_root/third_party/.tmp_rovo_stage.XXXXXXXX") || {
    rm -rf "$clone_dir"
    echo "Error: failed to create staging directory" >&2
    return 1
  }
  
  # Setup cleanup trap (removes both dirs on any exit, including failure)
  # NOTE: trap is local to this function; vars are captured by value when set
  trap "rm -rf '$clone_dir' '$staging' 2>/dev/null || true" EXIT
  
  # Shallow-clone the tag
  echo "Cloning $dep_name from $url at tag $tag..." >&2
  git clone --depth 1 --branch "$tag" "$url" "$clone_dir/clone" 2>&1 | grep -v "^hint:" || true
  if [[ ! -d "$clone_dir/clone" ]]; then
    echo "Error: clone failed for tag $tag" >&2
    return 1
  fi
  
  # Get commit SHA
  commit=$(cd "$clone_dir/clone" && git rev-parse HEAD)
  
  # Create dep subdirectory in staging
  mkdir -p "$staging/$dep_name"
  
  if [[ "$dep_name" == "simdjson" ]]; then
    echo "Fetching single-header amalgamation release assets..." >&2
    local release_url="https://github.com/simdjson/simdjson/releases/download/${tag}"
    if curl -sSfL "$release_url/simdjson.h" -o "$staging/$dep_name/simdjson.h" && \
       curl -sSfL "$release_url/simdjson.cpp" -o "$staging/$dep_name/simdjson.cpp"; then
      echo "Downloaded official release assets for $tag." >&2
    else
      echo "Release asset download failed; generating via singleheader/amalgamate.py..." >&2
      python3 "$clone_dir/clone/singleheader/amalgamate.py" "$staging/$dep_name/simdjson.h" "$staging/$dep_name/simdjson.cpp"
    fi
    for lic in LICENSE LICENSE-MIT; do
      if [[ -f "$clone_dir/clone/$lic" ]]; then
        cp "$clone_dir/clone/$lic" "$staging/$dep_name/$lic"
      fi
    done
  else
    echo "Copying allowlist into staging directory..." >&2
    for path in $allowlist; do
      src="$clone_dir/clone/$path"
      if [[ ! -e "$src" ]]; then
        echo "Error: path '$path' not found in $dep_name at $tag" >&2
        return 1
      fi
      
      if [[ -d "$src" ]]; then
        # For directories: copy contents including dotfiles using cp -R . dest/
        mkdir -p "$staging/$dep_name/$path"
        if ! cp -R "$src/." "$staging/$dep_name/$path/"; then
          echo "Error: failed to copy directory $path" >&2
          return 1
        fi
      else
        # For files: copy with directory creation
        mkdir -p "$(dirname "$staging/$dep_name/$path")"
        if ! cp "$src" "$staging/$dep_name/$path"; then
          echo "Error: failed to copy file $path" >&2
          return 1
        fi
      fi
    done
  fi
  
  # Generate VENDOR.md
  vendor_md "$staging/$dep_name" "$dep_name" "$tag" "$commit"
  
  # Atomic swap into third_party/
  old_dir="$repo_root/third_party/${dep_name}.old"
  new_dir="$repo_root/third_party/$dep_name"
  
  # Move old tree aside if it exists
  if [[ -d "$new_dir" ]]; then
    if ! mv "$new_dir" "$old_dir"; then
      echo "Error: failed to move existing tree aside" >&2
      return 1
    fi
  fi
  
  # Move new tree in (from staging subdirectory to final location)
  if ! mv "$staging/$dep_name" "$new_dir"; then
    echo "Error: failed to move new tree into place; restoring old tree" >&2
    if [[ -d "$old_dir" ]]; then
      mv "$old_dir" "$new_dir" || true
    fi
    return 1
  fi
  
  # Remove old tree
  rm -rf "$old_dir"
  
  echo "Vendored $dep_name ($tag, commit $commit) at $new_dir" >&2
  echo "  Tree-SHA256: $(grep 'Tree-SHA256:' "$new_dir/VENDOR.md" | awk '{print $NF}')" >&2
}

# Verify both trees: check VENDOR.md exists and tree checksum matches
verify() {
  local repo_root dep_name status=0
  
  repo_root="$(cd "$(dirname "$0")/.." && pwd)"
  
  echo "Verifying vendored dependencies..." >&2
  
  for row in "${DEPS[@]:-}"; do
    IFS='|' read -r dep_name _ _ _ <<<"$row"
    local vendor_file="$repo_root/third_party/$dep_name/VENDOR.md"
    local tree_dir="$repo_root/third_party/$dep_name"
    
    # Check if tree exists
    if [[ ! -d "$tree_dir" ]]; then
      echo "$dep_name: missing tree at $tree_dir" >&2
      status=1
      continue
    fi
    
    # Check if VENDOR.md exists
    if [[ ! -f "$vendor_file" ]]; then
      echo "$dep_name: missing VENDOR.md" >&2
      status=1
      continue
    fi
    
    # Extract expected tree checksum from VENDOR.md
    local expected_sha recorded_sha
    expected_sha=$(grep 'Tree-SHA256:' "$vendor_file" | awk '{print $NF}')
    if [[ -z "$expected_sha" || "$expected_sha" == "Tree-SHA256:" ]]; then
      echo "$dep_name: malformed VENDOR.md (missing Tree-SHA256)" >&2
      status=1
      continue
    fi
    
    # Compute current tree checksum
    recorded_sha=$(tree_checksum "$tree_dir")
    
    if [[ "$expected_sha" != "$recorded_sha" ]]; then
      echo "$dep_name: checksum mismatch (expected $expected_sha, got $recorded_sha)" >&2
      status=1
      continue
    fi
    
    echo "$dep_name: OK (Tree-SHA256: $expected_sha)" >&2
  done
  
  return $status
}

# Usage
usage() {
  cat >&2 <<'EOF'
Usage: vendor_deps.sh [<dep> <tag> | --verify]

  vendor_deps.sh <dep> <tag>   Vendor a dependency (online)
  vendor_deps.sh --verify       Verify all vendored dependencies (offline)

Known dependencies:
  simdjson    v5.0.5 (Apache-2.0/MIT)
  googletest  v1.14.0 (BSD-3-Clause)

Examples:
  vendor_deps.sh simdjson v5.0.5
  vendor_deps.sh googletest v1.14.0
  vendor_deps.sh --verify
EOF
}

# Main
case "${1:-}" in
  --verify)
    verify
    exit $?
    ;;
  *)
    if [[ $# -ne 2 ]]; then
      usage
      exit 2
    fi
    vendor "$1" "$2"
    exit $?
    ;;
esac

#!/usr/bin/env bash

set -eu

ROOT_DIR="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  printf '%s\n' 'check-release-workflow: not inside a git repository' >&2
  exit 1
}

WORKFLOW="$ROOT_DIR/.github/workflows/build-linux.yml"
DEPLOYMENT_DOC="$ROOT_DIR/docs/deployment/release-artifacts.md"
README="$ROOT_DIR/README.md"

failures=0

require_file() {
  if [ ! -f "$1" ]; then
    printf 'missing: %s\n' "${1#"$ROOT_DIR"/}" >&2
    failures=$((failures + 1))
  fi
}

require_match() {
  file=$1
  pattern=$2
  label=$3

  if [ -f "$file" ] && ! rg -q -- "$pattern" "$file"; then
    printf 'missing: %s\n' "$label" >&2
    failures=$((failures + 1))
  fi
}

job_block() {
  job=$1
  awk -v job="$job" '
    $0 == "  " job ":" { in_job = 1; print; next }
    in_job && $0 ~ /^  [A-Za-z0-9_-]+:/ { exit }
    in_job { print }
  ' "$WORKFLOW"
}

has_v_tag_trigger() {
  awk '
    function indent(line,    first_non_space) {
      first_non_space = match(line, /[^[:space:]]/)
      return first_non_space ? first_non_space - 1 : 0
    }
    /^[[:space:]]*tags:[[:space:]]*/ {
      tags_indent = indent($0)
      scanning = 1
      if (index($0, "v*") > 0) {
        found = 1
      }
      next
    }
    scanning {
      if ($0 ~ /^[[:space:]]*$/) {
        next
      }
      if (indent($0) <= tags_indent) {
        scanning = 0
        next
      }
      if ($0 ~ /^[[:space:]]*-/ && index($0, "v*") > 0) {
        found = 1
      }
    }
    END { exit(found ? 0 : 1) }
  ' "$WORKFLOW"
}

require_file "$WORKFLOW"
require_file "$DEPLOYMENT_DOC"
require_file "$README"

if [ -f "$WORKFLOW" ] && ! has_v_tag_trigger; then
  printf '%s\n' 'missing: v* tag trigger' >&2
  failures=$((failures + 1))
fi
require_match "$WORKFLOW" 'release/\*\*' 'release/** branch trigger'
require_match "$WORKFLOW" 'actions/upload-artifact@v4' 'upload-artifact@v4'
require_match "$WORKFLOW" 'sha256sum' 'sha256sum checksum handling'
require_match "$WORKFLOW" '^  verify-linux:' 'verify-linux job'

if [ -f "$WORKFLOW" ]; then
  verify_job=$(job_block verify-linux)
  case "$verify_job" in
    *'needs:'*'build-linux'*) ;;
    *)
      printf '%s\n' 'missing: verify-linux needs build-linux' >&2
      failures=$((failures + 1))
      ;;
  esac

  release_job=$(job_block release)
  case "$release_job" in
    *'github.event_name == '\''push'\'''*'github.ref_type == '\''tag'\'''*'startsWith(github.ref_name, '\''v'\'')'*) ;;
    *)
      printf '%s\n' 'missing: tag-only Release condition' >&2
      failures=$((failures + 1))
      ;;
  esac

  case "$release_job" in
    *'softprops/action-gh-release@v2'*'files:'*) ;;
    *)
      printf '%s\n' 'missing: Release asset upload' >&2
      failures=$((failures + 1))
      ;;
  esac
fi

require_match "$README" 'docs/deployment/release-artifacts\.md' 'README deployment documentation entry'

if [ "$failures" -ne 0 ]; then
  printf 'check-release-workflow: %s contract(s) missing\n' "$failures" >&2
  exit 1
fi

printf '%s\n' 'check-release-workflow: all release contracts present'

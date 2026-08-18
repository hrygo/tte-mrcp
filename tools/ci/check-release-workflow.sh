#!/usr/bin/env bash

set -eu

ROOT_DIR="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  printf '%s\n' 'check-release-workflow: not inside a git repository' >&2
  exit 1
}

WORKFLOW="$ROOT_DIR/.github/workflows/build-linux.yml"
PACKAGE_VERIFY_SCRIPT="$ROOT_DIR/tools/ci/verify-linux-package.sh"
STRESS_SCRIPT="$ROOT_DIR/tools/stress/stress_test_improved.sh"
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
require_file "$PACKAGE_VERIFY_SCRIPT"
require_file "$STRESS_SCRIPT"
require_file "$DEPLOYMENT_DOC"
require_file "$README"

if [ -f "$WORKFLOW" ] && ! has_v_tag_trigger; then
  printf '%s\n' 'missing: v* tag trigger' >&2
  failures=$((failures + 1))
fi
require_match "$WORKFLOW" 'release/\*\*' 'release/** branch trigger'
require_match "$WORKFLOW" 'sha256sum' 'sha256sum checksum handling'
require_match "$WORKFLOW" 'tools/ci/verify-linux-package\.sh' 'in-job runtime verification'
require_match "$WORKFLOW" 'test_funasr_close_fence_retry' 'close-fence retry CMake test build target'
require_match "$PACKAGE_VERIFY_SCRIPT" 'mixed_server\.log' 'mixed server diagnostic log'
require_match "$PACKAGE_VERIFY_SCRIPT" 'mixed_tts_fixture\.log' 'mixed TTS fixture diagnostic log'
require_match "$PACKAGE_VERIFY_SCRIPT" 'mixed_asr_fixture\.log' 'mixed ASR fixture diagnostic log'
require_match "$STRESS_SCRIPT" 'Mixed failure diagnostics' 'mixed worker failure diagnostics'

if [ -f "$WORKFLOW" ]; then
  prune_job=$(job_block prune-artifacts)
  case "$prune_job" in
    *'actions: write'*'status=in_progress'*) ;;
    *)
      printf '%s\n' 'missing: active-run-safe artifact pruning permissions or filter' >&2
      failures=$((failures + 1))
      ;;
  esac

  case "$prune_job" in
    *'map(select(.name | startswith('*)
      printf '%s\n' 'artifact pruning must not be restricted to a name prefix' >&2
      failures=$((failures + 1))
      ;;
  esac

  case "$prune_job" in
    *'actions/artifacts/$artifact_id" \\'*'|| true'*)
      printf '%s\n' 'artifact deletion failures must not be ignored' >&2
      failures=$((failures + 1))
      ;;
  esac

  case "$prune_job" in
    *'continue-on-error: true'*)
      printf '%s\n' 'artifact pruning failures must fail the job' >&2
      failures=$((failures + 1))
      ;;
  esac

  build_job=$(job_block build-linux)
  case "$build_job" in
    *'tools/ci/verify-linux-package.sh'*'softprops/action-gh-release@v2'*) ;;
    *)
      printf '%s\n' 'missing: in-job Release asset upload' >&2
      failures=$((failures + 1))
      ;;
  esac
  case "$build_job" in
    *'github.ref_type == '*) ;;
    *)
      printf '%s\n' 'missing: tag-only Release condition' >&2
      failures=$((failures + 1))
      ;;
  esac

  if rg -q 'actions/(upload|download)-artifact@' "$WORKFLOW"; then
    printf '%s\n' 'workflow must not use Actions artifacts for package transfer' >&2
    failures=$((failures + 1))
  fi
fi

require_match "$README" 'docs/deployment/release-artifacts\.md' 'README deployment documentation entry'

if [ "$failures" -ne 0 ]; then
  printf 'check-release-workflow: %s contract(s) missing\n' "$failures" >&2
  exit 1
fi

printf '%s\n' 'check-release-workflow: all release contracts present'

#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

if [ "$(uname -s)" != "Darwin" ]; then
    echo "setup_macos.sh requires macOS" >&2
    exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required: https://brew.sh" >&2
    exit 1
fi

echo "Installing or verifying UniMRCP development dependencies..."
bundle_status=0
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ENV_HINTS=1 \
    brew bundle --file="$SCRIPT_DIR/Brewfile" --no-upgrade || bundle_status=$?

if [ "$bundle_status" -ne 0 ]; then
    echo "Homebrew could not install every formula; continuing with local checks." >&2
fi

# shellcheck disable=SC1091
. "$SCRIPT_DIR/env-macos.sh"

missing=0
for command_name in pkg-config cmake autoreconf automake expect; do
    if command -v "$command_name" >/dev/null 2>&1; then
        printf 'OK %-12s %s\n' "$command_name" "$(command -v "$command_name")"
    else
        printf 'MISSING %-8s\n' "$command_name"
        missing=1
    fi
done

if command -v glibtoolize >/dev/null 2>&1; then
    printf 'OK %-12s %s\n' "glibtoolize" "$(command -v glibtoolize)"
elif command -v libtoolize >/dev/null 2>&1; then
    printf 'OK %-12s %s\n' "libtoolize" "$(command -v libtoolize)"
else
    printf 'MISSING %-8s\n' "glibtoolize"
    missing=1
fi

for package_name in apr-1 apr-util-1 sofia-sip-ua; do
    if pkg-config --exists "$package_name"; then
        printf 'OK %-12s %s\n' "$package_name" "$(pkg-config --modversion "$package_name")"
    else
        printf 'MISSING %-8s pkg-config module\n' "$package_name"
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo "Development environment is incomplete; see tools/dev/README.md" >&2
    exit 1
fi

echo "Environment is ready for $ROOT_DIR"
echo "Next: . tools/dev/env-macos.sh && ./bootstrap && ./configure --prefix=\"$ROOT_DIR/build/install\""

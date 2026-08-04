#!/bin/sh
# Source this file from a shell before configuring or building UniMRCP.

if [ "$(uname -s)" != "Darwin" ]; then
    echo "env-macos.sh requires macOS" >&2
    return 1 2>/dev/null || exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required: https://brew.sh" >&2
    return 1 2>/dev/null || exit 1
fi

UNIMRCP_BREW_PREFIX=$(brew --prefix)
export UNIMRCP_BREW_PREFIX
export PATH="$UNIMRCP_BREW_PREFIX/bin:$UNIMRCP_BREW_PREFIX/sbin:$PATH"

PKG_CONFIG_DIRS=""
FORMULA_BIN_DIRS=""
for formula in apr apr-util sofia-sip; do
    formula_prefix=$(brew --prefix "$formula" 2>/dev/null || true)
    if [ -n "$formula_prefix" ] && [ -d "$formula_prefix/lib/pkgconfig" ]; then
        if [ -n "$PKG_CONFIG_DIRS" ]; then
            PKG_CONFIG_DIRS="$PKG_CONFIG_DIRS:$formula_prefix/lib/pkgconfig"
        else
            PKG_CONFIG_DIRS="$formula_prefix/lib/pkgconfig"
        fi
    fi
    if [ -n "$formula_prefix" ] && [ -d "$formula_prefix/bin" ]; then
        if [ -n "$FORMULA_BIN_DIRS" ]; then
            FORMULA_BIN_DIRS="$FORMULA_BIN_DIRS:$formula_prefix/bin"
        else
            FORMULA_BIN_DIRS="$formula_prefix/bin"
        fi
    fi
done

if [ -n "$FORMULA_BIN_DIRS" ]; then
    export PATH="$FORMULA_BIN_DIRS:$PATH"
fi

if [ -n "${PKG_CONFIG_PATH:-}" ] && [ -n "$PKG_CONFIG_DIRS" ]; then
    export PKG_CONFIG_PATH="$PKG_CONFIG_DIRS:$PKG_CONFIG_PATH"
elif [ -n "$PKG_CONFIG_DIRS" ]; then
    export PKG_CONFIG_PATH="$PKG_CONFIG_DIRS"
fi

if command -v pkg-config >/dev/null 2>&1; then
    BREW_CFLAGS=$(pkg-config --cflags apr-1 apr-util-1 sofia-sip-ua 2>/dev/null || true)
    BREW_LIBS=$(pkg-config --libs-only-L apr-1 apr-util-1 sofia-sip-ua 2>/dev/null || true)
    export CPPFLAGS="${BREW_CFLAGS}${CPPFLAGS:+ $CPPFLAGS}"
    export LDFLAGS="${BREW_LIBS}${LDFLAGS:+ $LDFLAGS}"
fi

export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"

echo "UniMRCP macOS environment loaded: $UNIMRCP_BREW_PREFIX"
echo "PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-<unset>}"

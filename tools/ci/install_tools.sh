#!/bin/sh
# install_tools.sh rustup|uv - the CI jobs' tool installers, pinned.
#
# The self-hosted runners have no package manager for these (no Homebrew on
# Intel Macs), so the first run of a job installs them into the runner
# user's home: rustup-init into ~/.cargo, uv into ~/.local/bin. Instead of
# piping a live install script into sh, a fixed release is downloaded and
# checked against the SHA-256 recorded here before it runs. An already
# installed tool is left alone.
#
# To move a pin: download the new release's file for both targets, check it
# against the project's published .sha256, and update the version and both
# hashes below together.
set -eu

RUSTUP_VERSION=1.29.1
RUSTUP_SHA256_x86_64_unknown_linux_gnu=dda7234360b7f578ca8b0ddcb80145646fa61a67c1720a5abc7051b35c9fcb71
RUSTUP_SHA256_x86_64_apple_darwin=259e2b84274434085163fe8d556510571772cda2aa6d87ca6aa664f57bc644e3
UV_VERSION=0.12.18
UV_SHA256_x86_64_unknown_linux_gnu=89eadd7c76fc063887959510d5ba0ab1264dfd5f1143b925ddb73021a40acf16
UV_SHA256_x86_64_apple_darwin=2e4108f5395397c8bc5d43bf83d3bdbb2d0e92b90d0efa607756be704905fa33

case "$(uname -s)-$(uname -m)" in
Linux-x86_64)  target=x86_64-unknown-linux-gnu ;;
Darwin-x86_64) target=x86_64-apple-darwin ;;
*) echo "install_tools: no pinned build for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
key=$(echo "$target" | tr '-' '_')

sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# fetch URL FILE SHA256: download and refuse anything else
fetch() {
    curl -sSfL --retry 3 -o "$2" "$1"
    got=$(sha256 "$2")
    if [ "$got" != "$3" ]; then
        echo "install_tools: $1: SHA-256 $got, expected $3" >&2
        rm -f "$2"
        exit 1
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case "${1:-}" in
rustup)
    if command -v rustup >/dev/null || [ -x "$HOME/.cargo/bin/rustup" ]; then
        exit 0
    fi
    eval "sum=\$RUSTUP_SHA256_$key"
    fetch "https://static.rust-lang.org/rustup/archive/$RUSTUP_VERSION/$target/rustup-init" \
          "$tmp/rustup-init" "$sum"
    chmod +x "$tmp/rustup-init"
    "$tmp/rustup-init" -y --profile minimal --no-modify-path
    ;;
uv)
    if command -v uv >/dev/null || [ -x "$HOME/.local/bin/uv" ]; then
        exit 0
    fi
    eval "sum=\$UV_SHA256_$key"
    fetch "https://github.com/astral-sh/uv/releases/download/$UV_VERSION/uv-$target.tar.gz" \
          "$tmp/uv.tar.gz" "$sum"
    tar -xzf "$tmp/uv.tar.gz" -C "$tmp"
    mkdir -p "$HOME/.local/bin"
    cp "$tmp/uv-$target/uv" "$tmp/uv-$target/uvx" "$HOME/.local/bin/"
    ;;
*)
    echo "usage: install_tools.sh rustup|uv" >&2
    exit 2
    ;;
esac

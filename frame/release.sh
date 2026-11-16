#!/usr/bin/env bash
# Builds the KartFrame release tarball from parts that already exist. It does not build the kit.
#
#   release.sh --kit DIR [--toolchain DIR] [--translator DIR] [--out DIR]
#
#   --kit DIR         the kit made by frame/make-kit.sh (holds kit.json)
#   --toolchain DIR   a folder holding toolchain-aarch64/ (frame/toolchain.sh <dir> makes it) or
#                     toolchain-aarch64.tar.xz; default $KARTFRAME_CACHE/toolchain
#   --translator DIR  the folder frame/publish-translator.sh wrote (Translator.Cli); default
#                     $KARTFRAME_CACHE/translator
#   --out DIR         where the tarball and its sha256 go (default <repo>/release)
#
# The tarball has no top level folder: kit/, toolchain/, translator/, setup.sh, kartframe.sh,
# install.sh, uninstall.sh, collect-logs.sh, steam-cef.py, art/ (art.py, logo, icon, set/), frame-controller-models.py,
# Config.frame.toml, tools/{vrtiming.py,session-report.py,crash-report.py}, VERSION, README.md, docs/, LICENSE,
# THIRD-PARTY-NOTICES.md, licenses/, and tools/sentry-dsn when there is a Sentry DSN (frame/sentry-dsn.sh:
# KARTFRAME_SENTRY_DSN or ~/.config/kartframe-dev/sentry-dsn; without one the release has no Send Report).
# frame/install.sh unpacks it into the program folder.
# Then frame/boundary-check.sh and frame/tools/privacy-check.sh run on the tarball itself, and the
# sha256 is written beside it. Any failure removes the tarball again. Nothing from the game's disc
# is read here. Environment: KARTFRAME_CACHE (default ~/.cache/kartframe).
# With SENTRY_AUTH_TOKEN set (and SENTRY_ORG, SENTRY_PROJECT), frame/upload-symbols.sh then uploads the
# symbols of KARTFRAME_SYMBOLS (default $KARTFRAME_HOME/bin/kartframe, the game setup.sh built here),
# when that binary was built against the kit being released.
set -euo pipefail

VERSION=1.0.1
NAME=kartframe-$VERSION-linux-arm64
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
cache=${KARTFRAME_CACHE:-$HOME/.cache/kartframe}
kit= tc=$cache/toolchain tr=$cache/translator out=$repo/release

fail() { echo "release.sh: error: $*" >&2; exit 1; }
while [[ $# -gt 0 ]]; do
    case $1 in
        --kit) kit=${2:?}; shift ;;
        --toolchain) tc=${2:?}; shift ;;
        --translator) tr=${2:?}; shift ;;
        --out) out=${2:?}; shift ;;
        -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument $1" ;;
    esac
    shift
done
[[ -n $kit ]] || fail "--kit DIR is required (frame/make-kit.sh makes it)"
[[ -f $kit/kit.json && -f $kit/kit.sha256 ]] || fail "$kit is not a kit (kit.json or kit.sha256 missing)"
[[ -x $tr/Translator.Cli ]] || fail "no executable Translator.Cli in $tr (frame/publish-translator.sh)"
[[ -x $tc/toolchain-aarch64/bin/clang || -f $tc/toolchain-aarch64.tar.xz ]] ||
    fail "no toolchain-aarch64/ or toolchain-aarch64.tar.xz in $tc (frame/toolchain.sh <dir>)"
for f in README.md LICENSE THIRD-PARTY-NOTICES.md docs/RELEASE_NOTES_1.0.0.md docs/CHANGELOG.md docs/TECHNICAL_NOTES.md \
         frame/setup.sh frame/kartframe.sh frame/install.sh frame/uninstall.sh frame/collect-logs.sh frame/steam-cef.py \
         frame/frame-controller-models.py frame/Config.frame.toml frame/art/art.py frame/art/logo.png frame/art/icon-512.png frame/art/CREDITS.md frame/art/set/p.png frame/art/set/wide.png frame/art/set/hero.png frame/tools/vrtiming.py frame/tools/session-report.py frame/tools/crash-report.py frame/sentry-dsn.sh \
         frame/boundary-check.sh frame/release-allowlist.txt frame/tools/privacy-check.sh; do
    [[ -f $repo/$f ]] || fail "missing $f"
done

mkdir -p "$out"
out=$(cd "$out" && pwd)
stage=$(mktemp -d "${TMPDIR:-$HOME/.cache}/kf-release.XXXXXX")
trap 'rm -rf "$stage"' EXIT
tarball=$out/$NAME.tar.gz
rm -f "$tarball" "$tarball.sha256"
cleanup_on_fail() { rm -f "$tarball" "$tarball.sha256"; }
trap 'cleanup_on_fail; rm -rf "$stage"' ERR

echo "release: staging"
cp -a "$kit" "$stage/kit"
mkdir -p "$stage/toolchain" "$stage/translator" "$stage/tools" "$stage/docs" "$stage/art/set"
if [[ -x $tc/toolchain-aarch64/bin/clang ]]; then
    cp -a "$tc/toolchain-aarch64/." "$stage/toolchain/"
else
    tar -xJf "$tc/toolchain-aarch64.tar.xz" -C "$stage/toolchain" --strip-components=1
fi
[[ -x $stage/toolchain/bin/clang ]] || fail "the toolchain has no bin/clang"
install -m 0755 "$tr/Translator.Cli" "$stage/translator/Translator.Cli"
(cd "$stage/translator" && sha256sum Translator.Cli > Translator.Cli.sha256)
for f in setup.sh kartframe.sh install.sh uninstall.sh collect-logs.sh steam-cef.py; do
    install -m 0755 "$here/$f" "$stage/$f"
done
install -m 0644 "$here/frame-controller-models.py" "$here/Config.frame.toml" "$stage/"
install -m 0755 "$here/art/art.py" "$stage/art/art.py"
install -m 0644 "$here/art/logo.png" "$here/art/icon-512.png" "$here/art/CREDITS.md" "$stage/art/"
install -m 0644 "$here"/art/set/*.png "$stage/art/set/"
install -m 0644 "$here/tools/vrtiming.py" "$here/tools/session-report.py" "$here/tools/crash-report.py" "$stage/tools/"
printf '%s\n' "$VERSION" > "$stage/VERSION"
. "$here/sentry-dsn.sh"
[[ -z $KARTFRAME_SENTRY_DSN ]] || printf '%s\n' "$KARTFRAME_SENTRY_DSN" > "$stage/tools/sentry-dsn"
install -m 0644 "$repo/README.md" "$repo/LICENSE" "$repo/THIRD-PARTY-NOTICES.md" "$stage/"
install -m 0644 "$repo/docs/RELEASE_NOTES_1.0.0.md" "$repo/docs/CHANGELOG.md" "$repo/docs/TECHNICAL_NOTES.md" "$stage/docs/"
cp -a "$repo/licenses" "$stage/licenses"

# Tools that unpack the tarball safely (Frametanium) refuse absolute symlinks. A kit made before
# make-sysroot.sh copied the licence files by content holds one (sysroot/LICENSES/RUNTIME.LIBRARY.EXCEPTION
# -> /usr/share/licenses/gcc-libs/...). Replace each with a copy of the file it points to. kit.sha256 and
# kit.json are not touched: the kit's fingerprint is a hash of those two files, and verify_kit skips the
# symlink lines of the manifest, so the kit keeps its fingerprint and nobody has to redo the disc setup.
while IFS= read -r -d '' l; do
    [[ -f $l ]] || fail "absolute symlink in the release that does not point to a file here: ${l#"$stage"/} -> $(readlink "$l")"
    t=$(mktemp "$stage/.link.XXXXXX"); cp -L "$l" "$t"; chmod 0644 "$t"
    mv -f "$t" "$l"
done < <(find "$stage" -type l -lname '/*' -print0)
bad=$(find "$stage" -type l -lname '/*' -printf '%P -> %l\n' | head -n 3)
[[ -z $bad ]] || fail "absolute symlinks in the release: $bad"

echo "release: packing"
# Fixed order, owner and times, so the same inputs make the same tarball.
(cd "$stage" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort |
    tar --no-recursion --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00Z' -czf "$tarball" -T -)

echo "release: boundary check"
"$here/boundary-check.sh" "$tarball" "$here/release-allowlist.txt"
echo "release: privacy check"
"$repo/frame/tools/privacy-check.sh" "$tarball"

(cd "$out" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
trap 'rm -rf "$stage"' EXIT; trap - ERR
echo "release: $tarball ($(du -h "$tarball" | cut -f1))"
cat "$tarball.sha256"

# Crash report symbols: only with a token, and only for a game built against this kit.
if [[ -n ${SENTRY_AUTH_TOKEN:-} ]]; then
    kfhome=${KARTFRAME_HOME:-$HOME/.local/share/KartFrame}
    symbols=${KARTFRAME_SYMBOLS:-$kfhome/bin/kartframe}
    kitfp=$(sed -n 's/.*"fingerprint"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$kit/kit.json" | head -n 1 || true)
    builtfp=$(head -n 1 "$kfhome/bin/kartframe.fingerprint" 2> /dev/null | tr -d '[:space:]' || true)
    if [[ -n ${KARTFRAME_SYMBOLS:-} || ( -n $kitfp && $kitfp == "$builtfp" ) ]]; then
        "$here/upload-symbols.sh" "$symbols" || echo "release: warning: the symbol upload failed (the release is fine; run frame/upload-symbols.sh again)" >&2
    else
        echo "release: symbols not uploaded: $symbols was not built against this kit; run frame/setup.sh with it, then frame/upload-symbols.sh $symbols" >&2
    fi
fi

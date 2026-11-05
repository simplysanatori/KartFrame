#!/usr/bin/env bash
# Maintainer step: build the minimal compile/link sysroot that goes into the kit, from the files
# of this machine's own packages (glibc headers and crt objects, libstdc++ headers, libgcc and the
# crtbegin/crtend objects, the kernel headers glibc includes). The portable LLVM has no sysroot of
# its own; without this the first-run build would compile against whatever /usr/include a user's
# machine happens to have.
#
#   make-sysroot.sh <destdir>      creates <destdir> (replaced if present), prints its size
#
# Layout: <dest>/usr/include, <dest>/usr/lib (+ gcc/<triple>/<ver>), <dest>/lib -> usr/lib, and
# <dest>/LICENSES with the origin packages and licence texts. Use it as `--sysroot=<dest>`.
set -euo pipefail

[[ $# -eq 1 ]] || { echo "usage: make-sysroot.sh <destdir>" >&2; exit 2; }
dest=$1
command -v pacman >/dev/null || { echo "make-sysroot.sh needs pacman (it copies this Frame's packages)" >&2; exit 1; }
rm -rf "$dest"; mkdir -p "$dest/usr/include" "$dest/usr/lib" "$dest/LICENSES"
dest=$(cd "$dest" && pwd)
ln -s usr/lib "$dest/lib"

# Every regular file or symlink the package owns under the given prefix, copied with its path.
copy_pkg() { # <pkg> <regex on the absolute path>
    pacman -Ql "$1" | awk '{print $2}' | grep -E "$2" | grep -v '/$' | while IFS= read -r f; do
        [[ -f $f || -L $f ]] && cp -a --parents "$f" "$dest"
    done
}

gccver=$(pacman -Q gcc | awk '{print $2}' | sed 's/+.*//')
triple=aarch64-unknown-linux-gnu
copy_pkg glibc '^/usr/include/'
copy_pkg linux-api-headers '^/usr/include/'
copy_pkg gcc "^/usr/include/c\\+\\+/$gccver/"
copy_pkg glibc '^/usr/lib/(crt1|crti|crtn|Scrt1)\.o$'
copy_pkg glibc '^/usr/lib/(libc\.so|libc\.so\.6|libc_nonshared\.a|libm\.so|libm\.so\.6|libmvec\.so|libmvec\.so\.1|ld-linux-aarch64\.so\.1|libdl\.a|libdl\.so\.2|librt\.a|librt\.so\.1|libpthread\.a|libpthread\.so\.0|libutil\.a|libutil\.so\.1)$'
copy_pkg gcc-libs '^/usr/lib/(libstdc\+\+\.so[.0-9]*|libgcc_s\.so(\.1)?)$'
copy_pkg gcc "^/usr/lib/gcc/$triple/$gccver/(crtbegin|crtbeginS|crtbeginT|crtend|crtendS|libgcc|libgcc_eh)\\.(o|a)$"
[[ -f $dest/usr/lib/gcc/$triple/$gccver/crtbeginS.o ]] || { echo "gcc crt objects missing" >&2; exit 1; }
[[ -f $dest/usr/include/c++/$gccver/vector && -f $dest/usr/include/stdio.h && -f $dest/usr/include/linux/types.h ]] \
    || { echo "headers missing" >&2; exit 1; }

# Provenance and licences.
{
    echo "Minimal sysroot for building KartFrame's translated game, copied from these packages of the"
    echo "SteamOS image it was made on (files only; nothing was modified):"
    echo
    for p in glibc gcc gcc-libs linux-api-headers; do
        pacman -Qi "$p" | awk -F' *: *' -v p="$p" '/^Name/{n=$2} /^Version/{v=$2} /^Licenses/{l=$2} END{printf "  %-20s %-40s %s\n", n, v, l}'
    done
    echo
    echo "glibc: LGPL-2.1-or-later (GPL-2.0-or-later for some files), https://www.gnu.org/software/libc/"
    echo "gcc/libstdc++/libgcc: GPL-3.0-or-later with the GCC Runtime Library Exception, https://gcc.gnu.org/"
    echo "linux-api-headers: GPL-2.0 with the Linux syscall note, https://www.kernel.org/"
    echo "Object files linked into the game (crt*.o, libgcc, libc_nonshared) fall under the exceptions above."
    echo "Source for all of them is available from the upstream projects named here."
} > "$dest/LICENSES/README.txt"
for d in /usr/share/licenses/gcc /usr/share/licenses/gcc-libs; do
    # -L: the packages link some licence files by absolute path (/usr/share/licenses/common/...); an
    # absolute link in the release tarball is refused by tools that unpack it safely (Frametanium).
    [[ -d $d ]] && cp -aL "$d/." "$dest/LICENSES/" 2>/dev/null || true
done
for l in GPL2 GPL3 LGPL2.1 FDL1.3; do
    [[ -f /usr/share/licenses/common/$l/license.txt ]] && cp -a "/usr/share/licenses/common/$l/license.txt" "$dest/LICENSES/$l.txt" || true
done
echo "sysroot: $dest ($(du -sh "$dest" | cut -f1), $(find "$dest" -type f | wc -l) files)"

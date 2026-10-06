#!/usr/bin/env bash
#
# Generate NTFS test images and their manifests under tests/fixtures/.
#
# Needs mkfs.ntfs (package: ntfs-3g) and a way to mount the images:
# an unprivileged ntfs-3g FUSE mount is tried first, then sudo loop mount.
# Images are regenerable and never committed; the manifests record what
# each one must contain (paths + SHA-256), and tests/run.sh replays them
# against the built binary.
#
# Fixture file/dir names must not contain spaces (see run.sh's parser).
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p fixtures

command -v mkfs.ntfs >/dev/null || {
    echo "mkimages.sh: mkfs.ntfs not found (install ntfs-3g)" >&2
    exit 1
}

MNT=$(mktemp -d)
MOUNT_KIND=""
trap 'do_umount; rmdir "$MNT"' EXIT

do_mount() { # img
    # streams_interface=windows lets us create "file:stream" ADS paths
    if ntfs-3g "$1" "$MNT" -o streams_interface=windows 2>/dev/null; then
        MOUNT_KIND=fuse
    else
        sudo mount -o loop "$1" "$MNT"
        MOUNT_KIND=sudo
    fi
}

do_umount() {
    [ -n "$MOUNT_KIND" ] || return 0
    sync
    case "$MOUNT_KIND" in
    fuse) fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" ;;
    sudo) sudo umount "$MNT" ;;
    esac
    MOUNT_KIND=""
}

make_fs() { # name size_mb
    rm -f "fixtures/$1.img"
    truncate -s "${2}M" "fixtures/$1.img"
    mkfs.ntfs -F -q "fixtures/$1.img"
}

# Record every directory and file currently on $MNT into a manifest.
write_manifest() { # name
    local out="fixtures/$1.manifest"
    : >"$out"
    (cd "$MNT" && find . -mindepth 1 -type d | sort) |
        sed 's|^\.|D |' >>"$out"
    (cd "$MNT" && find . -type f | sort) | while read -r f; do
        printf 'F %s /%s\n' \
            "$(sha256sum "$MNT/${f#./}" | cut -d' ' -f1)" "${f#./}"
    done >>"$out"
}

# --- basic.img: the resident/non-resident split and path resolution -------
# tiny.txt fits in its MFT record ($DATA stays resident); medium.bin is
# forced non-resident; the nested directories exercise multi-component
# path lookups while every index still fits in $INDEX_ROOT.
make_fs basic 16
do_mount fixtures/basic.img
head -c 200 /dev/urandom >"$MNT/tiny.txt"
head -c $((1 << 20)) /dev/urandom >"$MNT/medium.bin"
mkdir -p "$MNT/docs/reports"
head -c 5000 /dev/urandom >"$MNT/docs/reports/deep.txt"
head -c 300 /dev/urandom >"$MNT/docs/readme.txt"
# alternate data stream (streams_interface=windows exposes "file:stream"
# paths, so the stream can be hashed into the manifest like any file)
ADS_OK=0
if [ "$MOUNT_KIND" = fuse ]; then
    echo "default stream" >"$MNT/ads.txt"
    echo "hidden stream" >"$MNT/ads.txt:extra" 2>/dev/null && ADS_OK=1
fi
write_manifest basic
if [ "$ADS_OK" = 1 ]; then
    printf 'F %s /ads.txt:extra\n' \
        "$(sha256sum "$MNT/ads.txt:extra" | cut -d' ' -f1)" \
        >>fixtures/basic.manifest
fi
do_umount

# --- bigdir.img: a directory too big for its MFT record -------------------
# ~800 entries push the $I30 index out of $INDEX_ROOT into external
# $INDEX_ALLOCATION blocks, giving the B+ tree real intermediate nodes.
make_fs bigdir 64
do_mount fixtures/bigdir.img
mkdir "$MNT/big"
for i in $(seq -w 1 800); do
    echo "content of file $i" >"$MNT/big/file-$i.txt"
done
write_manifest bigdir
do_umount

# --- frag.img: a file whose runlist has many runs --------------------------
# Fill the volume with pad files, delete every other one, then write a
# large file into the holes: the allocator has to scatter it, so its
# runlist gets several runs with negative (backward) LCN deltas.
make_fs frag 32
do_mount fixtures/frag.img
for i in $(seq -w 1 24); do
    head -c $((1 << 20)) /dev/zero >"$MNT/pad-$i.bin"
done
sync
for i in $(seq -w 1 24 | awk 'NR % 2'); do
    rm "$MNT/pad-$i.bin"
done
sync
head -c $((8 << 20)) /dev/urandom >"$MNT/scattered.bin"
write_manifest frag
do_umount

echo "fixtures written to $(pwd)/fixtures"

#!/usr/bin/env bash
# RECIPE / 待真机验证.
# Default is dry-run: print the commands and exit 0. No mkfs, no losetup.
# On a test machine, as root:
#   KN_DEMO_RUN=1 ./01-run.sh
# Optional filesystem (default ext4):
#   KN_DEMO_RUN=1 KN_DEMO_FS=xfs ./01-run.sh
# The script creates its own image under mktemp and loops only that file.
# It never accepts a caller-supplied block device.

set -euo pipefail

if [[ -n "${KN_DEMO_DEV:-}" ]]; then
	echo "refusing KN_DEMO_DEV=${KN_DEMO_DEV}" >&2
	echo "this script only loops an image it creates itself" >&2
	exit 1
fi

fs="${KN_DEMO_FS:-ext4}"
case "${fs}" in
ext4|xfs) ;;
*)
	echo "KN_DEMO_FS must be ext4 or xfs" >&2
	exit 1
	;;
esac

echo "=== meta ==="
date '+%Y-%m-%d %H:%M:%S %Z' || true
uname -a || true
echo "kver=$(uname -r)"
echo "fs=${fs}"
echo "image=512MiB regular file under mktemp"
echo "status 待真机验证 / RECIPE"
echo "filefrag -v without -s (FIEMAP_FLAG_SYNC would write back first)"

if [[ ${KN_DEMO_RUN:-0} != 1 ]]; then
	echo
	echo "DRY-RUN. No truncate, no mkfs, no losetup, no mount."
	echo "On a test machine: KN_DEMO_RUN=1 $0"
	echo
	echo "would: work=\$(mktemp -d /tmp/kn-ofs-XXXXXX)"
	echo "would: truncate -s 512M \"\$work/disk.img\""
	if [[ ${fs} == ext4 ]]; then
		echo "would: mkfs.ext4 -F -q \"\$work/disk.img\""
	else
		echo "would: mkfs.xfs -f \"\$work/disk.img\""
	fi
	echo "would: loop=\$(losetup -f --show -- \"\$work/disk.img\")"
	echo "would: require /dev/loop* and backing_file == that image"
	echo "would: mount \"\$loop\" \"\$work/mnt\""
	echo "would: dd if=/dev/zero of=\"\$work/mnt/delalloc.bin\" bs=1M count=4 status=none"
	echo "would: filefrag -v \"\$work/mnt/delalloc.bin\"    # before fsync"
	echo "would: python3 os.fsync on delalloc.bin"
	echo "would: filefrag -v \"\$work/mnt/delalloc.bin\"    # after fsync"
	echo "would: fallocate -l 4M \"\$work/mnt/unwritten.bin\""
	echo "would: filefrag -v \"\$work/mnt/unwritten.bin\""
	echo "would: trap: umount, losetup -d only if backing_file matches, rm -rf work"
	exit 0
fi

if [[ "$(id -u)" -ne 0 ]]; then
	echo "KN_DEMO_RUN=1 needs root" >&2
	exit 1
fi

need=()
for cmd in truncate losetup mount umount filefrag fallocate python3 dd; do
	if ! command -v "${cmd}" >/dev/null 2>&1; then
		need+=("${cmd}")
	fi
done
if [[ ${fs} == ext4 ]]; then
	command -v mkfs.ext4 >/dev/null 2>&1 || need+=(mkfs.ext4)
else
	command -v mkfs.xfs >/dev/null 2>&1 || need+=(mkfs.xfs)
fi
if ((${#need[@]})); then
	echo "missing: ${need[*]}" >&2
	exit 1
fi

work="$(mktemp -d /tmp/kn-ofs-XXXXXX)"
img="${work}/disk.img"
mnt="${work}/mnt"
loop=""

backing_of() {
	local devbase
	devbase="${1#/dev/}"
	cat "/sys/block/${devbase}/loop/backing_file"
}

cleanup() {
	set +e
	if [[ -n ${mnt} && -d ${mnt} ]]; then
		umount "${mnt}" 2>/dev/null
	fi
	if [[ -n ${loop} ]]; then
		local back
		back="$(backing_of "${loop}" 2>/dev/null || true)"
		if [[ ${back} == "${img}" ]]; then
			losetup -d "${loop}"
		else
			echo "refusing to detach ${loop} (backing '${back}' != '${img}')" >&2
		fi
	fi
	if [[ -n ${work} && ${work} == /tmp/kn-ofs-* ]]; then
		rm -rf "${work}"
	fi
}
trap cleanup EXIT

mkdir -p "${mnt}"
truncate -s 512M "${img}"
if [[ -L ${img} || ! -f ${img} ]]; then
	echo "image is not a regular file: ${img}" >&2
	exit 1
fi

if [[ ${fs} == ext4 ]]; then
	mkfs.ext4 -F -q -- "${img}"
else
	mkfs.xfs -f -- "${img}"
fi

loop="$(losetup -f --show -- "${img}")"
case "${loop}" in
/dev/loop*) ;;
*)
	echo "refusing unexpected device name: ${loop}" >&2
	exit 1
	;;
esac

back="$(backing_of "${loop}")"
if [[ ${back} != "${img}" ]]; then
	echo "backing file mismatch: ${back} != ${img}" >&2
	exit 1
fi
echo "loop=${loop}"
echo "backing=${back}"

mount -- "${loop}" "${mnt}"

dd if=/dev/zero of="${mnt}/delalloc.bin" bs=1M count=4 status=none
echo "=== filefrag before fsync ==="
filefrag -v "${mnt}/delalloc.bin"

python3 - "${mnt}/delalloc.bin" <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_RDWR)
os.fsync(fd)
os.close(fd)
PY

echo "=== filefrag after fsync ==="
filefrag -v "${mnt}/delalloc.bin"

fallocate -l 4M "${mnt}/unwritten.bin"
echo "=== filefrag after fallocate ==="
filefrag -v "${mnt}/unwritten.bin"

echo "ACCEPTANCE: paste this stdout as sampled on $(uname -r). Status was 待真机验证."
echo "Do not treat a missing delalloc flag before fsync as failure (writeback may have run)."
echo "The fallocate file is expected to show an unwritten flag; if it does not, keep RECIPE."

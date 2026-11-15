#!/usr/bin/env bash
# Read-only inventory of DMA/IOMMU clues on THIS machine.
# No sudo, no sysfs writes, no module loads, no reboot, no fault injection.
# Save output only by redirecting stdout yourself:
#   ./01-run.sh > inventory.txt
# A successful run is not hardware DMA verification.

set -u

section() { printf '\n=== %s ===\n' "$1"; }
say_skip() { printf 'SKIP: %s\n' "$1"; }
say_unknown() { printf 'UNKNOWN: %s\n' "$1"; }

section meta
date '+%Y-%m-%d %H:%M:%S %Z' || true
uname -a || true
if kver="$(uname -r 2>/dev/null)"; then
	printf 'sampled on %s\n' "${kver}"
else
	say_unknown "uname -r failed"
fi
printf 'uid=%s\n' "$(id -u 2>/dev/null || echo UNKNOWN)"
echo "status 待真机验证"
echo "scope read-only inventory; script does not change boot params, drivers, or groups"
echo "limitation: iommu group presence, the type file, and dmesg lines do not identify dma_map_ops, the live domain, or whether ACS isolation is sufficient"
echo "limitation: a virtual machine sample is not hardware DMA verification"
echo "note: /sys/class/dma is the dmaengine class and is not read"

section cmdline
if [[ -r /proc/cmdline ]]; then
	printf 'cmdline='
	cat /proc/cmdline
	printf '\n'
else
	say_skip "/proc/cmdline is missing or unreadable"
fi

section pci
pci_root=/sys/bus/pci/devices
if [[ ! -d ${pci_root} ]]; then
	say_skip "${pci_root} is missing"
else
	shopt -s nullglob
	devs=("${pci_root}"/*)
	shopt -u nullglob
	if ((${#devs[@]} == 0)); then
		say_unknown "${pci_root} exists but has no device entries"
	else
		printf 'pci_count=%s\n' "${#devs[@]}"
		shown=0
		for dev in "${devs[@]}"; do
			if ((shown >= 512)); then
				printf 'SKIP: remaining PCI devices not printed (cap 512 of %s)\n' "${#devs[@]}"
				break
			fi
			shown=$((shown + 1))
			bdf="${dev##*/}"
			if [[ -r ${dev}/vendor ]]; then
				vendor="$(cat "${dev}/vendor" 2>/dev/null || echo UNKNOWN)"
			else
				vendor="SKIP-unreadable"
			fi
			if [[ -r ${dev}/device ]]; then
				device="$(cat "${dev}/device" 2>/dev/null || echo UNKNOWN)"
			else
				device="SKIP-unreadable"
			fi
			if [[ -L ${dev}/driver ]]; then
				drv="$(readlink "${dev}/driver" 2>/dev/null || echo UNKNOWN)"
			elif [[ -e ${dev}/driver ]]; then
				drv="UNKNOWN-not-a-symlink"
			else
				drv="NONE"
			fi
			if [[ -L ${dev}/iommu_group ]]; then
				grp="$(readlink "${dev}/iommu_group" 2>/dev/null || echo UNKNOWN)"
			elif [[ -e ${dev}/iommu_group ]]; then
				grp="UNKNOWN-not-a-symlink"
			else
				grp="NONE"
			fi
			printf 'bdf=%s vendor=%s device=%s driver=%s iommu_group=%s\n' \
				"${bdf}" "${vendor}" "${device}" "${drv}" "${grp}"
		done
	fi
fi

section iommu_groups
groot=/sys/kernel/iommu_groups
echo "note: type is default_domain type (identity, DMA, DMA-FQ, unmanaged, blocked, unknown), not the attached domain and not dma_map_ops"
if [[ ! -d ${groot} ]]; then
	say_skip "${groot} is missing"
else
	shopt -s nullglob
	groups=("${groot}"/*)
	shopt -u nullglob
	if ((${#groups[@]} == 0)); then
		say_unknown "${groot} exists but contains no group"
	else
		printf 'group_count=%s\n' "${#groups[@]}"
		for g in "${groups[@]}"; do
			[[ -d ${g} ]] || continue
			gid="${g##*/}"
			printf -- '-- group %s\n' "${gid}"
			typef="${g}/type"
			if [[ -r ${typef} ]]; then
				IFS= read -r tline < "${typef}" || tline="UNKNOWN"
				printf 'type=%s\n' "${tline}"
			else
				say_skip "group ${gid} type is missing or unreadable"
			fi
			devdir="${g}/devices"
			if [[ ! -d ${devdir} ]]; then
				say_skip "group ${gid} has no devices directory"
				continue
			fi
			shopt -s nullglob
			links=("${devdir}"/*)
			shopt -u nullglob
			if ((${#links[@]} == 0)); then
				say_unknown "group ${gid} devices directory is empty"
				continue
			fi
			for link in "${links[@]}"; do
				name="${link##*/}"
				if [[ -L ${link} ]]; then
					target="$(readlink "${link}" 2>/dev/null || echo UNKNOWN)"
					printf 'device=%s readlink=%s\n' "${name}" "${target}"
				else
					printf 'device=%s readlink=SKIP-not-a-symlink\n' "${name}"
				fi
			done
		done
	fi
fi

section iommu_class
croot=/sys/class/iommu
echo "note: /sys/class/iommu names IOMMU device instances; it does not mean every PCI device uses a translated domain"
if [[ ! -d ${croot} ]]; then
	say_skip "${croot} is missing"
else
	shopt -s nullglob
	nodes=("${croot}"/*)
	shopt -u nullglob
	if ((${#nodes[@]} == 0)); then
		say_unknown "${croot} exists but is empty"
	else
		for n in "${nodes[@]}"; do
			name="${n##*/}"
			if [[ -L ${n} ]]; then
				printf 'iommu=%s readlink=%s\n' "${name}" "$(readlink "${n}" 2>/dev/null || echo UNKNOWN)"
			else
				printf 'iommu=%s readlink=SKIP-not-a-symlink\n' "${name}"
			fi
		done
	fi
fi

section dmesg
if ! command -v dmesg >/dev/null 2>&1; then
	say_skip "dmesg command is missing"
else
	rc=0
	out="$(dmesg 2>&1)" || rc=$?
	if ((rc != 0)); then
		if grep -qiE 'permission|not permitted|operation not permitted' <<<"${out}"; then
			say_skip "dmesg denied (${out})"
		else
			say_unknown "dmesg failed rc=${rc} (${out})"
		fi
	else
		matches="$(grep -E 'DMAR|IOMMU|SWIOTLB|software IO TLB' <<<"${out}" || true)"
		if [[ -z ${matches} ]]; then
			say_unknown "dmesg is readable and has no DMAR/IOMMU/SWIOTLB lines; absence is not evidence that the machine has no IOMMU"
		else
			count="$(printf '%s\n' "${matches}" | wc -l | tr -d ' ')"
			printf 'match_lines=%s\n' "${count}"
			if [[ ${count} =~ ^[0-9]+$ ]] && ((count > 80)); then
				printf 'showing first 80 of %s\n' "${count}"
				printf '%s\n' "${matches}" | head -n 80
			else
				printf '%s\n' "${matches}"
			fi
		fi
	fi
fi

section done
echo "result=ok"
echo "this run does not prove DMA translation, bounce-buffer use, or hardware isolation"
exit 0

#!/usr/bin/env bash
# Reconcile root memory.stat against /proc/meminfo.
# Both are read at nearly the same instant; kv = memory.stat value / 1024 (kB).
set -uo pipefail

ROOT=/sys/fs/cgroup
declare -A MS MI
while read -r k v; do MS[$k]=$v; done < "$ROOT/memory.stat"
while read -r k v _; do MI[${k%:}]=$v; done < /proc/meminfo
kv() { echo $(( ${MS[$1]} / 1024 )); }

row() { printf '%-22s %-16s %10s %10s %8s\n' "$1" "$2" "$3" "$4" "$(( $4 - $3 ))"; }
row_hdr() { printf '%-22s %-16s %10s %10s %8s\n' memory.stat meminfo memcg_kB meminfo_kB diff; }

echo "=== user memory: expected to match ==="
row_hdr
row file               "Cached+Buffers" "$(kv file)"  $(( MI[Cached] + MI[Buffers] ))
row shmem              Shmem            "$(kv shmem)" "${MI[Shmem]}"
row anon               AnonPages        "$(kv anon)"  "${MI[AnonPages]}"
row active_file        Active\(file\)   "$(kv active_file)"   "${MI[Active(file)]}"
row inactive_anon      Inactive\(anon\) "$(kv inactive_anon)" "${MI[Inactive(anon)]}"
row file_mapped        Mapped           "$(kv file_mapped)"   "${MI[Mapped]}"
row unevictable        Unevictable      "$(kv unevictable)"   "${MI[Unevictable]}"

echo
echo "=== kernel memory: expected NOT to match ==="
row_hdr
row slab_reclaimable   SReclaimable     "$(kv slab_reclaimable)"   "${MI[SReclaimable]}"
row slab_unreclaimable SUnreclaim       "$(kv slab_unreclaimable)" "${MI[SUnreclaim]}"
row vmalloc            VmallocUsed      "$(kv vmalloc)"  "${MI[VmallocUsed]}"
row kernel_stack       KernelStack      "$(kv kernel_stack)" "${MI[KernelStack]}"
row percpu             Percpu           "$(kv percpu)"   "${MI[Percpu]}"

echo
echo "=== totals ==="
used=$(( MI[MemTotal] - MI[MemFree] ))
memcg=$(( $(kv anon) + $(kv file) + $(kv kernel) ))
printf 'MemTotal - MemFree            = %8d kB\n' "$used"
printf 'memcg anon + file + kernel    = %8d kB\n' "$memcg"
printf 'unaccounted (driver, memcg=none...) = %d kB\n' "$(( used - memcg ))"
echo
echo "memory.stat values are BYTES; meminfo is kB. 'kernel' is NOT (stack+pt+percpu+vmalloc+slab)."

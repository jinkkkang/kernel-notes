#!/usr/bin/env bash
# Walk the v2 tree the way the kernel sees it: mount -> root files -> subtrees
# -> the current task's cgroup -> a leaf. Read-only.
set -uo pipefail

ROOT=/sys/fs/cgroup
show() { printf '%-34s %s\n' "$1" "$2"; }

echo "=== 1. is this v2? ==="
show "mount:" "$(mount | grep 'type cgroup2' || echo 'NO cgroup2')"
show "stat -fc %T:" "$(stat -fc %T "$ROOT")"

echo
echo "=== 2. root interfaces ==="
show "cgroup.controllers:" "$(cat "$ROOT/cgroup.controllers" 2>/dev/null)"
show "cgroup.subtree_control:" "$(cat "$ROOT/cgroup.subtree_control" 2>/dev/null)"
while read -r k v; do show "cgroup.stat $k:" "$v"; done < "$ROOT/cgroup.stat"
show "cgroup.procs count:" "$(wc -l < "$ROOT/cgroup.procs")"

echo
echo "=== 3. who created each top-level child? ==="
# systemd naming: *.slice / *.scope / *.service are systemd units.
# anything else was created by a userspace mkdir().
for d in "$ROOT"/*/; do
	name=$(basename "$d")
	case $name in
	*.slice|*.service|*.scope|*.mount|*.socket|*.swap|*.target|*.path|*.timer|*.device)
		owner=systemd ;;
	*)
		owner="userspace (not systemd)" ;;
	esac
	printf '%-28s %-24s procs=%-4s populated=%s\n' "$name" "$owner" \
		"$(wc -l < "$d/cgroup.procs" 2>/dev/null || echo ?)" \
		"$(awk '/^populated/{print $2}' "$d/cgroup.events" 2>/dev/null)"
done

echo
echo "=== 4. controller propagation: root -> child chain ==="
rel=$(cut -d: -f3 /proc/self/cgroup)
cur="$ROOT$rel"
show "self /proc/self/cgroup:" "$rel"
# Walk upward printing cgroup.controllers at each level.
chain=()
p=$cur
while [[ -n $p && $p != "$ROOT" ]]; do chain=("$p" "${chain[@]}"); p=$(dirname "$p"); done
for c in "$ROOT" "${chain[@]}"; do
	printf '%-60s <%s>\n' "${c#$ROOT/}" "$(cat "$c/cgroup.controllers" 2>/dev/null)"
done

echo
echo "=== 5. the current task's cgroup: is it a leaf? ==="
show "cgroup.type:" "$(cat "$cur/cgroup.type" 2>/dev/null || echo '(root)')"
show "cgroup.subtree_control:" "$(cat "$cur/cgroup.subtree_control" 2>/dev/null)"
show "cgroup.events:" "$(tr '\n' ' ' < "$cur/cgroup.events" 2>/dev/null)"
show "pids.current / pids.max:" "$(cat "$cur/pids.current") / $(cat "$cur/pids.max")"

echo
echo "=== 6. a real systemd leaf for comparison ==="
leaf=$(ls -d "$ROOT"/system.slice/*.service 2>/dev/null | head -1)
if [[ -n $leaf ]]; then
	printf 'leaf: %s\n' "${leaf#$ROOT/}"
	for f in cgroup.controllers cgroup.type memory.current memory.max cpu.max; do
		[[ -e $leaf/$f ]] && show "  $f:" "$(cat "$leaf/$f" | tr '\n' ' ')"
	done
	show "  procs:" "$(head -3 "$leaf/cgroup.procs" | tr '\n' ' ')"
fi

echo
echo "=== 7. dying cgroups ==="
show "root nr_descendants:" "$(awk '/^nr_descendants/{print $2}' "$ROOT/cgroup.stat")"
show "root nr_dying_descendants:" "$(awk '/^nr_dying_descendants/{print $2}' "$ROOT/cgroup.stat")"
echo "(dying = rmdir'ed but still referenced; see kernel/cgroup/cgroup.c cgroup_destroy_locked)"

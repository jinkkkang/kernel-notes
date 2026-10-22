#!/usr/bin/env bash
# cgroup v2 smoke: read-only by default. Set CG_DEMO_WRITE=1 to try mkdir+procs.
set -euo pipefail

echo "=== meta ==="
date '+%Y-%m-%d %H:%M:%S %Z'
uname -a
echo "pid=$$"
echo "proc cgroup: $(cat /proc/self/cgroup)"

echo
echo "=== root controllers ==="
ROOT=/sys/fs/cgroup
if [[ ! -d $ROOT ]]; then
  echo "no $ROOT"; exit 1
fi
cat "$ROOT/cgroup.controllers" 2>/dev/null || true
echo -n "subtree_control: "; cat "$ROOT/cgroup.subtree_control" 2>/dev/null || true

BASE=${CG_DEMO_ROOT:-}
if [[ -z ${BASE} ]]; then
  # prefer current path under cgroup2
  rel=$(cut -d: -f3 /proc/self/cgroup)
  BASE="$ROOT$rel"
fi
echo
echo "=== demo base: $BASE ==="
ls "$BASE" 2>/dev/null | head -30 || true

if [[ ${CG_DEMO_WRITE:-0} != 1 ]]; then
  echo
  echo "DRY-RUN only. Export CG_DEMO_WRITE=1 and CG_DEMO_ROOT=<writable cgroup dir> to try:"
  echo "  mkdir \$BASE/demo-\$\$"
  echo "  echo \$\$ > \$BASE/demo-\$\$/cgroup.procs"
  exit 0
fi

if [[ ! -w $BASE ]]; then
  echo "BASE not writable: $BASE" >&2
  exit 1
fi

# If controllers must be enabled on parent for children to see them, show hint
if [[ -f $BASE/cgroup.subtree_control ]]; then
  echo "parent subtree_control: $(cat "$BASE/cgroup.subtree_control")"
fi

D="$BASE/demo-$$"
echo "mkdir $D"
mkdir "$D"
echo "self -> $D/cgroup.procs"
echo $$ > "$D/cgroup.procs"
echo "now: $(cat /proc/self/cgroup)"
echo "procs:"; cat "$D/cgroup.procs"
echo "OK. Remove with: rmdir $D  (after moving tasks out)"

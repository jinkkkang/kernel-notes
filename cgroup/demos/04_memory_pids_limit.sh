#!/usr/bin/env bash
# MEASURED: create a throwaway cgroup with a memory.max and pids.max, run a
# child in it, watch memory.current / memory.events / pids.events.
#
#   ./04_memory_pids_limit.sh              # DRY-RUN: print the plan only
#   CG_WRITE=1 ./04_memory_pids_limit.sh   # actually mkdir + write (root, v2, writable)
#
# Safe by default: it never touches an existing cgroup, only cg-demo-<pid> under
# the parent you point it at, and removes it on exit.
set -uo pipefail

ROOT=/sys/fs/cgroup
PARENT=${CG_PARENT:-$ROOT}          # must be writable and have memory/pids in subtree_control
MEM_MAX=${CG_MEM_MAX:-64M}
PIDS_MAX=${CG_PIDS_MAX:-32}
D=$PARENT/cg-demo-$$
PY=${PYTHON:-python3}

fail() { echo "ERR: $*" >&2; exit 1; }

echo "plan:"
echo "  parent       $PARENT"
echo "  mkdir        $D"
echo "  memory.max   $MEM_MAX"
echo "  pids.max     $PIDS_MAX"
echo "  child 1      $PY -c 'bytearray(200MB)'  -> touches pages, expect OOM inside cgroup"
echo "  child 2      fork bomb-ish 40 procs    -> expect pids.max = max (EAGAIN)"
echo

if [[ ${CG_WRITE:-0} != 1 ]]; then
	echo "DRY-RUN. Re-run with CG_WRITE=1 to execute."
	exit 0
fi

[[ $EUID -eq 0 ]] || fail "need root"
[[ $(stat -fc %T "$ROOT") == cgroup2fs ]] || fail "not cgroup2"
[[ -w $PARENT ]] || fail "parent not writable: $PARENT"
grep -qw memory "$PARENT/cgroup.subtree_control" || fail "$PARENT does not delegate memory"
grep -qw pids   "$PARENT/cgroup.subtree_control" || fail "$PARENT does not delegate pids"

mkdir "$D" || fail "mkdir"
cleanup() {
	[[ -d $D ]] || return
	echo 1 > "$D/cgroup.kill" 2>/dev/null
	for _ in $(seq 50); do
		[[ -s $D/cgroup.procs ]] || break
		sleep 0.1
	done
	rmdir "$D" 2>/dev/null && echo "removed $D" || echo "left $D (dying; check cgroup.stat)"
}
trap cleanup EXIT

echo "$MEM_MAX"  > "$D/memory.max"
echo "$PIDS_MAX" > "$D/pids.max"
echo "created $D, memory.max=$(cat "$D/memory.max") pids.max=$(cat "$D/pids.max")"

echo
echo "--- child 1: allocate 200MB of anonymous memory under a $MEM_MAX cap ---"
before_oom=$(awk '/^oom_kill/{print $2}' "$D/memory.events")
set +e
"$PY" - "$D" <<'EOF'
import os, sys
cg = sys.argv[1]
os.makedirs(cg, exist_ok=True)
# Put *this* process into the cgroup before touching memory.
with open(f"{cg}/cgroup.procs", "w") as f:
    f.write(str(os.getpid()))

blocks = []
try:
    for i in range(200):
        b = bytearray(1024 * 1024)
        for j in range(0, len(b), 4096):   # touch every page
            b[j] = 1
        blocks.append(b)
        if i % 20 == 0:
            cur = int(open(f"{cg}/memory.current").read())
            print(f"  allocated {i+1:3d}MB  memory.current={cur//1048576}MB")
except MemoryError:
    print("  MemoryError (userspace alloc failed)")
print("  survived; freed now" if len(blocks) < 100 else "  finished all 200MB?!")
EOF
rc=$?
set -e
echo "  python exit=$rc (137 = SIGKILL by memcg OOM)"
kill -0 0 2>/dev/null # no-op, keeps set -e happy

# OOM kill is asynchronous w.r.t. memory.events; give the flusher a moment.
sleep 0.3
echo "  memory.events: $(awk '$1=="oom"||$1=="oom_kill"||$1=="max"{printf "%s=%s ", $1, $2}' "$D/memory.events")"
echo "  memory.current after: $(( $(cat "$D/memory.current") / 1048576 ))MB"
echo "  memory.peak:          $(( $(cat "$D/memory.peak" 2>/dev/null || echo 0) / 1048576 ))MB"

echo
echo "--- child 2: pids.max in action ---"
"$PY" - "$D" "$PIDS_MAX" <<'EOF'
import os, sys, time
cg, pmax = sys.argv[1], int(sys.argv[2])
with open(f"{cg}/cgroup.procs", "w") as f:
    f.write(str(os.getpid()))
kids = []
eagain = 0
for i in range(pmax * 2):
    try:
        pid = os.fork()
        if pid == 0:
            time.sleep(3); os._exit(0)
        kids.append(pid)
    except (BlockingIOError, OSError) as e:
        eagain += 1
        if eagain == 1:
            print(f"  fork #{i+1} failed: {e!r}")
print(f"  spawned {len(kids)} children, {eagain} fork failures")
for p in kids:
    os.waitpid(p, 0)
EOF
echo "  pids.events: $(cat "$D/pids.events")"
echo "  pids.peak:   $(cat "$D/pids.peak" 2>/dev/null || echo n/a)"

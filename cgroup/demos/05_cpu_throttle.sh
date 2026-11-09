#!/usr/bin/env bash
# MEASURED: cpu.max throttling. Sets cpu.max="20000 100000" (20% of one CPU),
# runs a busy loop, and shows cpu.stat / cpu.stat.local / cpu.pressure.
#
#   ./05_cpu_throttle.sh             # DRY-RUN
#   CG_WRITE=1 ./05_cpu_throttle.sh  # run for CG_SECS (default 3) seconds
set -uo pipefail

ROOT=/sys/fs/cgroup
PARENT=${CG_PARENT:-$ROOT}
QUOTA=${CG_QUOTA:-20000}     # us per period
PERIOD=${CG_PERIOD:-100000}  # us
SECS=${CG_SECS:-3}
D=$PARENT/cg-demo-cpu-$$
PY=${PYTHON:-python3}

echo "plan: mkdir $D ; cpu.max = '$QUOTA $PERIOD' (${QUOTA}/${PERIOD} = $(( QUOTA * 100 / PERIOD ))% of one CPU)"
echo "      busy loop for ${SECS}s, then cpu.stat / cpu.stat.local / cpu.pressure"
if [[ ${CG_WRITE:-0} != 1 ]]; then
	echo "DRY-RUN. Re-run with CG_WRITE=1."
	exit 0
fi

[[ $EUID -eq 0 ]] || { echo "need root" >&2; exit 1; }
grep -qw cpu "$PARENT/cgroup.subtree_control" || { echo "$PARENT does not delegate cpu" >&2; exit 1; }

mkdir "$D" || exit 1
cleanup() {
	[[ -d $D ]] || return
	echo 1 > "$D/cgroup.kill" 2>/dev/null
	for _ in $(seq 50); do [[ -s $D/cgroup.procs ]] || break; sleep 0.1; done
	rmdir "$D" 2>/dev/null && echo "removed $D"
}
trap cleanup EXIT

echo "$QUOTA $PERIOD" > "$D/cpu.max"
echo "cpu.max=$(cat "$D/cpu.max")  cpu.weight=$(cat "$D/cpu.weight")"

t0=$EPOCHREALTIME
"$PY" - "$D" "$SECS" <<'EOF'
import os, sys, time
cg, secs = sys.argv[1], float(sys.argv[2])
with open(f"{cg}/cgroup.procs", "w") as f:
    f.write(str(os.getpid()))
end = time.monotonic() + secs
while time.monotonic() < end:
    pass
EOF
t1=$EPOCHREALTIME
wall=$(printf '%.2f' "$(echo "$t1 - $t0" | bc)")

echo "self-reported wall: ${wall}s (should be ~${SECS}s: wall time is NOT slowed, only quota)"
echo
echo "cpu.stat:"
cat "$D/cpu.stat"
echo
echo "cpu.stat.local:"
cat "$D/cpu.stat.local" 2>/dev/null || echo "(absent)"
echo
echo "cpu.pressure:"
cat "$D/cpu.pressure"
echo
echo "read it as:"
echo "  usage_usec / ${SECS}s  ~= quota ratio  -> we got about $(( QUOTA * 100 / PERIOD ))% of a CPU"
echo "  nr_throttled > 0       -> the scheduler cut us off at period boundaries"

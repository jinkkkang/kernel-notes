#!/usr/bin/env bash
# Capture a sched observability snapshot (read-mostly).
# Safe-ish defaults: no perf record unless CAPTURE_PERF=1.
set -euo pipefail
OUT=${1:-./box-sched-obs.txt}
{
  echo "=== uname ==="; uname -a
  echo "=== date Asia/Shanghai ==="; TZ=Asia/Shanghai date '+%Y-%m-%d %H:%M:%S %Z'
  echo "=== nproc ==="; nproc
  echo "=== loadavg ==="; cat /proc/loadavg
  echo "=== /proc/stat head ==="; head -5 /proc/stat
  echo "=== PSI cpu ==="; cat /proc/pressure/cpu 2>&1 || true
  echo "=== cpu.pressure root ==="; cat /sys/fs/cgroup/cpu.pressure 2>&1 || true
  echo "=== sched_debug ==="; ls -la /proc/sched_debug 2>&1 || true
  echo "=== schedstat ==="; ls -la /proc/schedstat 2>&1 || true
  echo "=== self sched ==="; ls -la /proc/self/sched 2>&1 || true
  echo "=== tools ==="; command -v perf || echo 'no perf'; command -v bpftrace || echo 'no bpftrace'
  if [[ "${CAPTURE_PERF:-0}" == 1 ]] && command -v perf >/dev/null; then
    echo "=== perf sched sample ==="
    perf sched record -o /tmp/perf-sched.data -- sleep 1 || true
    perf sched latency -i /tmp/perf-sched.data 2>&1 | head -40 || true
  fi
} | tee "$OUT"

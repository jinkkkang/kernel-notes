#!/bin/sh
# 在 instance 里打开 sched:sched_switch，读几行。
# 实测过：sampled on 6.12.113+。
set -u
T=/sys/kernel/tracing
INST=$T/instances/kn-demo

cleanup() {
	if [ -d "$INST" ]; then
		echo 0 > "$INST/events/sched/sched_switch/enable" 2>/dev/null || true
		echo 0 > "$INST/tracing_on" 2>/dev/null || true
		echo nop > "$INST/current_tracer" 2>/dev/null || true
		rmdir "$INST" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

mkdir "$INST"
echo 1 > "$INST/events/sched/sched_switch/enable"
echo 1 > "$INST/tracing_on"
sleep 0.05
echo 0 > "$INST/tracing_on"
grep -v '^#' "$INST/trace" | head -n 4

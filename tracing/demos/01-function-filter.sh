#!/bin/sh
# 只跟踪 do_sys_openat2，读几行 function trace。
# 实测过：sampled on 6.12.113+。顶层 current_tracer 保持不动。
set -u
T=/sys/kernel/tracing
INST=$T/instances/kn-demo

cleanup() {
	if [ -d "$INST" ]; then
		echo nop > "$INST/current_tracer" 2>/dev/null || true
		echo 0 > "$INST/tracing_on" 2>/dev/null || true
		: > "$INST/set_ftrace_filter" 2>/dev/null || true
		rmdir "$INST" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

mkdir "$INST"
echo do_sys_openat2 > "$INST/set_ftrace_filter"
echo function > "$INST/current_tracer"
echo 1 > "$INST/tracing_on"
cat /etc/hostname >/dev/null
echo 0 > "$INST/tracing_on"
echo "top enabled_functions:"
cat "$T/enabled_functions"
echo "instance trace:"
grep -v '^#' "$INST/trace" | head -n 6

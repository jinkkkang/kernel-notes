#!/bin/sh
# 顶层 kprobe_events 增加 p:myprobe，在 instance 里 enable。
# kprobe_events 只存在于顶层 tracefs（v6.6.160 trace_kprobe.c 把文件建在顶层）。
# 实测过：sampled on 6.12.113+。
set -u
T=/sys/kernel/tracing
INST=$T/instances/kn-demo

cleanup() {
	if [ -d "$INST" ]; then
		if [ -e "$INST/events/kprobes/myprobe/enable" ]; then
			echo 0 > "$INST/events/kprobes/myprobe/enable" 2>/dev/null || true
		fi
		echo 0 > "$INST/tracing_on" 2>/dev/null || true
		echo nop > "$INST/current_tracer" 2>/dev/null || true
		rmdir "$INST" 2>/dev/null || true
	fi
	if grep -q myprobe "$T/kprobe_events" 2>/dev/null; then
		echo '-:myprobe' >> "$T/kprobe_events" || true
	fi
}
trap cleanup EXIT INT TERM

mkdir "$INST"
echo 'p:myprobe __x64_sys_getpid' >> "$T/kprobe_events"
echo 1 > "$INST/events/kprobes/myprobe/enable"
echo 1 > "$INST/tracing_on"
python3 -c 'import ctypes; ctypes.CDLL(None).getpid()'
echo 0 > "$INST/tracing_on"
echo "kprobe_events=$(cat "$T/kprobe_events")"
grep -v '^#' "$INST/trace" | head -n 4

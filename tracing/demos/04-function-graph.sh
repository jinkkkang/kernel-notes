#!/bin/sh
# function_graph 只跟踪 do_sys_openat2。
# set_graph_function 只在顶层 tracefs（v6.6.160 ftrace.c 的 ftrace_init_dyn_tracefs）。
# 打开它会改顶层 current_tracer。脚本结束时设回 nop 并清空名单。
# RECIPE：默认不执行。开销见 tracing/12-perf-analysis.md。
# 要跑：sudo TRACE_RUN_GRAPH=1 ./04-function-graph.sh
set -u
T=/sys/kernel/tracing
if [ "${TRACE_RUN_GRAPH:-0}" != 1 ]; then
	echo "RECIPE: set TRACE_RUN_GRAPH=1 to enable function_graph on do_sys_openat2"
	echo "commands (top-level tracefs, then restore nop):"
	echo "  echo do_sys_openat2 > $T/set_graph_function"
	echo "  echo function_graph > $T/current_tracer"
	echo "  echo 1 > $T/tracing_on"
	echo "  cat /etc/hostname"
	echo "  echo 0 > $T/tracing_on"
	echo "  echo nop > $T/current_tracer"
	echo "  : > $T/set_graph_function"
	exit 0
fi

prev=$(cat "$T/current_tracer")
cleanup() {
	echo nop > "$T/current_tracer" 2>/dev/null || true
	echo 0 > "$T/tracing_on" 2>/dev/null || true
	: > "$T/set_graph_function" 2>/dev/null || true
	if [ -n "${prev:-}" ] && [ "$prev" != nop ]; then
		echo "$prev" > "$T/current_tracer" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

echo do_sys_openat2 > "$T/set_graph_function"
echo function_graph > "$T/current_tracer"
echo 1 > "$T/tracing_on"
cat /etc/hostname >/dev/null
echo 0 > "$T/tracing_on"
grep -v '^#' "$T/trace" | head -n 20

#!/usr/bin/env bash
# RECIPE / 待真机验证.
# Default is dry-run: print what would happen and exit 0. No insmod.
# On a test machine with CAP_SYS_MODULE and matching headers:
#   KN_DEMO_LOAD=1 ./01-run.sh
# Optional message token (also the grep key in dmesg):
#   KN_DEMO_LOAD=1 KN_DEMO_MSG=hello ./01-run.sh
# Do not run the load path on a production machine.

set -euo pipefail

cd "$(dirname "$0")"

echo "=== meta ==="
date '+%Y-%m-%d %H:%M:%S %Z'
uname -a
kver="$(uname -r)"
echo "kver=${kver}"
echo "needs CAP_SYS_MODULE and /lib/modules/${kver}/build"
echo "module kn_demo_wq; workqueue kn-demo-wq; status 待真机验证"

if [[ ${KN_DEMO_LOAD:-0} != 1 ]]; then
	echo
	echo "DRY-RUN. No make, no insmod."
	echo "On a test machine: KN_DEMO_LOAD=1 $0"
	exit 0
fi

if [[ "$(id -u)" -ne 0 ]]; then
	echo "KN_DEMO_LOAD=1 needs root (CAP_SYS_MODULE)." >&2
	exit 1
fi

kdir="/lib/modules/${kver}/build"
if [[ ! -d "${kdir}" ]]; then
	echo "missing headers: ${kdir}" >&2
	exit 1
fi

if grep -q '^kn_demo_wq ' /proc/modules; then
	echo "kn_demo_wq is already loaded. rmmod it, then retry." >&2
	exit 1
fi

make

token="${KN_DEMO_MSG:-kn-demo-$$}"
if [[ "${token}" == *" "* ]]; then
	echo "KN_DEMO_MSG must not contain spaces" >&2
	exit 1
fi

insmod ./kn_demo_wq.ko "msg=${token}"
# unbound worker should run promptly; this is a wait, not a measured latency
sleep 1

fail=0
log="$(dmesg || true)"

if ! grep -q '^kn_demo_wq ' /proc/modules; then
	echo "FAIL: kn_demo_wq not in /proc/modules" >&2
	fail=1
else
	echo "=== /proc/modules ==="
	grep '^kn_demo_wq ' /proc/modules
fi

if [[ ! -d /sys/module/kn_demo_wq ]]; then
	echo "FAIL: /sys/module/kn_demo_wq missing" >&2
	fail=1
else
	echo "=== /sys/module/kn_demo_wq ==="
	ls /sys/module/kn_demo_wq
	if [[ -f /sys/module/kn_demo_wq/parameters/msg ]]; then
		echo -n "parameters/msg="
		cat /sys/module/kn_demo_wq/parameters/msg
	else
		echo "FAIL: parameters/msg missing" >&2
		fail=1
	fi
fi

bus=/sys/bus/workqueue/devices/kn-demo-wq
if [[ -d "${bus}" ]]; then
	echo "=== ${bus} ==="
	ls "${bus}"
	if [[ -f ${bus}/per_cpu ]]; then
		echo -n "per_cpu="
		cat "${bus}/per_cpu"
	fi
	if [[ -f ${bus}/max_active ]]; then
		echo -n "max_active="
		cat "${bus}/max_active"
	fi
else
	echo "FAIL: ${bus} missing (workqueue_sysfs_register comment)" >&2
	fail=1
fi

virt=/sys/devices/virtual/workqueue/kn-demo-wq
if [[ -d "${virt}" ]]; then
	echo "virtual device present: ${virt}"
else
	echo "virtual device not present (recorded, not a failure): ${virt}"
fi

if ! grep -F "kn_demo_wq: init" <<<"${log}" | grep -F "${token}" >/dev/null; then
	echo "FAIL: dmesg has no init line containing ${token}" >&2
	fail=1
else
	echo "=== dmesg init ==="
	grep -F "kn_demo_wq: init" <<<"${log}" | grep -F "${token}"
fi

if ! grep -F "kn_demo_wq: work" <<<"${log}" | grep -F "${token}" >/dev/null; then
	echo "FAIL: dmesg has no work line containing ${token}" >&2
	fail=1
else
	echo "=== dmesg work ==="
	grep -F "kn_demo_wq: work" <<<"${log}" | grep -F "${token}"
fi

rmmod kn_demo_wq
sleep 1
log_after="$(dmesg || true)"

if grep -q '^kn_demo_wq ' /proc/modules; then
	echo "FAIL: still listed in /proc/modules after rmmod" >&2
	fail=1
else
	echo "rmmod: gone from /proc/modules"
fi

if [[ -d /sys/module/kn_demo_wq ]]; then
	echo "FAIL: /sys/module/kn_demo_wq still exists" >&2
	fail=1
else
	echo "rmmod: /sys/module/kn_demo_wq gone"
fi

if [[ -d ${bus} ]]; then
	echo "FAIL: ${bus} still exists" >&2
	fail=1
else
	echo "rmmod: ${bus} gone"
fi

if ! grep -F "kn_demo_wq: exit" <<<"${log_after}" | grep -F "${token}" >/dev/null; then
	echo "FAIL: dmesg has no exit line containing ${token}" >&2
	fail=1
else
	echo "=== dmesg exit ==="
	grep -F "kn_demo_wq: exit" <<<"${log_after}" | grep -F "${token}"
fi

if [[ ${fail} -ne 0 ]]; then
	echo "ACCEPTANCE FAILED"
	exit 1
fi

echo "ACCEPTANCE OK. Paste this stdout into the notes as sampled on ${kver}. Status was 待真机验证."

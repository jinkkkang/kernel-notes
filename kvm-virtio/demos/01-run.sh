#!/bin/bash
# Build and exercise demos/01-minimal-kvm.
# Does not sudo, modprobe, chmod, or change nested virt.
# A SKIP from the real /dev/kvm run is passed through as exit 77.
set -euo pipefail

cd "$(dirname "$0")"

if [[ "$(id -u)" -eq 0 ]]; then
	echo "runner: uid 0; this script still does not chmod or modprobe" >&2
fi

make

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

run_capture() {
	set +e
	"$@" >"$tmp/out" 2>"$tmp/err"
	local rc=$?
	set -e
	echo "---- command: $* ----"
	echo "exit=$rc"
	if [[ -s "$tmp/err" ]]; then
		echo "-- stderr --"
		cat "$tmp/err"
	fi
	if [[ -s "$tmp/out" ]]; then
		echo "-- stdout --"
		cat "$tmp/out"
	fi
	CAPTURE_RC=$rc
}

echo "==== self-test: pending SIGALRM ===="
if command -v timeout >/dev/null 2>&1; then
	run_capture timeout --foreground 3 env KN_KVM_TEST_SIGNAL=1 ./01-minimal-kvm
else
	run_capture env KN_KVM_TEST_SIGNAL=1 ./01-minimal-kvm
fi
if [[ "$CAPTURE_RC" -ne 0 ]]; then
	echo "self-test failed: signal handoff exit $CAPTURE_RC, want 0" >&2
	exit 1
fi
if ! grep -q '^signal_test=pending-delivered$' "$tmp/out"; then
	echo "self-test failed: signal handoff did not print signal_test=pending-delivered" >&2
	exit 1
fi
if grep -q '^result=ok$' "$tmp/out"; then
	echo "self-test failed: signal handoff printed result=ok" >&2
	exit 1
fi

echo "==== self-test: missing device ===="
run_capture env KN_KVM_DEVICE="$tmp/no-such-kvm" ./01-minimal-kvm
if [[ "$CAPTURE_RC" -ne 77 ]]; then
	echo "self-test failed: missing device exit $CAPTURE_RC, want 77" >&2
	exit 1
fi
if ! grep -q '^SKIP: no-device ' "$tmp/err"; then
	echo "self-test failed: missing device did not print SKIP: no-device" >&2
	exit 1
fi
if grep -q '^result=ok$' "$tmp/out"; then
	echo "self-test failed: missing device printed result=ok" >&2
	exit 1
fi

echo "==== self-test: /dev/null is not KVM ===="
run_capture env KN_KVM_DEVICE=/dev/null ./01-minimal-kvm
if [[ "$CAPTURE_RC" -eq 0 || "$CAPTURE_RC" -eq 77 ]]; then
	echo "self-test failed: /dev/null exit $CAPTURE_RC, want a real error" >&2
	exit 1
fi
if ! grep -q '^ERROR: ' "$tmp/err"; then
	echo "self-test failed: /dev/null did not print ERROR" >&2
	exit 1
fi
if grep -q '^result=ok$' "$tmp/out"; then
	echo "self-test failed: /dev/null printed result=ok" >&2
	exit 1
fi

echo "==== real device ===="
echo "runner_uname=$(uname -a)"
if command -v systemd-detect-virt >/dev/null 2>&1; then
	echo "runner_virt=$(systemd-detect-virt || true)"
else
	echo "runner_virt=UNKNOWN"
fi
run_capture ./01-minimal-kvm
echo "real_exit=$CAPTURE_RC"
if [[ "$CAPTURE_RC" -eq 77 ]]; then
	echo "KVM_RUN was not executed. Success path remains 待真机验证."
fi
exit "$CAPTURE_RC"

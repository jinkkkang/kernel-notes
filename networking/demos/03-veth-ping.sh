#!/bin/sh
# Two netns, one veth pair, one ICMP echo. Needs CAP_NET_ADMIN.
set -eu
NS_A=kn-net-a-$$
NS_B=kn-net-b-$$
cleanup() {
	ip netns del "$NS_A" 2>/dev/null || true
	ip netns del "$NS_B" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if ! ip netns add "$NS_A" 2>/dev/null; then
	echo "kernel=$(uname -r)"
	echo "veth_ping=skipped reason=CAP_NET_ADMIN"
	exit 0
fi
ip netns add "$NS_B"
ip link add veth-a type veth peer name veth-b
ip link set veth-a netns "$NS_A"
ip link set veth-b netns "$NS_B"
ip -n "$NS_A" addr add 10.88.0.1/24 dev veth-a
ip -n "$NS_B" addr add 10.88.0.2/24 dev veth-b
ip -n "$NS_A" link set veth-a up
ip -n "$NS_B" link set veth-b up
ip -n "$NS_A" link set lo up
ip -n "$NS_B" link set lo up
echo "kernel=$(uname -r)"
ip -n "$NS_A" link show veth-a | head -n 2
ip -n "$NS_B" -4 addr show veth-b | sed -n '1,3p'
ip netns exec "$NS_A" ping -c 1 -W 2 10.88.0.2
echo "veth_ping=ok"

#!/bin/bash
# This script is for running and configuring L4Proxy
# Each L4Proxy instance should run with an unique BESSD port, an unique set of CPU cores, and an unique service IP:Port.
# Note: besides this script, you have to configure Linux tc (at the bottom of this doc) in order to route traffic for the instance

set -e

usage () {
    echo "run_l4_proxy.sh -a <BESSD_PORT> -b <BESSD_ID> -c <CORES> -d <NIC> -i <SERVICE_IP> -n <NEXT_HOP_HOSTNAME> -u <USER_IP>"
}

# Assign an unique TCP port for standing up the bess daemon
# for controlling L4Proxy's behavior
BESSD_PORT=""
# Assign an unique ID for this L4Proxy
BESSD_ID=""
# By default, L4Proxy runs on core 1
NODE_CORES="1"
# Specify the NIC hardware that is assigned with IP |NODE_IP|
NODE_IFACE="enp2s0f0"
# L4Proxy service's public IP:Port
NODE_IP=""
NODE_PORT="443"
# Next-hop endpoint's public IP:Port
NEXT_HOP_HOSTNAME=""
NEXT_HOP_PORT="8444"
# Put |USER_IP| into L4Proxy's whitelist if it is not empty
USER_IP=""

while getopts "h?a:b:c:d:i:n:u:" opt; do
    case "${opt}" in
        h|\?)
            usage
            exit 0
            ;;
        a)
            BESSD_PORT=${OPTARG}
            ;;
        b)
            BESSD_ID=${OPTARG}
            ;;
        c)
            NODE_CORES=${OPTARG}
            ;;
        d)
            NODE_IFACE=${OPTARG}
            ;;
        i)
            NODE_IP=${OPTARG}
            ;;
        n)
            NEXT_HOP_HOSTNAME=${OPTARG}
            ;;
        u)
            USER_IP=${OPTARG}
            ;;
    esac
done

if [ -z ${BESSD_PORT} ]; then
        usage
        exit -1
fi
if [ -z ${BESSD_ID} ]; then
        usage
        exit -1
fi
if [ -z ${NODE_CORES} ]; then
        usage
        exit -1
fi
if [ -z ${NODE_IFACE} ]; then
        usage
        exit -1
fi
if [ -z ${NODE_IP} ]; then
        usage
        exit -1
fi
if [ -z ${NEXT_HOP_HOSTNAME} ]; then
        usage
        exit -1
fi

# Check if NEXT_HOP_HOSTNAME is already an IP address
if [[ $NEXT_HOP_HOSTNAME =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    # It's an IP address, use it directly
    NEXT_HOP_IP=$NEXT_HOP_HOSTNAME
else
    # Not an IP address, resolve it using dig
    NEXT_HOP_IP=$(dig +noall +short -t A $NEXT_HOP_HOSTNAME | awk '{print; exit}')
fi

# Do not send ICMP / TCP RST packets
/sbin/iptables -A OUTPUT -p icmp -m icmp --icmp-type port-unreachable -j DROP
/sbin/iptables -A OUTPUT -p tcp --tcp-flags RST RST -j DROP

# Run bessd (need to run container with --privileged)
/bess/core/bessd --iova=va --dpdk=false --m=0 --grpc_url="127.0.0.1:${BESSD_PORT}"

# L4 proxy configure example:
/bess/bessctl/bessctl "run invisv/l4_proxy_deploy_vdev BESS_DEV='eth0',BESS_DEV_ID=${BESSD_ID},BESS_CORES='${NODE_CORES}'"
sleep 0.5

/bess/bessctl/bessctl "command module l4proxy set_proxy INVISVL4ProxySetProxyEndpointArg {'proxy_addr':'${NODE_IP}', 'proxy_tcp_port': ${NODE_PORT}, 'proxy_udp_port': ${NODE_PORT}}"
sleep 0.5

/bess/bessctl/bessctl "command module l4proxy set_next_hop_endpoint INVISVL4ProxySetNextHopEndpointArg {'next_hop_addr':'${NEXT_HOP_IP}', 'next_hop_tcp_port': ${NEXT_HOP_PORT}, 'next_hop_udp_port': ${NEXT_HOP_PORT}}"

# Add a client IP if |USER_IP| is set
if [ ! -z ${USER_IP} ]; then
        sleep 0.5
        /bess/bessctl/bessctl "command module l4proxy set_client_allowlist INVISVL4ProxySetClientAllowlistArg {'client_addr':'${USER_IP}', 'add': True}"
fi

# Configure traffic redirect Linux tc
tc qdisc add dev ${NODE_IFACE} ingress handle ffff:0 || true

# Delete a previous proxy's rule (with the same BESSD ID)
tc filter del dev ${NODE_IFACE} parent ffff: protocol ip prio ${BESSD_ID} || true

# Ingress: NIC -> PROXY
# - upstream traffic: dport 8765
# - downstream traffic: sport 2345
tc filter add dev ${NODE_IFACE} parent ffff: protocol ip prio ${BESSD_ID} u32 match ip dst ${NODE_IP}/32 match ip dport ${NODE_PORT} 0xffff action mirred egress redirect dev proxytap${BESSD_ID} || true
tc filter add dev ${NODE_IFACE} parent ffff: protocol ip prio ${BESSD_ID} u32 match ip src ${NEXT_HOP_IP}/32 match ip sport ${NEXT_HOP_PORT} 0xffff action mirred egress redirect dev proxytap${BESSD_ID} || true

# Egress: PROXY -> NIC
tc filter add dev proxytap${BESSD_ID} parent ffff: protocol all u32 match u8 0 0 action mirred egress redirect dev ${NODE_IFACE} || true

# To reset Linux tc:
# tc filter del dev ${NODE_IFACE} parent ffff: protocol ip prio 1
# tc qdisc del dev ${NODE_IFACE} ingress (only delete after all proxy instances were removed)

# Hold
BESSPID=$(pidof bessd)
while [ -e /proc/$BESSPID ]; do
    sleep 1
done

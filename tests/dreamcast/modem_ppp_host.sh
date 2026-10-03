#!/bin/sh
# modem_ppp_host.sh - be the dial-up provider for the emulated Dreamcast modem.
#
# Start QEMU with the modem's phone line on a pty:
#
#   qemu-system-sh4 -M dreamcast,modem=mdm -chardev pty,id=mdm ...
#
# QEMU prints "char device redirected to /dev/pts/N (label mdm)".  Run this
# script on that pty; it answers the call with a pppd, like a DreamPi does:
#
#   sudo tests/dreamcast/modem_ppp_host.sh /dev/pts/N [--nat <uplink-iface>]
#
# Then, in the guest:
#
#   pppd /dev/ttyDCM0 noauth defaultroute usepeerdns \
#        connect "chat '' ATDT1111111 CONNECT ''"
#   ping 10.0.9.1
#
# With --nat the guest's traffic is masqueraded out of <uplink-iface>, so the
# Dreamcast reaches the internet through ppp0.

set -e

HOST_IP=10.0.9.1
GUEST_IP=10.0.9.2
DNS=${DNS:-1.1.1.1}

pty=$1
[ -c "$pty" ] || { echo "usage: $0 /dev/pts/N [--nat <iface>]" >&2; exit 1; }

if [ "$2" = "--nat" ]; then
    [ -n "$3" ] || { echo "--nat needs the uplink interface" >&2; exit 1; }
    sysctl -q -w net.ipv4.ip_forward=1
    iptables -t nat -C POSTROUTING -s $GUEST_IP -o "$3" -j MASQUERADE 2>/dev/null ||
        iptables -t nat -A POSTROUTING -s $GUEST_IP -o "$3" -j MASQUERADE
    trap 'iptables -t nat -D POSTROUTING -s $GUEST_IP -o "$3" -j MASQUERADE' EXIT
fi

# "silent" waits for the guest to start LCP, so pppd can be started before
# the call; "local" because a pty has no modem control lines.
pppd "$pty" nodetach debug noauth local silent \
     $HOST_IP:$GUEST_IP ms-dns "$DNS"

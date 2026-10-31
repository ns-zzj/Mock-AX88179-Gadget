#!/bin/bash
#
# Configure the downstream subnet served by the emulated AX88179 NIC.
#
# This script changes system state (routing, firewall, dnsmasq), so it is
# deliberately conservative:
#   - it never installs packages; missing tools are reported and it exits
#   - existing config files are only replaced after an explicit confirmation
#   - the uplink interface is detected, and can be overridden with --uplink
#
set -u

TAP=ax88179
SUBNET=10.10.10.0/24
GATEWAY=10.10.10.1
CLIENT_IP=10.10.10.50
CLIENT_MAC=02:00:00:00:00:01
DNS1=223.5.5.5
DNS2=9.9.9.9

CONF=/etc/dnsmasq.d/ax88179.conf
UDEV=/etc/udev/rules.d/90-ax88179-dnsmasq.rules
SYSCTL=/etc/sysctl.d/99-ax88179.conf

UPLINK=""

usage() {
    cat <<EOF
usage: $0 [--uplink <interface>]

  --uplink  interface that provides the upstream network
            (default: detected from the default route)
EOF
}

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -u|--uplink) [ $# -ge 2 ] || die "--uplink needs a value"; UPLINK=$2; shift 2 ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "unknown argument: $1 (try --help)" ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "must run as root"

# ---------------------------------------------------------------- uplink

if [ -z "$UPLINK" ]; then
    UPLINK=$(ip route show default 2>/dev/null | awk '{print $5; exit}')
    [ -n "$UPLINK" ] || die "cannot detect the uplink interface; use --uplink <interface>"
    info "uplink detected: $UPLINK"
else
    info "uplink: $UPLINK"
fi
ip link show "$UPLINK" >/dev/null 2>&1 || die "no such interface: $UPLINK"

# ---------------------------------------------------------- dependencies

info "checking dependencies"
missing=""
for c in dnsmasq iptables ip sysctl udevadm; do
    command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
done

if [ -n "$missing" ]; then
    cat >&2 <<EOF

Missing tools:$missing

This script does not install anything. Install them first, for example:

    Debian / Ubuntu : apt install dnsmasq iptables
    Fedora          : dnf install dnsmasq iptables
    Arch            : pacman -S dnsmasq iptables-nft
    Alpine          : apk add dnsmasq iptables iproute2

Then run this script again.
EOF
    exit 1
fi
info "dependencies ok"

# ------------------------------------------------------------- helpers

ask_write() {
    # ask_write <path> <content>
    local path=$1 content=$2

    if [ -e "$path" ] && [ -s "$path" ]; then
        if [ "$(cat "$path")" = "$content" ]; then
            info "$path is already up to date"
            return 0
        fi
        echo
        echo "    $path already exists and differs from what this script would write:"
        echo
        printf '%s\n' "$content" | diff -u "$path" - | sed 's/^/    /'
        echo
        printf '    overwrite %s? [y/N] ' "$path"
        read -r ans
        case "$ans" in
            y|Y) ;;
            *)   die "aborted; $path was left untouched" ;;
        esac
    fi

    printf '%s\n' "$content" > "$path"
    info "wrote $path"
}

# ------------------------------------------------------------ ip_forward

info "enabling ip_forward"
sysctl -q -w net.ipv4.ip_forward=1
ask_write "$SYSCTL" "net.ipv4.ip_forward=1"

# -------------------------------------------------------------- firewall

info "installing firewall rules"

add_rule() {
    if iptables -C "$@" 2>/dev/null; then
        echo "    exists : $*"
    else
        iptables -A "$@"
        echo "    added  : $*"
    fi
}

if iptables -t nat -C POSTROUTING -o "$UPLINK" -j MASQUERADE 2>/dev/null; then
    echo "    exists : POSTROUTING -o $UPLINK MASQUERADE"
else
    iptables -t nat -A POSTROUTING -o "$UPLINK" -j MASQUERADE
    echo "    added  : POSTROUTING -o $UPLINK MASQUERADE"
fi

add_rule FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT
add_rule FORWARD -i "$UPLINK" -o "$TAP" -m state --state ESTABLISHED,RELATED -j ACCEPT

if command -v netfilter-persistent >/dev/null 2>&1; then
    netfilter-persistent save >/dev/null 2>&1 && info "firewall rules saved"
else
    cat <<'EOF'

    ##############################################################
    #  STRONGLY RECOMMENDED: install a firewall rule saver       #
    ##############################################################
    'netfilter-persistent' was not found, so these NAT rules will
    NOT survive a reboot.  After a reboot the host still gets an
    address but has no internet until you run this script again.

    Install it once and the rules are restored automatically:

        Debian / Ubuntu : apt install iptables-persistent
        Fedora          : dnf install iptables-services
                          iptables-save > /etc/sysconfig/iptables
        Arch            : pacman -S iptables-nft

EOF
fi

# --------------------------------------------------------------- dnsmasq

info "configuring dnsmasq"

DM_CONF=$(cat <<EOF
# Downstream subnet for the emulated AX88179 NIC.
interface=$TAP
bind-dynamic

dhcp-range=interface:$TAP,$CLIENT_IP,$CLIENT_IP,12h
dhcp-option=3,$GATEWAY
dhcp-option=6,$GATEWAY,$DNS1

# The gadget reports a fixed MAC, so pin its lease.
dhcp-host=$CLIENT_MAC,$CLIENT_IP

server=$DNS1
server=$DNS2
EOF
)

ask_write "$CONF" "$DM_CONF"

systemctl restart dnsmasq && info "dnsmasq restarted"

# ------------------------------------------------------------------ udev

info "installing udev rule"

UDEV_RULE='ACTION=="add", SUBSYSTEM=="net", KERNEL=="'"$TAP"'", RUN+="/bin/systemctl restart dnsmasq"'
ask_write "$UDEV" "$UDEV_RULE"

udevadm control --reload-rules && info "udev rules reloaded"

# ------------------------------------------------------------------ done

cat <<EOF

Done.

Next steps (from the directory holding the binary and this repo):

    install -m 0755 ax88179_gadget /usr/local/bin/
    install -m 0644 deploy/ax88179-gadget.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable --now ax88179-gadget

Plug a host into the gadget port and it should receive $CLIENT_IP
with gateway $GATEWAY.
EOF

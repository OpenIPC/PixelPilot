#!/bin/sh
#
# apfpv-open-static-cam.sh - CAMERA: open Wi-Fi, no DHCP
#
# Prepares an OpenIPC apfpv camera for PixelPilot's APFPV link mode. Patches
# /usr/bin/adapter (which writes hostapd.conf on every start):
#   - WPA2 off (wpa_passphrase/rsn_pairwise removed, wpa=2 -> wpa=0)
#     -> no 4-way handshake when joining
#   - the DHCP server is no longer started, nor restarted when a station
#     connects; the ground station has the fixed IP 192.168.0.10 (PixelPilot's
#     default in APFPV settings). Clearing the station's ARP entry stays.
#   - disassoc_low_ack=0: hostapd no longer drops the ground station when many
#     frames go unacknowledged during a fade. The association survives the
#     dropout and video resumes without rejoining.
#   - optional: only one given MAC address may join
#
# The access point is restarted afterwards (Wi-Fi drops briefly).
# Also point majestic at the ground station, e.g. in /etc/majestic.yaml:
#   outgoing: { enabled: true, server: udp://192.168.0.10:5600 }
#
# On the camera:
#   sh apfpv-open-static-cam.sh                          install
#   sh apfpv-open-static-cam.sh --allow-mac aa:bb:cc:dd:ee:ff
#                                                        install + allow only this MAC
#   sh apfpv-open-static-cam.sh --no-restart             install, active from the next start
#   sh apfpv-open-static-cam.sh --revert                 restore the backup
# From a PC:
#   ssh root@192.168.0.1 sh -s < apfpv-open-static-cam.sh
#   ssh root@192.168.0.1 sh -s -- --allow-mac aa:bb:cc:dd:ee:ff < apfpv-open-static-cam.sh

ADAPTER=${ADAPTER:-/usr/bin/adapter}
BACKUP=${BACKUP:-/root/adapter.before-open-static}
ACCEPT=${ACCEPT:-/etc/hostapd.accept}
MARK="apfpv-open-static"
RESTART=1
REVERT=0
MAC=""

die() { echo "ERROR: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --revert) REVERT=1 ;;
        --no-restart) RESTART=0 ;;
        --allow-mac)
            # Without a value the filter would silently be left out.
            [ -n "${2:-}" ] || die "--allow-mac needs a MAC address, nothing changed."
            shift; MAC=$(echo "$1" | tr 'A-F' 'a-f') ;;
        *) die "unknown option: $1" ;;
    esac
    shift
done

[ -f "$ADAPTER" ] || die "$ADAPTER not found"

restart_ap() {
    [ "$RESTART" = 1 ] || { echo "Active from the next start (or: setsid $ADAPTER start)."; return; }
    echo "Access point restarts in 2 s - Wi-Fi drops briefly."
    setsid sh -c "sleep 2; $ADAPTER start" >/tmp/adapter-restart.log 2>&1 </dev/null &
}

if [ "$REVERT" = 1 ]; then
    [ -f "$BACKUP" ] || die "no backup at $BACKUP"
    cp "$BACKUP" "$ADAPTER" && chmod 755 "$ADAPTER" || die "restoring failed"
    rm -f "$ACCEPT"
    echo "Reverted: $ADAPTER restored from $BACKUP (WPA2 + DHCP on again)."
    restart_ap
    exit 0
fi

if [ -n "$MAC" ]; then
    echo "$MAC" | grep -qE '^([0-9a-f]{2}:){5}[0-9a-f]{2}$' || die "invalid MAC address: $MAC"
fi

if grep -q "$MARK" "$ADAPTER"; then
    # An older version of this script is installed: only add disassoc_low_ack.
    # The backup at $BACKUP stays the one from the first install (original state).
    if [ "$(grep -c '^disassoc_low_ack=1$' "$ADAPTER")" = 1 ]; then
        TMP="$ADAPTER.tmp.$$"
        sed -e "s|^disassoc_low_ack=1\$|# $MARK: keep the ground station through a fade\\ndisassoc_low_ack=0|" \
            "$ADAPTER" > "$TMP" || { rm -f "$TMP"; die "editing failed, nothing changed."; }
        sh -n "$TMP" || { rm -f "$TMP"; die "syntax error in the result, nothing changed."; }
        grep -q '^disassoc_low_ack=0$' "$TMP" || { rm -f "$TMP"; die "replacing failed, nothing changed."; }
        mv "$TMP" "$ADAPTER" && chmod 755 "$ADAPTER" || die "writing $ADAPTER failed"
        echo "Updated: disassoc_low_ack=0."
        restart_ap
        exit 0
    fi
    echo "Already installed, nothing to do. (To change it, --revert first.)"
    exit 0
fi

# Each spot must exist exactly like this, otherwise it is a different apfpv version.
for pat in '^wpa_passphrase=\$pass$' '^wpa=2$' '^rsn_pairwise=CCMP$' '^disassoc_low_ack=1$' \
           '^        udhcpd -S /etc/udhcpd.conf$' '^                    reset_single_ip_dhcp "\$1"$'; do
    [ "$(grep -c "$pat" "$ADAPTER")" = 1 ] || die "spot '$pat' not found exactly once - nothing changed."
done
# The ARP reset that replaces reset_single_ip_dhcp uses the adapter's $master.
grep -qE '^[[:space:]]*master=' "$ADAPTER" || die "\$master not set in $ADAPTER - nothing changed."

cp "$ADAPTER" "$BACKUP" || die "backup to $BACKUP failed"
TMP="$ADAPTER.tmp.$$"

ACL=""
[ -n "$MAC" ] && ACL="macaddr_acl=1\\naccept_mac_file=$ACCEPT\\n"

sed \
    -e '/^wpa_passphrase=\$pass$/d' \
    -e '/^rsn_pairwise=CCMP$/d' \
    -e "s|^wpa=2\$|# $MARK: open network, no WPA2 handshake\\nwpa=0\\n$ACL|" \
    -e "s|^disassoc_low_ack=1\$|# $MARK: keep the ground station through a fade\\ndisassoc_low_ack=0|" \
    -e "s|^        udhcpd -S /etc/udhcpd.conf\$|        # $MARK: no DHCP, the ground station has the fixed IP \$master|" \
    -e "s|^                    reset_single_ip_dhcp \"\\\$1\"\$|                    ip neigh del \"\$master\" dev \"\$1\" >/dev/null 2>\&1  # $MARK: instead of the DHCP reset|" \
    "$BACKUP" > "$TMP" || { rm -f "$TMP"; die "editing failed, nothing changed."; }

# Drop the blank line an empty $ACL leaves behind
[ -z "$MAC" ] && sed -i '/^wpa=0$/{n;/^$/d}' "$TMP"

sh -n "$TMP" || { rm -f "$TMP"; die "syntax error in the result, nothing changed."; }
[ "$(grep -c "$MARK" "$TMP")" -ge 4 ] || { rm -f "$TMP"; die "not all spots replaced, nothing changed."; }
grep -q '^disassoc_low_ack=0$' "$TMP" || { rm -f "$TMP"; die "disassoc_low_ack not replaced, nothing changed."; }

# The accept file goes first: the patched adapter must never be in place without it.
if [ -n "$MAC" ]; then
    echo "$MAC" > "$ACCEPT" && [ "$(cat "$ACCEPT")" = "$MAC" ] ||
        { rm -f "$TMP" "$ACCEPT"; die "writing $ACCEPT failed, nothing changed."; }
fi
mv "$TMP" "$ADAPTER" && chmod 755 "$ADAPTER" ||
    { cp "$BACKUP" "$ADAPTER"; rm -f "$TMP"; [ -n "$MAC" ] && rm -f "$ACCEPT"; die "writing $ADAPTER failed, original restored."; }
[ -n "$MAC" ] && echo "Only $MAC may join ($ACCEPT)."
echo "Installed. Backup: $BACKUP"

killall -q udhcpd 2>/dev/null
restart_ap
exit 0

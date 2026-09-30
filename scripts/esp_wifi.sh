#!/usr/bin/env bash
# Move the host Wi-Fi card onto the Kestrel ESP32-S3 SoftAP for Phase 2 tests, and back.
#   scripts/esp_wifi.sh up     # wlp9s0 -> kestrel-esp32 (never the default route; Ethernet stays up)
#   KX_HOME_WIFI=<connection> scripts/esp_wifi.sh down   # wlp9s0 -> the host's normal Wi-Fi connection
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IF=${KX_WIFI_IF:-wlp9s0}
HOME_WIFI=${KX_HOME_WIFI:-}
. "$ROOT/esp/secrets.env"
case ${1:-} in
  up)
    if ! nmcli -t -f NAME con show | grep -qx "$KX_AP_SSID"; then
      nmcli con add type wifi ifname "$IF" con-name "$KX_AP_SSID" ssid "$KX_AP_SSID" \
        wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$KX_AP_PASS" connection.autoconnect no \
        ipv4.method auto ipv4.never-default yes ipv6.method disabled >/dev/null
    fi
    nmcli con up "$KX_AP_SSID" ifname "$IF" >/dev/null
    ip -4 -o addr show "$IF" | awk '{print $4}' ;;
  down)
    [ -n "$HOME_WIFI" ] || { echo "set KX_HOME_WIFI to the NetworkManager connection to restore"; exit 2; }
    nmcli con up "$HOME_WIFI" ifname "$IF" >/dev/null && nmcli -t -f DEVICE,CONNECTION dev status | grep "^$IF:" ;;
  *) echo "usage: $0 up|down"; exit 2 ;;
esac

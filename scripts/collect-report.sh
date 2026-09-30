#!/usr/bin/env bash
# Collect what a device report needs. Read-only. Paste the output in the issue.
# The "in the driver" line says whether ayaneo-leds knows this model:
# confirmed, untested (loads with untested=1) or not listed.
set -uo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

dmi() { cat "/sys/class/dmi/id/$1" 2>/dev/null || echo "?"; }

# Which table of ayaneo-leds.c lists this board_vendor and board_name.
driver_table() {
  awk -v entry="AYANEO_LEDS_DMI(\"$1\", \"$2\"," '
    /^static const struct dmi_system_id dmi_table\[\]/ { t = "confirmed" }
    /^static const struct dmi_system_id dmi_table_untested\[\]/ { t = "untested (load with untested=1)" }
    /^};/ { t = "" }
    t != "" && index($0, entry) { print t; found = 1; exit }
    END { if (!found) print "not listed" }' "$SRC/ayaneo-leds.c"
}

VENDOR="$(dmi board_vendor)"
BOARD="$(dmi board_name)"
KVER="$(uname -r)"
KMAJ="${KVER%%.*}"
KMIN="${KVER#*.}"
KMIN="${KMIN%%.*}"
KNOTE=""
if [[ "$KMAJ" =~ ^[0-9]+$ && "$KMIN" =~ ^[0-9]+$ ]] &&
   { [ "$KMAJ" -lt 6 ] || { [ "$KMAJ" -eq 6 ] && [ "$KMIN" -lt 10 ]; }; }; then
  KNOTE=" (too old: needs 6.10 or later)"
fi
HEADERS=no
{ [ -d "/lib/modules/$KVER/build" ] || [ -d "/usr/src/kernels/$KVER" ]; } && HEADERS=yes

echo '```'
echo "board_vendor:  $VENDOR"
echo "board_name:    $BOARD"
echo "sys_vendor:    $(dmi sys_vendor)"
echo "product_name:  $(dmi product_name)"
echo "bios_version:  $(dmi bios_version)"
[ -f "$SRC/ayaneo-leds.c" ] && echo "in the driver: $(driver_table "$VENDOR" "$BOARD")"
echo "kernel:        $KVER$KNOTE"
echo "headers:       $HEADERS"
echo "os:            $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-?}")"
echo "secure boot:   $(mokutil --sb-state 2>/dev/null | head -1 || echo '?')"
echo "loaded:        $(lsmod | awk '/^(ayaneo|led_class_multicolor)/{print $1}' | tr '\n' ' ')"
echo "leds:          $(for d in /sys/class/leds/*ayaneo*; do [ -e "$d" ] && printf '%s ' "${d##*/}"; done)"
if [ -r /sys/module/ayaneo_leds/parameters/untested ]; then
  echo "untested=      $(cat /sys/module/ayaneo_leds/parameters/untested)"
fi
# Tools that may also drive the rings during the test.
echo "services:      $(for s in inputplumber hhd plugin_loader; do
  systemctl list-units --all --no-legend "$s*" 2>/dev/null | grep -q ' active ' && printf '%s ' "$s"; done)"
echo "decky plugins: $(for d in "$HOME"/homebrew/plugins/*; do n="${d##*/}"
  case "${n,,}" in *huesync*|*ayadecky*) printf '%s ' "$n" ;; esac; done)"
echo "--- dmesg"
{ dmesg 2>/dev/null || sudo dmesg 2>/dev/null; } | grep -iE 'ayaneo' | tail -15
echo '```'

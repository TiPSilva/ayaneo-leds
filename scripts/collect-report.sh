#!/usr/bin/env bash
# Collect what a device report needs. Read-only. Paste the output in the issue.
set -uo pipefail

dmi() { cat "/sys/class/dmi/id/$1" 2>/dev/null || echo "?"; }

echo '```'
echo "board_vendor:  $(dmi board_vendor)"
echo "board_name:    $(dmi board_name)"
echo "product_name:  $(dmi product_name)"
echo "bios_version:  $(dmi bios_version)"
echo "kernel:        $(uname -r)"
echo "os:            $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-?}")"
echo "secure boot:   $(mokutil --sb-state 2>/dev/null | head -1 || echo '?')"
echo "loaded:        $(lsmod | awk '/^(ayaneo|led_class_multicolor)/{print $1}' | tr '\n' ' ')"
echo "leds:          $(ls /sys/class/leds 2>/dev/null | grep -i ayaneo | tr '\n' ' ')"
if [ -r /sys/module/ayaneo_leds/parameters/untested ]; then
  echo "untested=      $(cat /sys/module/ayaneo_leds/parameters/untested)"
fi
echo "--- dmesg"
sudo dmesg 2>/dev/null | grep -iE 'ayaneo' | tail -15
echo '```'

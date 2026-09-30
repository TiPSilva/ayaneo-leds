#!/usr/bin/env bash
# Build, sign and load ayaneo-leds until the next reboot. Touches nothing in
# /usr and adds no rpm-ostree layer. Run on the device, from the repo:
#
#   bash scripts/test-transient.sh              # build, sign, load
#   bash scripts/test-transient.sh --untested   # same, on a not yet confirmed model
#   bash scripts/test-transient.sh --unload     # unload (rings go back to the EC)
#
# With Secure Boot on, the module must be signed with an enrolled MOK key.
# Set MOKDIR to a directory with private_key.priv and public_key.der
# (default: /etc/ayaneo-leds/mok, else /etc/ayad/mok). See TESTING.md.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER="$(uname -r)"
KDIR="${KDIR:-/lib/modules/$KVER/build}"
[ -d "$KDIR" ] || KDIR="/usr/src/kernels/$KVER"
MOKDIR="${MOKDIR:-}"
PARAMS=""

case "${1:-}" in
  --unload)
    sudo modprobe -r ayaneo_leds 2>/dev/null || sudo rmmod ayaneo_leds
    echo ">> unloaded"
    exit 0 ;;
  --untested) PARAMS="untested=1" ;;
  "") ;;
  *) echo "unknown option: $1" >&2; exit 2 ;;
esac

if [ ! -d "$KDIR" ]; then
  echo "ERROR: kernel headers for $KVER not found (kernel-devel)." >&2
  exit 1
fi
if lsmod | grep -q '^ayaneo_platform'; then
  echo "ERROR: ayaneo_platform is loaded; both would drive the rings." >&2
  echo "       Unload it first (it may hang on unload), or reboot without it." >&2
  exit 1
fi
if lsmod | grep -q '^ayaneo_leds'; then
  echo "ERROR: ayaneo_leds is already loaded. Run with --unload first." >&2
  exit 1
fi
if [ -e /sys/class/leds/ayaneo:rgb:joystick_rings ]; then
  echo "ERROR: another driver already exposes ayaneo:rgb:joystick_rings." >&2
  exit 1
fi

# A private directory: root signs and loads what is built here.
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
cp "$SRC/ayaneo-leds.c" "$SRC/Makefile" "$BUILD/"
make -C "$BUILD" KDIR="$KDIR"

if mokutil --sb-state 2>/dev/null | grep -q enabled; then
  # The key directory is usually root-only, so check it as root.
  if [ -z "$MOKDIR" ]; then
    MOKDIR=/etc/ayaneo-leds/mok
    sudo test -r "$MOKDIR/public_key.der" || MOKDIR=/etc/ayad/mok
  fi
  if ! sudo test -r "$MOKDIR/public_key.der" || ! sudo test -r "$MOKDIR/private_key.priv"; then
    echo "ERROR: Secure Boot is on and there is no signing key in $MOKDIR." >&2
    echo "       See 'Secure Boot' in TESTING.md." >&2
    exit 1
  fi
  sudo "$KDIR/scripts/sign-file" sha256 "$MOKDIR/private_key.priv" \
    "$MOKDIR/public_key.der" "$BUILD/ayaneo-leds.ko"
fi

sudo modprobe led-class-multicolor
# shellcheck disable=SC2086
if ! sudo insmod "$BUILD/ayaneo-leds.ko" $PARAMS; then
  sudo dmesg | grep -i ayaneo | tail -5
  exit 1
fi
echo ">> loaded:"
ls /sys/class/leds/ | grep ayaneo
sudo dmesg | grep -i ayaneo-leds | tail -3 || true

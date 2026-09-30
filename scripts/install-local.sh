#!/usr/bin/env bash
# Install ayaneo-leds permanently on an rpm-ostree system (Bazzite, Fedora
# Atomic) until it ships as an akmod in the image. Builds and signs the module
# for the running kernel, wraps it in a small local RPM and layers it.
#
#   sudo bash scripts/install-local.sh          # install (active after reboot)
#   sudo bash scripts/install-local.sh --dry    # show what it would do
#   sudo bash scripts/install-local.sh --remove # uninstall (after reboot)
#
# The package is tied to the running kernel. After a kernel update the rings
# fall back to the EC until you run this again; system updates are never
# blocked by it. With Secure Boot on, it signs with an enrolled MOK key from
# MOKDIR (default: /etc/ayaneo-leds/mok, else /etc/ayad/mok). See TESTING.md.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RPM_NAME=ayaneo-leds-local
VERSION="$(sed -n 's/^MODULE_VERSION("\(.*\)");$/\1/p' "$SRC/ayaneo-leds.c")"
[ -n "$VERSION" ] || { echo "ERROR: no MODULE_VERSION in ayaneo-leds.c" >&2; exit 1; }
KVER="$(uname -r)"
KDIR="/usr/src/kernels/$KVER"
[ -d "$KDIR" ] || KDIR="/lib/modules/$KVER/build"
STATE=/var/lib/ayaneo-leds
BUILD="$STATE/build"
RPMDIR="$STATE/rpmbuild"

if [ -z "${MOKDIR:-}" ]; then
  MOKDIR=/etc/ayaneo-leds/mok
  [ -f "$MOKDIR/public_key.der" ] || MOKDIR=/etc/ayad/mok
fi

DRY=0
REMOVE=0
for arg in "$@"; do
  case "$arg" in
    --dry) DRY=1 ;;
    --remove) REMOVE=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

run() {
  if [ "$DRY" = 1 ]; then printf '  [dry] %s\n' "$*"; else "$@"; fi
}

# Local packages of the deployments waiting for a reboot. They come before
# the booted one; the rollback deployments after it are left out.
pending_local_packages() {
  rpm-ostree status --json | python3 -c '
import json, sys
for d in json.load(sys.stdin)["deployments"]:
    if d.get("booted"):
        break
    print("\n".join(d.get("requested-local-packages", [])))
'
}

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
command -v rpm-ostree >/dev/null || { echo "this is for rpm-ostree systems" >&2; exit 1; }

if [ "$REMOVE" = 1 ]; then
  run rpm-ostree uninstall --idempotent "$RPM_NAME"
  run rm -rf "$STATE"
  echo ">> removed; reboot to apply"
  exit 0
fi

echo ">> device: $(cat /sys/class/dmi/id/board_vendor) / $(cat /sys/class/dmi/id/board_name)"

if modinfo ayaneo_platform >/dev/null 2>&1; then
  echo "ERROR: ayaneo-platform is installed; both would drive the rings. Remove it first." >&2
  exit 1
fi

for bin in gcc make rpmbuild; do
  command -v "$bin" >/dev/null || { echo "ERROR: '$bin' is missing (rpm-ostree install rpm-build gcc make)" >&2; exit 1; }
done
[ -d "$KDIR" ] || { echo "ERROR: kernel headers for $KVER not found" >&2; exit 1; }

RELEASE="$(echo "$KVER" | tr '.-' '_')"
if rpm -q "$RPM_NAME" >/dev/null 2>&1; then
  CUR="$(rpm -q --qf '%{VERSION}-%{RELEASE}' "$RPM_NAME")"
  if [ "$CUR" = "$VERSION-$RELEASE" ]; then
    echo ">> $RPM_NAME $CUR is already installed for this kernel"
    exit 0
  fi
  echo ">> installed: $CUR; rebuilding for $KVER"
fi
case "$(pending_local_packages)" in
  *"$RPM_NAME-$VERSION-$RELEASE"*)
    echo ">> $RPM_NAME $VERSION-$RELEASE is already staged; reboot to apply"
    exit 0 ;;
esac

echo ">> building for $KVER"
run rm -rf "$BUILD"
run install -d -m 0755 "$BUILD"
run cp "$SRC/ayaneo-leds.c" "$SRC/Makefile" "$BUILD/"
run make -C "$BUILD" KDIR="$KDIR"

if mokutil --sb-state 2>/dev/null | grep -q enabled; then
  [ -f "$MOKDIR/private_key.priv" ] && [ -f "$MOKDIR/public_key.der" ] || {
    echo "ERROR: Secure Boot is on and there is no key in $MOKDIR (see TESTING.md)" >&2; exit 1; }
  MOK_TEST="$(mokutil --test-key "$MOKDIR/public_key.der" 2>&1 || true)"
  printf '%s' "$MOK_TEST" | grep -qi "already enrolled" || {
    echo "ERROR: the key in $MOKDIR is not enrolled (mokutil --import, then reboot)" >&2; exit 1; }
  echo ">> signing with $MOKDIR"
  run "$KDIR/scripts/sign-file" sha256 "$MOKDIR/private_key.priv" \
    "$MOKDIR/public_key.der" "$BUILD/ayaneo-leds.ko"
fi

echo ">> packaging"
run install -d -m 0755 "$RPMDIR/SPECS" "$RPMDIR/RPMS"
SPEC="$RPMDIR/SPECS/$RPM_NAME.spec"
if [ "$DRY" = 0 ]; then
  # No stripping or debuginfo: either would break the module signature.
  cat > "$SPEC" <<EOF
%global debug_package %{nil}
%global __os_install_post %{nil}
Name: $RPM_NAME
Version: $VERSION
Release: $RELEASE
Summary: ayaneo-leds joystick ring RGB driver, local build for $KVER
License: GPL-2.0-or-later
BuildArch: x86_64
Conflicts: ayaneo-platform
%description
Local build of ayaneo-leds for kernel $KVER, until it ships as an akmod.
No dependency on the kernel version, so it never blocks system updates;
after a kernel update, run scripts/install-local.sh again.
%install
install -D -m 0644 "$BUILD/ayaneo-leds.ko" %{buildroot}/usr/lib/modules/$KVER/extra/ayaneo-leds.ko
%files
/usr/lib/modules/$KVER/extra/ayaneo-leds.ko
%post
depmod -a $KVER || :
%changelog
* $(LC_ALL=C date "+%a %b %d %Y") ayaneo-leds install-local - $VERSION-$RELEASE
- Local build for $KVER
EOF
fi
run rm -f "$RPMDIR/RPMS/x86_64/$RPM_NAME-"*.rpm
run rpmbuild --quiet --define "_topdir $RPMDIR" -bb "$SPEC"
RPM_FILE="$RPMDIR/RPMS/x86_64/$RPM_NAME-$VERSION-$RELEASE.x86_64.rpm"

if rpm -q "$RPM_NAME" >/dev/null 2>&1; then
  echo ">> replacing the old build in one transaction"
  run rpm-ostree install --idempotent --uninstall "$RPM_NAME" "$RPM_FILE"
else
  echo ">> layering $RPM_FILE"
  run rpm-ostree install --idempotent "$RPM_FILE"
fi

echo
echo ">> done. Reboot; then check: ls /sys/class/leds | grep ayaneo"

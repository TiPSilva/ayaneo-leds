# ayaneo-leds

Linux driver for the joystick ring RGB LEDs of Ayaneo handhelds that use the
legacy EC interface. It exposes three multicolor LEDs:

```
/sys/class/leds/ayaneo:rgb:joystick_rings/        both rings, one color (3 channels)
    multi_intensity   # "R G B", 0-255 each
    brightness        # 0-255; writing it pushes the colour to the EC
    ec_control        # write 1: hand the rings back to the EC animation
/sys/class/leds/ayaneo:rgb:joystick_ring_left/    4 zones x RGB (12 channels)
/sys/class/leds/ayaneo:rgb:joystick_ring_right/   4 zones x RGB (12 channels)
```

Zone order (measured on the AYANEO 2S, same on both rings): 0 right, 1 bottom,
2 left, 3 top, i.e. clockwise from the right.

```bash
L=/sys/class/leds/ayaneo:rgb:joystick_rings
echo "0 0 255" | sudo tee $L/multi_intensity; echo 255 | sudo tee $L/brightness
R=/sys/class/leds/ayaneo:rgb:joystick_ring_right   # right ring: red, green, blue, white zones
echo "255 0 0 0 255 0 0 0 255 255 255 255" | sudo tee $R/multi_intensity; echo 255 | sudo tee $R/brightness
```

The last LED written wins. While writes keep coming, only the channels that
changed are sent to the EC, so userspace effects (breathe, radar, ...) that
rewrite the rings several times per second stay cheap.

## Supported models

| Model | Status |
|---|---|
| AYANEO 2S | confirmed (BIOS 2.15_S20, kernel 7.2) |
| AYANEO 2, GEEK, GEEK 1S, AIR, AIR Pro, AIR 1S, AIR 1S Limited, AIR Plus (Mendocino), SuiPlay0X1 | same interface per ayaneo-platform; load with `untested=1` — **testers wanted**, see [TESTING.md](TESTING.md) |
| KUN, AIR Plus AMD, Slide | not supported (different LED layout or EC interface) |

Have one of the untested models? A test takes about 15 minutes, installs nothing and is undone by a
reboot: see [TESTING.md](TESTING.md).

## Behaviour

- Loading the driver does not touch the rings. The EC keeps its own animation
  (charging, battery) until userspace sets a colour. Writing `multi_intensity`
  applies it right away with the current `brightness`, which is 0 after
  loading, so write `brightness` as well.
- On suspend, unload and shutdown the rings go back to the EC; if nothing set a
  colour, unloading leaves them as they are. On resume the last colours set by
  userspace come back. After `ec_control`, the EC may keep the rings dark until
  its next power event (plugging or unplugging the charger).
- The EC may redraw the rings when the charger is plugged or unplugged, so the
  driver takes them back 2 seconds later.
- The first write after 2 seconds without writes resends every channel. If
  another tool wrote the EC directly, writing the same colour again repairs the
  rings.
- The LED core runs the EC writes from its own workqueue, so a sysfs write
  returns before the EC is updated, and errors only show up in `dmesg`.

## Relation to other drivers

- **ayaneo-ec** (mainline): handles fan and charge control through the same
  ACPI EC interface, on different registers. Both can be loaded together.
- **ayaneo-platform** (out of tree): drives the same LED registers. Do not load
  both. This driver only covers the legacy EC models; the AIR Plus AMD and the
  Slide use a different interface and are not handled here.

## Building

Needs kernel 6.10 or later.

```bash
make                    # against the running kernel
make KDIR=/path/to/kernel/build
```

An akmod package for [Terra](https://terra.fyralabs.com/) is in progress. Until it is available, on Bazzite / Fedora
Atomic with an AYANEO 2S, `sudo bash scripts/install-local.sh` layers a local build for the running kernel (rerun it
after a kernel update). With Secure Boot on, it signs the module with an enrolled MOK key; see
[Secure Boot](TESTING.md#secure-boot) in TESTING.md. `cat /sys/module/ayaneo_leds/version` shows the version loaded.

## License

GPL-2.0-or-later. The register protocol was documented by
[ayaneo-platform](https://github.com/ShadowBlip/ayaneo-platform).

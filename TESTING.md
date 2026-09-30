# Testing ayaneo-leds on your Ayaneo

Thanks for helping! Only the **AYANEO 2S** is confirmed so far. These models
use the same EC interface according to
[ayaneo-platform](https://github.com/ShadowBlip/ayaneo-platform) and are
waiting for someone to test them:

| Model | `board_name` |
|---|---|
| AYANEO 2 | `AYANEO 2` |
| AYANEO GEEK | `GEEK` |
| AYANEO GEEK 1S | `GEEK 1S` |
| AYANEO AIR | `AIR` |
| AYANEO AIR Pro | `AIR Pro` |
| AYANEO AIR 1S | `AIR 1S` |
| AYANEO AIR 1S Limited | `AIR 1S Limited` |
| AYANEO AIR Plus (Mendocino) | `AB05-Mendocino` |
| SuiPlay0X1 | `SuiPlay0X1` |

Not covered: the KUN, the AIR Plus AMD (`AB05-AMD`) and the Slide (`AS01`).
Please don't force the driver onto them.

Check yours with `cat /sys/class/dmi/id/board_name`.

## What the test does (and doesn't)

- The module is built and loaded **until the next reboot**. Nothing is
  installed, and on immutable systems (Bazzite, SteamOS…) no layer is added.
- It only writes the LED registers of the EC, and only when you set a colour.
  Unloading, suspending or rebooting hands the rings back to the EC.
- Worst case on an unconfirmed model: the rings show the wrong thing or stay
  dark until the next power event (plugging or unplugging the charger).

## Before you start

- Kernel headers for the running kernel (`kernel-devel` on Fedora/Bazzite;
  Bazzite ships them under `/usr/src/kernels`), `gcc` and `make`.
- Nothing else controlling the rings: no `ayaneo-platform` module, and turn
  off the LED control in HHD, AyaDecky or similar tools for the test.
- **Secure Boot:** check with `mokutil --sb-state`. If it's enabled, see
  [Secure Boot](#secure-boot) first.

## Steps

```bash
git clone https://github.com/TiPSilva/ayaneo-leds.git
cd ayaneo-leds
bash scripts/test-transient.sh --untested
```

Three LEDs should show up (`ayaneo:rgb:joystick_rings`,
`ayaneo:rgb:joystick_ring_left` and `ayaneo:rgb:joystick_ring_right`), and the
rings should **not** change yet.

### Colours and brightness

```bash
L=/sys/class/leds/ayaneo:rgb:joystick_rings
echo "255 0 0" | sudo tee $L/multi_intensity; echo 255 | sudo tee $L/brightness   # red
echo "0 255 0" | sudo tee $L/multi_intensity; echo 255 | sudo tee $L/brightness   # green
echo "0 0 255" | sudo tee $L/multi_intensity; echo 255 | sudo tee $L/brightness   # blue
echo 1   | sudo tee $L/brightness      # very dim, but still lit
echo 255 | sudo tee $L/brightness      # back to full
```

Look at **both rings** each time: are all four segments of each ring lit, are
the colours right, do both rings look equally bright?

### Zones

Each ring also has its own LED with four zones. This paints the zones of the
right ring red, green, blue and white, then does the same on the left ring:

```bash
RIGHT=/sys/class/leds/ayaneo:rgb:joystick_ring_right
echo "255 0 0 0 255 0 0 0 255 255 255 255" | sudo tee $RIGHT/multi_intensity; echo 255 | sudo tee $RIGHT/brightness
LEFT=/sys/class/leds/ayaneo:rgb:joystick_ring_left
echo "255 0 0 0 255 0 0 0 255 255 255 255" | sudo tee $LEFT/multi_intensity; echo 255 | sudo tee $LEFT/brightness
```

On the AYANEO 2S the zones go clockwise from the right: red on the right,
green at the bottom, blue on the left and white at the top, on both rings.
Note where each colour shows up on yours.

### Charger, suspend and handing back

With a colour set, plug the charger in, wait a few seconds and unplug it: each
time the colour should come back within about 2 seconds.

Suspend and wake the device: the colour should come back by itself after a few
seconds.

Hand the rings back to the EC:

```bash
echo 1 | sudo tee /sys/class/leds/ayaneo:rgb:joystick_rings/ec_control
```

The EC's own animation should come back, possibly only after the next time you
plug or unplug the charger. Finally:

```bash
bash scripts/test-transient.sh --unload
bash scripts/collect-report.sh
```

Open an issue with the **Device report** template and paste the output of
`collect-report.sh`. Reports that something *doesn't* work are just as useful.

## Secure Boot

A self-built module won't load with Secure Boot on unless it's signed with a
key the firmware trusts. Either disable Secure Boot for the test, or create
and enroll your own key once:

```bash
sudo install -d -m 0700 /etc/ayaneo-leds/mok
sudo openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
  -subj "/CN=ayaneo-leds local signing key/" \
  -keyout /etc/ayaneo-leds/mok/private_key.priv \
  -outform DER -out /etc/ayaneo-leds/mok/public_key.der
sudo mokutil --import /etc/ayaneo-leds/mok/public_key.der   # asks for a one-time password
```

Reboot, choose **Enroll MOK** in the blue screen, and type the password. Then
follow the steps above as they are: `test-transient.sh` and `install-local.sh`
find the key in `/etc/ayaneo-leds/mok` by themselves. For a key somewhere
else, set `MOKDIR` to its directory.

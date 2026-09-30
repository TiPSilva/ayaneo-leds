# Testing ayaneo-leds on your AYANEO

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

A test takes about 15 minutes. Each model confirmed by a report loads by itself
for everyone from the next release on.

## What the test does (and doesn't)

- The module is built and loaded **until the next reboot**. Nothing is
  installed, and on immutable systems (Bazzite, Fedora Atomic) no layer is
  added.
- It only writes the LED registers of the EC, and only when you set a colour.
  Unloading, suspending or rebooting hands the rings back to the EC.
- Worst case on an unconfirmed model: the rings show the wrong thing or stay
  dark until the next power event (plugging or unplugging the charger).

## 1. Get the code

```bash
git clone https://github.com/TiPSilva/ayaneo-leds.git
cd ayaneo-leds
```

Without git:

```bash
curl -L https://github.com/TiPSilva/ayaneo-leds/archive/refs/heads/main.tar.gz | tar xz
cd ayaneo-leds-main
```

## 2. Check your model

```bash
bash scripts/collect-report.sh
```

Look at the `in the driver:` line:

- `untested (load with untested=1)`: this guide is for you.
- `confirmed`: your model already works; no test needed.
- `not listed`: the driver doesn't know your board, so don't force it. Open a
  [Device report](https://github.com/TiPSilva/ayaneo-leds/issues/new?template=device-report.md)
  with that output anyway: the board strings show whether it's a known model
  under another name.

Also check that `kernel:` doesn't say "too old" (the driver needs 6.10 or
later) and that `headers:` says `yes`.

## 3. Before you start

**Kernel headers**, `gcc` and `make` for the running kernel:

| System | Command |
|---|---|
| Bazzite | already installed |
| Fedora, Nobara | `sudo dnf install kernel-devel-$(uname -r) gcc make` |
| Arch, CachyOS | `sudo pacman -S --needed base-devel` plus the headers of your kernel (`linux-headers`, `linux-zen-headers`, `linux-cachyos-headers`…) |
| Debian, Ubuntu | `sudo apt install build-essential linux-headers-$(uname -r)` |
| SteamOS, ChimeraOS | not covered by this guide (read-only system) |

**Nothing else controlling the rings:** no `ayaneo-platform` module, and turn
off the LED control in HHD, HueSync and AyaDecky for the test. HueSync's
effects write the EC directly and would mix with the test. InputPlumber, the
default on Bazzite, can stay on. The `services:` and `decky plugins:` lines of
the report show what is running.

**Secure Boot:** check with `mokutil --sb-state`. If it's enabled, see
[Secure Boot](#secure-boot) first.

## 4. Load the driver

```bash
bash scripts/test-transient.sh --untested
```

It asks for your password (sudo). Three LEDs should show up
(`ayaneo:rgb:joystick_rings`, `ayaneo:rgb:joystick_ring_left` and
`ayaneo:rgb:joystick_ring_right`), and the rings should **not** change yet.

If it doesn't load:

- **`No such device`**: the driver doesn't know this board. Check step 2.
- **`Key was rejected by service`**: Secure Boot is on and the module isn't
  signed with an enrolled key. See [Secure Boot](#secure-boot).
- **`Invalid module format`, or the build fails**: the headers don't match the
  running kernel. After a kernel update, reboot first; then reinstall the
  headers.
- **`ayaneo_platform is loaded`**: reboot without it.

A report about a failure is just as useful: skip to step 6.

## 5. Test

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
Note where each colour shows up on yours. A photo helps.

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
plug or unplug the charger.

## 6. Unload and report

```bash
bash scripts/test-transient.sh --unload
bash scripts/collect-report.sh
```

Open a [Device report](https://github.com/TiPSilva/ayaneo-leds/issues/new?template=device-report.md),
paste the output of `collect-report.sh` and tick what worked. Reports that
something *doesn't* work are just as useful.

## What happens next

- **Everything worked:** the model moves to the confirmed list in the next
  release and loads by itself from then on. You're credited in the commit
  (`Tested-by:`) if you want; the report asks how.
- **Something was off**, like one ring brighter than the other or the zones in
  another order: I may ask you to test a build adjusted for your model.
- **Nothing lit up, or something else reacted:** the model comes off the list.
  That report helps just as much.

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

# Installing the Goodix `27c6:55a2` fingerprint driver

This guide sets up the `goodixtls55x4` driver for the **Goodix `27c6:55a2`**
sensor so you can **log in / unlock with your finger** through `fprintd` + PAM.

It was written and verified on **openSUSE Tumbleweed, KDE Plasma (Wayland) +
SDDM**, with `fprintd 1.94.5` and the system `libfprint-2-tod1 1.94.10`. Adapt
package names for other distros.

> **Result:** enroll + verify work on hardware — the right finger matches
> (bozorth score ~31–87), other fingers are rejected (≤14, threshold 24).
> KDE screen-unlock and `sudo` by fingerprint both work, with the password
> always available as a fallback.

---

## How this integrates (important)

The system `fprintd` normally loads the distro's **TOD** build of libfprint,
whose only Goodix module is `53xc` — it does **not** drive the `55a2`. Instead of
replacing the system libfprint, we:

1. Build **this** libfprint (the `55a2` driver is compiled in).
2. Install it into an **isolated** directory (`/opt/fprint55a2/lib`) so it
   affects nothing else on the system.
3. Add a tiny **systemd drop-in** so only the `fprintd` daemon loads our
   libfprint via `LD_LIBRARY_PATH`.
4. Enroll a finger and let PAM use it.

Everything here is reversible (see [Uninstall](#uninstall)).

---

## 1. Build dependencies (openSUSE)

```bash
sudo zypper install -t pattern devel_basis
sudo zypper install meson ninja gcc git \
  glib2-devel libgusb-devel libgudev-1_0-devel \
  pixman-devel nss-devel openssl-devel \
  libgnutls-devel cairo-devel
```

You also need `fprintd` and the PAM module (usually already present):

```bash
sudo zypper install fprintd fprintd-pam
```

## 2. Build libfprint with the driver

From the root of this repository:

```bash
meson setup build \
  -Ddrivers=goodixtls55x4 \
  -Ddoc=false -Dintrospection=false \
  -Dinstalled-tests=false -Dgtk-examples=false
ninja -C build
```

This produces `build/libfprint/libfprint-2.so.2.0.0` with the `55a2` driver
built in.

### Optional: test standalone before installing

```bash
cd build/examples
sudo ./enroll        # follow the swipe prompts (see swipe technique below)
sudo ./verify        # should report a match
```

## 3. Install the library (isolated)

```bash
sudo install -d /opt/fprint55a2/lib
sudo install -m0755 build/libfprint/libfprint-2.so.2.0.0 /opt/fprint55a2/lib/
sudo ln -sf libfprint-2.so.2.0.0 /opt/fprint55a2/lib/libfprint-2.so.2
sudo ln -sf libfprint-2.so.2     /opt/fprint55a2/lib/libfprint-2.so
```

> We deliberately use `/opt/...` (not a directory in `ld.so.conf`) so this
> library is **only** picked up by `fprintd`, never globally.

## 4. Point fprintd at our library

```bash
sudo install -d /etc/systemd/system/fprintd.service.d
sudo tee /etc/systemd/system/fprintd.service.d/override.conf >/dev/null <<'EOF'
[Service]
Environment=LD_LIBRARY_PATH=/opt/fprint55a2/lib
EOF
sudo systemctl daemon-reload
sudo systemctl stop fprintd 2>/dev/null   # next D-Bus call restarts it with our lib
```

Verify the daemon now sees the device:

```bash
fprintd-list "$USER"          # may say "no fingers enrolled" — that's fine
# Confirm it loaded our lib:
pid=$(pgrep -x fprintd); sudo grep -a fprint55a2 /proc/$pid/maps | head -1
```

You should see `/opt/fprint55a2/lib/libfprint-2.so.2.0.0` in the maps.

## 5. Enroll your finger

Enroll as **your own user** (no `sudo`, or it enrolls `root` instead):

```bash
fprintd-enroll
```

Swipe **6 times** until you see `enroll-completed`. **Do not Ctrl-C** before it
finishes, or the device stays claimed (fix: `sudo systemctl restart fprintd`).

### Swipe technique (matters a lot)

The sensor is tiny and treated as a **swipe** sensor:

- Place the finger flat at **one end** of the sensor, light, even pressure.
- Move **slowly and continuously** across the whole long axis (~1–1.5 s).
- **Lift only at the other end.**
- Too fast / too short → `swipe too short, waiting for another`; just redo it.

## 6. Enable fingerprint unlock

**KDE screen-unlock works automatically** once a finger is enrolled — the
`kde-fingerprint` PAM service already references `pam_fprintd.so`. Lock with
`Meta+L` and swipe.

**`sudo` by fingerprint** — insert one line at the top of the *existing*
`/etc/pam.d/sudo`, don't blindly overwrite the whole file: the include names
differ by distro (Debian/openSUSE use `common-auth`/`common-account`/
`common-password`/`common-session-nonlogin`; **Arch uses `system-auth`**).
Always back up first and confirm what your distro's file actually contains:

```bash
sudo cp /etc/pam.d/sudo /etc/pam.d/sudo.orig   # backup, always
cat /etc/pam.d/sudo                            # see what your distro ships
```

On **Arch Linux**, the stock file is:

```
#%PAM-1.0
auth		include		system-auth
account		include		system-auth
session		include		system-auth
session		optional	pam_systemd.so class=none
```

so add a `pam_fprintd.so` line before the `system-auth` include instead of
replacing the file:

```bash
sudo tee /etc/pam.d/sudo >/dev/null <<'EOF'
#%PAM-1.0
auth		sufficient	pam_fprintd.so
auth		include		system-auth
account		include		system-auth
session		include		system-auth
session		optional	pam_systemd.so class=none
EOF
```

(On Debian/Ubuntu/openSUSE, replace the `system-auth` includes above with
`common-auth`/`common-account`/`common-password`+`common-session-nonlogin` —
whatever `cat /etc/pam.d/sudo` showed you before you touched it.)

Test: `sudo -k && sudo echo ok` → it should ask you to swipe.

> **Do NOT add `pam_fprintd` to `system-auth`/`common-auth`.** `sshd`, `su`,
> etc. include it, and a remote `ssh` login would then hang waiting for a
> finger swipe. Always add it to the specific service file you want (`sudo`,
> `sddm`, …), never to the shared base include.

For the **SDDM login screen**, on Arch `/etc/pam.d/sddm` is already a real
(non-symlink) file installed by the `sddm` package — back it up
(`sudo cp /etc/pam.d/sddm /etc/pam.d/sddm.orig`) then add
`auth sufficient pam_fprintd.so` as the first `auth` line (optional; the lock
screen already works via `kde-fingerprint`, see below).

---

## Troubleshooting

### "Invalid device PSK" after dual-booting Windows
Booting Windows **re-provisions the sensor's PSK**, which breaks our TLS
handshake (`fprintd` fails activation with `Invalid device PSK: 0x23dce6...`).
The fix is to re-write the factory all-zero/whitebox PSK (provisioning only, no
firmware erase) using the **goodix-fp-dump** tooling:

```bash
git clone https://github.com/Ravira43/goodix-fp-dump.git
cd goodix-fp-dump
python3 -m venv .venv
.venv/bin/pip install pyusb
sudo systemctl stop fprintd
sudo .venv/bin/python restore_psk_55a2.py   # writes PSK_WHITE_BOX, verifies hash
sudo systemctl start fprintd
```
After it reports the hash is back to `81b8ff49...`, fingerprint works again.
You must re-run this after every Windows boot.

### Capture times out / sensor stops detecting the finger (FDT degradation)
After many capture cycles the finger-detect (FDT) can stop firing. A USB reset
clears it:

```bash
sudo systemctl stop fprintd
sudo python3 -c "import usb.core,time; d=usb.core.find(idVendor=0x27c6,idProduct=0x55a2); d.reset(); time.sleep(2)"
sudo systemctl start fprintd
```
A full reboot clears deeper degradation. The driver also has an opt-in
`GOODIX_USB_RESET=1` env var that resets on each activation (default off; adds
~2 s per scan).

### "Device was already claimed"
A capture was interrupted (Ctrl-C) and left the device claimed:
`sudo systemctl restart fprintd`.

### The LED never turns orange
Cosmetic only — the visible LED colour is **not** controlled by the `SetLed`
(`0xc6`) payload on this firmware (verified by sweeping all values). It does not
affect matching.

---

## Uninstall

```bash
sudo mv /etc/pam.d/sudo.orig /etc/pam.d/sudo               # restore your backup
sudo rm /etc/systemd/system/fprintd.service.d/override.conf
sudo systemctl daemon-reload && sudo systemctl restart fprintd
sudo rm -rf /opt/fprint55a2
fprintd-delete "$USER"                                    # remove enrolled prints
```

If you didn't keep the `.orig` backup, reinstall the vendor package's version
instead (e.g. `sudo pacman -S --overwrite /etc/pam.d/sudo sudo` on Arch) rather
than leaving `/etc/pam.d/sudo` deleted — an absent PAM file makes `sudo` fail
authentication for everyone until it's restored.

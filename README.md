

<div align="center">

# LibFPrint

*LibFPrint is part of the **[FPrint][Website]** project.*

<br/>

[![Button Website]][Website]
[![Button Documentation]][Documentation]

[![Button Supported]][Supported]
[![Button Unsupported]][Unsupported]

[![Button Contribute]][Contribute]
[![Button Contributors]][Contributors]

</div>

## This fork: Goodix 27c6:55a2 driver

This is an experimental libfprint fork adding a working driver for the **Goodix
`27c6:55a2`** fingerprint sensor (firmware `GF3208_RTSEC_APP_10062`), via the
`goodixtls55x4` driver.

> **Vibe-coded.** This fork, like the upstream fork it builds on, was written
> with heavy LLM assistance rather than hand-audited line by line by an
> experienced C/USB developer. It works on the hardware it's been tested on,
> but treat it accordingly — see [Audit this yourself](#audit-this-yourself)
> before you run any of it as root.

**Status — enroll + verify working on hardware:** the right finger matches
(bozorth score ~27–87), other fingers are rejected (≤14, threshold 24).
Verified end-to-end through `fprintd` + PAM: **KDE screen-unlock and `sudo` by
fingerprint both work**, confirmed on a real Arch Linux install (see
[Audit this yourself](#audit-this-yourself) and [Install log](#install-log-arch-linux)
below).

### 👉 [Installation guide: INSTALL_55a2.md](INSTALL_55a2.md)

Step-by-step build + isolated install + `fprintd`/PAM setup, plus
troubleshooting (Windows dual-boot PSK reset, FDT recovery, swipe technique).
Originally written for openSUSE/KDE; now also covers Arch Linux's PAM layout
(see fixes below).

How it works: TLS-PSK encrypted capture, treats the tiny 56×176 sensor as a
**swipe** sensor — streams frames during the swipe, per-pixel background
subtraction against the calibration frame (removes the sensor fixed-pattern),
keeps only distinct frames and stacks them **edge-to-edge** into a tall,
minutiae-rich image that NBIS/bozorth matches. A true single-tap unlock (like
the Windows driver) isn't currently possible — the vendor's tap-capable
matching is proprietary and undocumented; this driver's open alternative
(`GOODIX_SWIPE_FDT=1`, static-frame capture) exists but is a known-worse
fallback, kept mainly for reference.

Device coverage of this branch's Goodix TLS drivers:

| USB ID | Driver | Status |
| --- | --- | --- |
| `27c6:55a2` | `goodixtls55x4` | supported & tested (enroll + verify) |
| `27c6:55b4` | `goodixtls55x4` | supported & tested (upstream) |
| `27c6:55a4` | `goodixtls55x4` | unsupported — try at your own risk |
| `27c6:5110` | `goodixtls511` | unrelated, separate pre-existing driver (80×64) |

> Known follow-ups: FDT (finger-detection) can stop firing after many cycles —
> a USB `dev.reset()` restores it; single-tap capture is unimplemented (see
> above); broader hardware validation is still open.

## Audit this yourself

Don't take anyone's word for it, including this README — this fork (and the
companion [`goodix-fp-dump`](https://github.com/Ravira43/goodix-fp-dump),
used for `restore_psk_55a2.py`) runs as root and talks directly to USB
hardware. Before building or installing, check it yourself:

**1. Diff against real upstream** — see everything that changed beyond the
driver this fork claims to add:
```bash
git remote add upstream https://gitlab.freedesktop.org/libfprint/libfprint.git
git fetch upstream
git diff $(git merge-base HEAD upstream/master) HEAD --stat
```
Read through any file outside `libfprint/drivers/goodixtls/` and
`INSTALL_55a2.md` — that's scope beyond "adds a 55a2 driver".

**2. Grep the new driver code for anything that shouldn't be there** — a USB
fingerprint driver has no business spawning processes, opening real network
sockets, or touching files outside a debug dir you opted into:
```bash
grep -rnE 'system\(|popen\(|exec[lv]|fork\(|socket\(|connect\(|curl|wget|base64' \
  libfprint/drivers/goodixtls/
```
Anything that shows up needs an explanation in a comment nearby, or it's a
red flag.

**3. Read `restore_psk_55a2.py` and what it imports** — it's short (~30
lines) and only calls `check_psk()`/`write_psk()`. Confirm the USB device
lookup is pinned to `idVendor=0x27c6, idProduct=0x55a2` (in `protocol.py`),
not a wildcard, and that nothing it actually calls spawns a subprocess or
opens a socket (some *other*, unused functions in `driver_55x4.py` do —
check they're not on the call path).

**4. Check the build files don't fetch anything** — `meson.build` should have
no `subproject()`/`.wrap` files and no `run_command()` pulling from the
network:
```bash
find . -iname '*.wrap' -o -iname subprojects
grep -rn 'run_command\|subproject' meson.build libfprint/meson.build
```

**5. Compare `INSTALL_55a2.md` against what actually touches your system** —
it should be the only place describing changes outside `/opt`: a systemd
drop-in for `fprintd`, and an opt-in PAM edit for `sudo`. Nothing here should
touch sudoers, cron, or udev rules:
```bash
grep -rlE '\.rules$|sudoers|cron' . --include='*.md' --include='*.c'
```

If any of these turn up something INSTALL_55a2.md doesn't mention, stop and
ask before running it as root.

## Fixes applied (this pass)

1. **`libfprint/sigfm/meson.build`** — the new `sigfm` (OpenCV feature-match)
   subsystem hard-required `opencv4` via pkg-config/cmake, which doesn't exist
   on distros that have moved to OpenCV 5 (e.g. current Arch). Build now tries
   `opencv4` then falls back to `opencv5`. Pure build-config change; the
   `sigfm` C++ code itself was reviewed during the audit and found to be
   inert unless a driver explicitly opts into it (none currently do).
2. **`INSTALL_55a2.md`** — the `sudo`-by-fingerprint step used to *replace*
   `/etc/pam.d/sudo` wholesale with Debian/openSUSE-style
   `common-auth`/`common-account`/`common-password`/`common-session-nonlogin`
   includes. On Arch (and other distros using `system-auth`) this silently
   breaks `sudo`'s PAM stack. The guide now: backs up the original file first,
   shows you how to check what your distro actually ships, and *inserts* the
   `pam_fprintd.so` line ahead of whatever include your distro already uses,
   with an explicit Arch example. The uninstall step now restores the backup
   instead of deleting the file outright (deleting it left `sudo`
   authentication broken for everyone until the package was reinstalled).
3. **`.gitignore`** — added `*.variant`. The standalone example programs
   (`build/examples/enroll` etc.) write a local `test-storage.variant` file
   containing raw enrolled fingerprint template data into the working
   directory; it wasn't previously ignored and could have been committed by
   accident.

## Install log (Arch Linux)

Walked through end-to-end on a real machine with the `27c6:55a2` device
present: `meson setup` + `ninja` build → hit the `opencv4` issue above, fixed
→ standalone `examples/enroll` hit `Invalid device PSK` (factory-provisioned
key, expected on first use per the troubleshooting section) → ran
`restore_psk_55a2.py`, hash restored to `81b8ff49...` → standalone enroll and
`fprintd-enroll` both succeeded → `pam_fprintd.so` wired into `/etc/pam.d/sudo`
per the corrected steps above → verified with `fprintd-verify`
(`score 27/24`, genuine match) and live `sudo -k && sudo <cmd>` (prompts for a
swipe, falls back to password). KDE lock-screen unlock worked without any
extra PAM changes (`kde-fingerprint` already references `pam_fprintd.so` on
Arch out of the box).

## History

**LibFPrint** was originally developed as part of an
academic project at the **[University Of Manchester]**.

It aimed to hide the differences between consumer
fingerprint scanners and provide a single uniform
API to application developers.

## Goal

The ultimate goal of the **FPrint** project is to make
fingerprint scanners widely and easily usable under
common Linux environments.

## License

`Section 6` of the license states that for compiled works that use
this library, such works must include **LibFPrint** copyright notices
alongside the copyright notices for the other parts of the work.

**LibFPrint** includes code from **NIST's** **[NBIS]** software distribution.

We include **Bozorth3** from the **[US Export Controlled]**
distribution, which we have determined to be fine
being shipped in an open source project.

<br/>

<div align="right">

[![Badge License]][License]

</div>


<!----------------------------------------------------------------------------->

[Documentation]: https://fprint.freedesktop.org/libfprint-dev/
[Contributors]: https://gitlab.freedesktop.org/libfprint/libfprint/-/graphs/master
[Unsupported]: https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices
[Supported]: https://fprint.freedesktop.org/supported-devices.html
[Website]: https://fprint.freedesktop.org/

[Contribute]: ./HACKING.md
[License]: ./COPYING

[University Of Manchester]: https://www.manchester.ac.uk/
[US Export Controlled]: https://fprint.freedesktop.org/us-export-control.html
[NBIS]: http://fingerprint.nist.gov/NBIS/index.html


<!---------------------------------[ Badges ]---------------------------------->

[Badge License]: https://img.shields.io/badge/License-LGPL2.1-015d93.svg?style=for-the-badge&labelColor=blue


<!---------------------------------[ Buttons ]--------------------------------->

[Button Documentation]: https://img.shields.io/badge/Documentation-04ACE6?style=for-the-badge&logoColor=white&logo=BookStack
[Button Contributors]: https://img.shields.io/badge/Contributors-FF4F8B?style=for-the-badge&logoColor=white&logo=ActiGraph
[Button Unsupported]: https://img.shields.io/badge/Unsupported_Devices-EF2D5E?style=for-the-badge&logoColor=white&logo=AdBlock
[Button Contribute]: https://img.shields.io/badge/Contribute-66459B?style=for-the-badge&logoColor=white&logo=Git
[Button Supported]: https://img.shields.io/badge/Supported_Devices-428813?style=for-the-badge&logoColor=white&logo=AdGuard
[Button Website]: https://img.shields.io/badge/Homepage-3B80AE?style=for-the-badge&logoColor=white&logo=freedesktopDotOrg

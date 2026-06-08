

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
`27c6:55a2`** fingerprint sensor (firmware `GF3206_RTSEC_APP_10062`), via the
`goodixtls55x4` driver.

**Status — enroll + verify working on hardware:** the right finger matches
(bozorth score ~31–87), other fingers are rejected (≤14, threshold 24).
Verified end-to-end through `fprintd` + PAM: **KDE screen-unlock and `sudo` by
fingerprint both work.**

### 👉 [Installation guide: INSTALL_55a2.md](INSTALL_55a2.md)

Step-by-step build + isolated install + `fprintd`/PAM setup (openSUSE/KDE),
plus troubleshooting (Windows dual-boot PSK reset, FDT recovery, swipe technique).

How it works: TLS-PSK encrypted capture, treats the tiny 56×176 sensor as a
**swipe** sensor — streams frames during the swipe, per-pixel background
subtraction against the calibration frame (removes the sensor fixed-pattern),
keeps only distinct frames and stacks them **edge-to-edge** into a tall,
minutiae-rich image that NBIS/bozorth matches.

Device coverage of this branch's Goodix TLS drivers:

| USB ID | Driver | Status |
| --- | --- | --- |
| `27c6:55a2` | `goodixtls55x4` | supported & tested (enroll + verify) |
| `27c6:55b4` | `goodixtls55x4` | supported & tested (upstream) |
| `27c6:55a4` | `goodixtls55x4` | unsupported — try at your own risk |
| `27c6:5110` | `goodixtls511` | unrelated, separate pre-existing driver (80×64) |

> Known follow-ups: FDT (finger-detection) can stop firing after many cycles —
> a USB `dev.reset()` restores it; fprintd wiring and broader validation are
> still open.

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

# Samsung Notebook 9 Always Fingerprint Driver (`04e8:7301` / FPC1025)

Linux `libfprint` driver (`fpc1021_samsung`) for the **Samsung Semiconductor Biometric Coprocessor (`04e8:7301`)** found in **Samsung Notebook 9 Always (`NT900X5Y` / `NT900X3N` series)** laptops.

This driver was reverse-engineered from the Windows UMDF2 biometric driver (`fpc_wbdi.dll` v1.50.21.25) and implements the full **FpcG2Usb USB-SPI Bridge** protocol and **FPC1025 (160×160 capacitive sensor) + FPC2050 companion drive IC** calibration and capture pipeline.

---

## Supported Hardware

| Field | Value |
| :--- | :--- |
| **USB VID:PID** | `04e8:7301` (Samsung Electronics Co., Ltd. Biometric Coprocessor) |
| **USB Bridge** | `FpcG2Usb` (Custom Vendor Class `0xFF`, Bulk `0x01 OUT` / `0x81 IN`) |
| **Sensor IC** | Fingerprint Cards **FPC1025** (`HWID = 0x0211`, 160×160 @ 508 DPI) + **FPC2050** Drive IC |
| **Tested Laptop** | Samsung Notebook 9 Always (`NT900X5Y-AD2S`) |

---

## Repository Structure

```text
├── 0001-fpc1021-samsung.patch       # Unified patch against upstream libfprint (v1.94.100)
├── PKGBUILD                         # Arch / Manjaro Linux makepkg build script
├── driver/
│   └── fpc1021_samsung.c            # Standalone libfprint driver source
├── tools/
│   └── test_true_spi.c              # Standalone libusb-1.0 SPI diagnostic & image capture tool
└── docs/
    ├── protocol_spec.md             # Reverse-engineered USB-SPI & FPC1025 register specification
    └── development_plan_and_srs.md  # Software Requirements Specification & Architecture
```

---

## Installation

### Option 1: Arch Linux / Manjaro Linux (`makepkg`)

```bash
git clone https://github.com/chokangmyeong/samsung_fpc1025_fingerprint_driver.git
cd samsung_fpc1025_fingerprint_driver
makepkg -si
sudo systemctl restart fprintd
```

### Option 2: Building from Source (Ubuntu / Debian / Fedora / Other Distros)

1. Install build dependencies for `libfprint` (`meson`, `ninja`, `libgusb`, `libglib2.0`, `nss`, `pixman`, `gobject-introspection`).
2. Clone upstream `libfprint` and apply the patch:
   ```bash
   git clone --branch v1.94.100 https://gitlab.freedesktop.org/libfprint/libfprint.git
   cd libfprint
   git apply /path/to/samsung_fpc1025_fingerprint_driver/0001-fpc1021-samsung.patch
   meson setup build --prefix=/usr
   ninja -C build
   sudo ninja -C build install
   sudo udevadm control --reload-rules && sudo udevadm trigger
   sudo systemctl restart fprintd
   ```

---

## Enrollment & Verification

Once installed and `fprintd` is restarted:

```bash
# Enroll your fingerprint (scans 5 stages)
fprintd-enroll

# Verify the enrolled fingerprint
fprintd-verify
```

### PAM Authentication Setup (sudo / KDE / GNOME Lock Screen)

Add `auth sufficient pam_fprintd.so` to the top of `/etc/pam.d/sudo` or `/etc/pam.d/system-auth`:

```text
#%PAM-1.0
auth      sufficient pam_fprintd.so
auth      include    system-auth
...
```

---

## Key Hardware & Protocol Insights

1. **`FpcG2Usb` USB-SPI Bridgeframing**:
   - Every SPI transaction requires two USB Vendor Control OUT transfers (`bmRequestType = 0x40`):
     - `bRequest = 0xC3`: 16-byte SPI master configuration (3 MHz clock, 8-bit word).
     - `bRequest = 0xCA`, `wValue = 3`, `wIndex = len`: Asserts SPI `CS_N` across `len` bytes.
   - **576-Byte Bridge FIFO Limit**:
     - For transfers `<= 512` bytes, `EP_OUT (0x01)` and `EP_IN (0x81)` can be executed sequentially.
     - For full-frame image readout (`0xC4`, `25,602` bytes), `EP_OUT` **must** be submitted asynchronously in parallel with `EP_IN` so the bridge FIFO is drained as it fills.
2. **FPC1025 + FPC2050 Sensor Configuration**:
   - Register `0x8C` (`FINGER_DRIVE_CONF`) must be set to `0x12` during finger detection and ridge capture to enable the external **FPC2050** companion drive IC (`0x8C = 0x04` activates the internal checkerboard test pattern generator and is only used during ADC calibration).
   - Closed-loop ADC comparator calibration (`0xA8` / `0xA0`) and sub-window Auto-Gain Control (AGC) dynamically tune sensor exposure before each 160×160 capture.
   - Images undergo 8-column ADC channel offset calibration, dynamic range normalization, and `2×` bilinear upscaling (`320×320`) for reliable NIST NBIS `mindtct` / `bozorth3` minutiae matching.

See [docs/protocol_spec.md](docs/protocol_spec.md) for the complete register map and protocol specification.

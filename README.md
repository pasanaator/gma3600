# Intel GMA 3600 (Cedar Trail) Modern Linux DRM/KMS Driver

Development status: version 1.0.4 has user-confirmed visible VGA test patterns at
1024x768/60 and 1920x1080/60 on an Intel D2500HN with Debian 13 / Linux 6.12.117.
TTY text renders correctly; stale contents after late driver takeover clear with
Ctrl+L or `clear`. The user also confirmed that X and the desktop render correctly
using llvmpipe. Automatic boot-time takeover, suspend, and other outputs remain unverified. See [the staged hardware test guide](docs/D2500HN-VGA-TESTING.md)
before installing.

The **SGX545 hybrid port for Debian 13 amd64** has started: see the
[experimental build and hardware identification test](experimental/sgx545/README.md).
The user confirmed readable SGX registers (`01150000` / `0001000d`, revision
1.0.13) with the prior VGA behavior preserved. Parent-owned SGX interrupt routing
now has host tests, including concurrent callback teardown; it is not yet wired
to the legacy services runtime.
The [services preparation test](docs/SGX545-SERVICES-TESTING.md) passed on the Atom:
one device, four tracked pages, zero pages after cleanup, and no blocked register
accesses. The [i386 diagnostic bridge test](docs/SGX545-BRIDGE-TESTING.md) also passed
on the amd64 target, including handle validation, copyout rollback and cleanup.
The [initializer tests](docs/SGX545-INITIALIZER-TESTING.md) passed for explicit and
automatic abort. The [shared-buffer tests](docs/SGX545-BUFFER-TESTING.md) also passed
on the Atom, including WC coherence and mapping lifetime. The
[offline MMU test also passed](research/sgx545/d2500hn-offline-mmu.json), including
all ten scatter/boundary/reuse cases. The
[persistent device memory tests](docs/SGX545-DEVICE-MEMORY-TESTING.md) passed with
services 0.8.0, including native VA/PTE lifetimes, neighboring allocations, WC
coherence and both CPU mapping cleanup paths. Version 0.8.1 adds the
[original Intel CONNECT/free forms and initializer ABI audit](docs/SGX545-INTEL-ABI.md).
The original Intel payload test has now passed on the Atom. Services 0.9.0
adds [initializer translation and ownership staging](docs/SGX545-STAGING-TESTING.md).
GPU execution remains disabled.
The separated legacy SGX core now builds against 6.12.117; runtime rendering,
the complete i386 compat bridge, and presentation remain unfinished. The normal
1.0.4 display build stays the default. See also the
[legacy compatibility assessment](docs/SGX545-ACCELERATION.md).

Run host clock regression tests with `bash tests/run.sh`. Build validation currently
targets Debian Linux 6.12.117; compatibility with other kernel APIs requires separate testing.


A modern Direct Rendering Manager (DRM) and Kernel Mode-Setting (KMS) driver for the **Intel GMA 3600** integrated graphics controller (Intel Atom Cedar Trail processors: **D2500, D2550, D2700, N2600, N2800**), engineered for **modern Linux kernels (5.x / 6.x)** and **modern X.Org Server (1.20+, 21.1+)** and **Wayland**.

---

## 1. Architectural Overview & Context

### 1.1 The Hardware Architecture
Intel Atom Cedarview processors integrate an Intel Gen3/4-derived display controller paired with an Imagination Technologies PowerVR SGX545 3D core:
* **Display Engine (Intel IP)**: Connects to system memory via GTTMMADR (PCI BAR 0) and the GMCH host bridge (PCI 0:0.0). It features dual display pipelines (Pipe A and Pipe B), DPLL frequency synthesizers, analog VGA DAC (`ADPA`), flat-panel LVDS transmitter with automated power sequencing (`PP_CONTROL`/`PP_STATUS`), HDMI/DVI transmitter (`HDMIB`), GMBUS hardware I²C engines for DDC EDID extraction, and hardware cursor engines.
* **3D Core (PowerVR SGX545)**: A separate rasterization IP that was historically driven by closed-source 32-bit user-space binaries.

### 1.2 The Modern Linux & X11 Challenge
Historically, GMA 3600 systems were plagued by two major issues:
1. **The Proprietary Driver Trap**: The original manufacturer driver (`xf86-video-cdv` / PVR binary blobs) was compiled only for 32-bit user spaces, tied to obsolete Linux 3.0/3.2 kernels, and bound to X.Org Server 1.9/1.10 (Video ABI 8). It relied on deprecated User Mode-Setting (UMS) and became completely incompatible when X.Org modernized to Video ABI 24/25 and Linux adopted 64-bit kernels.
2. **The In-Tree Driver Deficiencies**: The in-tree `gma500_gfx` driver contained stubbed and unstable Cedarview support lacking atomic modesetting, suffering from lockups on display reconfiguration, and lacking proper integration with modern generic display servers.

### 1.3 The Modern Solution
This driver (`gma3600`) implements a modern DRM/KMS driver that:
* Conforms to the modern Linux **Atomic Modesetting API** (`drm_atomic_helper_*`).
* Exposes standard GEM dumb buffers (`DRM_IOCTL_MODE_CREATE_DUMB`) through a DRM display node (`/dev/dri/cardN`). There is no SGX submission API or accelerated render-node support yet.
* Integrates directly with the modern X.Org Server's built-in **`modesetting` DDX driver** (`xf86-video-modesetting`), completely eliminating the need for obsolete proprietary DDX drivers.
* Supports CPU rendering with Mesa **llvmpipe** / **softpipe** and a standard modesetting desktop. This is software OpenGL, not SGX545 hardware 3D acceleration. X11 **ShadowFB** is another software display option.

---

## 2. Driver Subsystems & Implementation Details

| Source File | Functionality |
| :--- | :--- |
| `gma3600_drv.c` | Module initialization, PCI probe/remove, DRM driver registration, file operations table. |
| `gma3600_device.c` | PCI BAR 0 (GTTMMADR) MMIO mapping, BAR 2 aperture configuration, stolen memory size discovery from GMCH bridge (PCI 0:0.0), VGA disablement. |
| `gma3600_gtt.c` | Graphics Translation Table (GTT) management: 32-bit PTE programming, stolen memory mapping, dynamic page insertion/removal into the aperture. |
| `gma3600_gem.c` | GEM buffer allocation, memory page management, DMA mapping, dumb buffer creation (`gma3600_gem_dumb_create`), memory mapping (`mmap`). |
| `gma3600_display.c` | KMS setup: primary planes, 64×64 hardware cursor planes, CRTCs (Pipe A & Pipe B), timing register programming, atomic commit & page flips. |
| `gma3600_dpll.c` | Display PLL clock synthesis: target dotclock search algorithm (`m`, `n`, `p1`, `p2`), VCO bounds validation, and hardware DPLL programming. |
| `gma3600_crt.c` | Analog CRT / VGA output (`ADPA`), polarity configuration, DPMS power states, DDC EDID probe via GMBUS pin 2. |
| `gma3600_lvds.c` | Flat panel LVDS output (`LVDS`), automated panel power sequencing (`PP_CONTROL`/`PP_STATUS`), dual-channel / single-channel modes, DDC EDID. |
| `gma3600_hdmi.c` | Digital HDMI / DVI output (`HDMIB`), audio/video packet mode control, TMDS encoder, DDC EDID probe via GMBUS pin 4. |
| `gma3600_i2c.c` | GMBUS hardware I²C / DDC bus controller for monitor EDID negotiation across all ports. |
| `gma3600_irq.c` | Interrupt handler: top-level IER/IIR/IMR master control and second-level PIPExSTAT for VBLANK, page flip completion, and hotplug. |
| `gma3600_backlight.c` | Backlight subsystem (`/sys/class/backlight`): PWM frequency and duty-cycle control via `BLC_PWM_CTL` and `BLC_PWM_CTL2`. |
| `gma3600_pm.c` | Power management: PCI D0/D3hot power state transitions, CRTC register preservation and restoration across suspend/resume. |
| `gma3600_fb.c` | Framebuffer configuration (`drm_mode_fb_cmd2`) and generic in-kernel fbdev console emulation. |

---

## 3. Directory Layout

```
project/
├── kernel/
│   ├── gma3600/
│   │   ├── Makefile                # Out-of-tree kernel module build script
│   │   ├── Kconfig                 # Kernel configuration entry
│   │   ├── dkms.conf               # DKMS automatic rebuild configuration
│   │   ├── gma3600_reg.h           # Complete hardware register & bitfield definitions
│   │   ├── gma3600_drv.h           # Driver structures, helpers & prototypes
│   │   ├── gma3600_drv.c           # Module & PCI lifecycle
│   │   ├── gma3600_device.c        # Hardware resource allocation & initialization
│   │   ├── gma3600_gtt.c           # GTT & stolen memory management
│   │   ├── gma3600_gem.c           # GEM & dumb buffer management
│   │   ├── gma3600_display.c       # Atomic KMS CRTCs & planes
│   │   ├── gma3600_dpll.c          # DPLL clock synthesis
│   │   ├── gma3600_crt.c           # Analog VGA / CRT encoder & connector
│   │   ├── gma3600_lvds.c          # LVDS flat panel encoder & connector
│   │   ├── gma3600_hdmi.c          # HDMI / DVI encoder & connector
│   │   ├── gma3600_i2c.c           # GMBUS I2C / DDC bus controller
│   │   ├── gma3600_irq.c           # Interrupts & VBLANK handling
│   │   ├── gma3600_backlight.c     # Backlight sysfs device
│   │   ├── gma3600_pm.c            # Power management suspend/resume
│   │   └── gma3600_fb.c            # Framebuffer & fbdev emulation
├── xorg/
│   ├── 20-gma3600.conf             # Modern X11 configuration (modesetting + Glamor)
│   ├── 20-gma3600-shadowfb.conf    # Low-power X11 configuration (modesetting + ShadowFB)
│   ├── blacklist-gma500.conf       # Prevents conflicts with legacy in-tree driver
│   └── install.sh                  # One-step automated installation script
└── README.md                       # Comprehensive documentation
```

---

## 4. Building and Installation

### 4.1 System Prerequisites
Ensure build essentials, kernel headers matching your running kernel, and X.Org are installed:

* **Debian / Ubuntu / Linux Mint**:
  ```bash
  sudo apt update
  sudo apt install build-essential linux-headers-$(uname -r) dkms xserver-xorg-core
  ```
* **Fedora / RHEL**:
  ```bash
  sudo dnf install gcc make kernel-devel-$(uname -r) dkms xorg-x11-server-Xorg
  ```
* **Arch Linux / Manjaro**:
  ```bash
  sudo pacman -S base-devel linux-headers dkms xorg-server
  ```

### 4.2 Automated Installation (Recommended)
Run the automated installation script with root privileges:
```bash
cd xorg
sudo bash install.sh
```
The script will:
1. Verify kernel headers and build dependencies.
2. Blacklist the legacy `gma500_gfx` driver in `/etc/modprobe.d/blacklist-gma500.conf`.
3. Compile the `gma3600.ko` module and install it to `/lib/modules/$(uname -r)/updates/dkms/`.
4. Register with DKMS so the driver automatically rebuilds whenever the kernel updates.
5. Install the modern X.Org configuration to `/etc/X11/xorg.conf.d/20-gma3600.conf`.
6. Configure automatic module loading on boot via `/etc/modules-load.d/gma3600.conf`.
7. Unload conflicting drivers and load the new `gma3600` module.

### 4.3 Manual Build & Install
If you prefer building manually:
```bash
cd kernel/gma3600
make
sudo make install
sudo depmod -a
sudo cp ../../xorg/blacklist-gma500.conf /etc/modprobe.d/
sudo cp ../../xorg/20-gma3600.conf /etc/X11/xorg.conf.d/
sudo modprobe gma3600
```

### 4.4 Reference Clock & DPLL Configuration
Cedar Trail uses an indirect DPIO Sideband (SB) interface for its Display PLLs. Different Cedar Trail SKUs use different DPLL reference clocks:
* **Desktop SKUs (D2500, D2550, D2700)**: Typically use a **27 MHz** reference clock for external CRT (VGA) and HDMI/DVI monitors.
* **Netbook / Mobile SKUs (N2600, N2800)**: Use a **96 MHz** (or 100 MHz SSC) reference clock for internal LVDS flat panels.

The driver automatically detects the correct reference clock from the board identification and BIOS boot PLL state. D2500HN uses the desktop 27 MHz reference. The driver does not retry unrelated crystal frequencies when lock fails, because a locked PLL can still produce incorrect monitor timings.

You can also manually override the reference clock via module parameter if needed:
```bash
# Force 27 MHz reference clock (desktop motherboards)
sudo modprobe gma3600 refclk=27000

# Force 96 MHz reference clock (netbook / mobile)
sudo modprobe gma3600 refclk=96000
```
Or persistently in `/etc/modprobe.d/gma3600.conf`:
```text
options gma3600 refclk=27000
```

---

## 5. Modern X11 Server Integration

Modern X.Org Server (1.20, 21.1+) uses the built-in `modesetting` DDX driver. With `gma3600` loaded, the server communicates with the kernel via DRM atomic ioctls:

### 5.1 Standard High-Performance Profile (Glamor 2D Acceleration)
File: `/etc/X11/xorg.conf.d/20-gma3600.conf`
```xorg
Section "Device"
    Identifier     "Intel GMA 3600"
    Driver         "modesetting"
    BusID          "PCI:0:2:0"
    Option         "kmsdev"            "/dev/dri/card0"
    Option         "SWcursor"          "false"          # Uses GMA 3600 64x64 HW cursor
    Option         "PageFlip"          "true"           # Tear-free VBlank page flips
    Option         "AccelMethod"       "glamor"         # Mesa LLVMpipe OpenGL acceleration
    Option         "DRI"               "3"
EndSection
```

### 5.2 Low-Power / Netbook Profile (ShadowFB)
For single-core Atom N2600 systems where you want zero GPU/JIT rasterization overhead:
```bash
sudo cp xorg/20-gma3600-shadowfb.conf /etc/X11/xorg.conf.d/20-gma3600.conf
```
This mode uses a CPU shadow buffer with direct dirty-rectangle damage tracking, minimizing memory bandwidth on Atom processors.

---

## 6. Verification & Diagnostics

Once installed, verify driver operation with the following commands:

1. **Kernel Module Verification**:
   ```bash
   dmesg | grep -i gma3600
   ```
   *Expected output:*
   ```text
   gma3600 0000:00:02.0: MMIO at 0x... (1024 KB), Aperture at 0x... (256 MB)
   gma3600 0000:00:02.0: Stolen memory: base 0x..., size 64 MB
   gma3600 0000:00:02.0: GTT total entries: 65536 (maps 256 MB aperture)
   gma3600 0000:00:02.0: Intel GMA 3600 (Cedar Trail) initialized successfully
   ```

2. **DRM Device Nodes**:
   ```bash
   ls -la /dev/dri/
   ```
   *Verify the gma3600 display node; its card number can vary. The current driver does not advertise accelerated render-node support.*

3. **Display Outputs and EDID**:
   ```bash
   xrandr --verbose
   ```
   *Displays available connectors (VGA-1, LVDS-1, HDMI-1), native panel resolutions, and active refresh rates.*

4. **Backlight Control**:
   ```bash
   cat /sys/class/backlight/gma3600_backlight/max_brightness
   echo 1000 | sudo tee /sys/class/backlight/gma3600_backlight/brightness
   ```

5. **X.Org Driver Binding**:
   ```bash
   cat /var/log/Xorg.0.log | grep -i modeset
   ```
   *Confirms `modesetting: Driver for Modesetting Kernel Drivers: kms` is loaded and bound to `/dev/dri/card0`.*

---

## 7. License
This driver is licensed under the **GNU General Public License v2.0 only (GPL-2.0-only)**.

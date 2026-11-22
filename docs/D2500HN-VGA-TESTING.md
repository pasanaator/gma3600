# D2500HN VGA testing on Debian 13

This change targets the reported invalid-timing display and failed takeover on
Intel D2500HN VGA with Linux 6.12.117. Compilation and host tests pass; display
operation still needs testing on the board. Keep another computer connected by
SSH during the first load. Build on the Atom for its own kernel and architecture.

## What changed

* Use Cedarview PLL control rather than legacy Intel PLL mode bits. Select the
  desktop 27 MHz reference on D2500HN; `refclk=27000` remains an explicit override.
  Do not retry 96 MHz arithmetic just because a PLL did not lock: a lock cannot
  prove the crystal frequency. Reject unavailable clocks and errors above 0.5%.
* Validate modes before commit and leave encoders disabled on PLL/pipe failure.
  Log requested/actual clocks and dividers for every enabled mode.
* Remove conflicting firmware framebuffers, stop firmware scanout before changing
  its registers/page tables, and register the fbdev callbacks required by the
  6.12.117 DRM client backport. Add GEM kernel mappings for the console client.
* Map the GTT at PGETBL_CTL's physical address, preserve its control/address bits,
  use coherent PTEs, and stop guessing stolen-memory mappings.
* The follow-up uses GPIOA bit-banged VGA DDC and CRT load detection (matching
  the upstream Cedarview path), corrects DSPFW1 field positions, and enables the
  reference clock before DPIO reset. Mocked-register tests cover PLL ordering
  and error propagation; these do not simulate actual analog display hardware.
* Fix EDID transactions (WAIT phase, separate STOP, reset deassertion), serialize
  shared GMBUS access, and poll connector changes outside hard IRQ context.
* Correct HDMI lane bits, keep them untouched for VGA, and correct interrupt
  acknowledgement ordering. Handle unaligned framebuffer offsets explicitly.

## 1. Enable logs before testing

Run these on the Atom while the working driver is active:

```sh
sudo mkdir -p /var/log/journal /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=persistent\n' | sudo tee /etc/systemd/journald.conf.d/90-gma3600-test.conf
sudo systemd-tmpfiles --create --prefix /var/log/journal
sudo systemctl restart systemd-journald
sudo journalctl --flush
sudo journalctl --sync
journalctl --list-boots
uname -a
lspci -nnk -s 00:02.0
```

A hard lock before journald starts or writes to disk can still lose boot logs.
The controlled late-load test below lets you stream messages over SSH instead.

## 2. Build the changed sources

Transfer this changed checkout to the Atom (the GitHub main branch may still
contain the old code). From its root:

```sh
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) kmod pciutils libdrm-tests
bash tests/run.sh
make -C kernel/gma3600 -j2
modinfo kernel/gma3600/gma3600.ko
```

The module's vermagic must start with `uname -r`. Keep the module file for the
next step. Do not run the automated installer yet.

## 3. Test late loading before installing at boot

In GRUB press `e` on the Debian entry. Append to the line beginning `linux`:

```text
module_blacklist=gma500_gfx systemd.unit=multi-user.target drm.debug=0x1e log_buf_len=4M
```

Remove `nomodeset` if present. If an older gma3600 was configured to load at boot,
also append `modprobe.blacklist=gma3600` for this test; use `insmod` below to load
the newly built file directly. Boot with Ctrl-X/F10. These edits last one boot.

From SSH, first check:

```sh
lspci -nnk -s 00:02.0
lsmod | grep -E 'gma3600|gma500'
```

Neither gma500_gfx nor an old gma3600 should own the PCI device. Do not unbind a
live graphics driver. Correct the temporary boot arguments and reboot if needed.

In one SSH terminal, start streaming logs to the other computer's terminal
(and save its terminal output):

```sh
sudo dmesg -wH
```

In a second SSH terminal on the Atom, from the changed checkout:

```sh
sudo modprobe drm_kms_helper
sudo modprobe drm_ttm_helper
sudo modprobe i2c_algo_bit
sudo insmod kernel/gma3600/gma3600.ko refclk=27000
cat /sys/module/gma3600/version
lspci -nnk -s 00:02.0
readlink -f /sys/bus/pci/devices/0000:00:02.0/driver
cat /proc/fb
sudo modetest -M gma3600 -c -p
```

Expected: running module version `1.0.4`, `Kernel driver in use: gma3600`, a gma3600 PCI driver link, a DRM fbdev
console when fbdev emulation is enabled, and VGA connector modes matching the
monitor. Seeing the module in `lsmod` alone does not establish display ownership.

The log should say `Display reference 27000 kHz` and print a line similar to
`Pipe A: 1024x768 clock 65000 -> ... kHz (N=... M2=... P1=... P2=...)`.
There should be no PLL timeout, failed-pipe message, Oops, or atomic commit timeout.
Record the exact output if it fails; do not try repeated live reloads.

With the graphical session stopped, test a mode actually listed by modetest:

```sh
sudo modetest -M gma3600 -s CONNECTOR_ID@CRTC_ID:1024x768-60
```

Replace both IDs with those listed by modetest and choose a listed mode if
1024x768 at 60 Hz is unavailable. Visually confirm the test pattern and monitor
OSD frequency. Exit modetest with Enter. Test 640x480/60, 1024x768/60, and native
resolution where advertised. Include which mode triggers an invalid-timing error.
For the reported immediate signal loss, capture the complete dmesg output after
loading, plus `sudo modetest -M gma3600 -c -p` and `cat /proc/fb`, over SSH.
Look for `VGA: EDID supplied ... modes` (or the no-EDID warning), the requested
pixel-clock line, and `VGA enabled: ...` register snapshots. If the last snapshot
is missing, a failure message or absent connected output should precede it.
`firmware scanout: ...` snapshots record the pre-takeover timing registers.
A forced diagnostic boot option `video=VGA-1:640x480@60e` can isolate detection
from mode programming; use it only for one boot and still collect the logs.

Then start the desktop with `sudo systemctl start display-manager`.
An existing desktop does not automatically migrate to a newly loaded DRM device;
start it after binding. Under X11, save `xrandr --verbose` from the desktop session.

## Blank screen with a detected signal

On the reported 1.0.1 test, EDID detection succeeded (Dell P2417H, 12 modes),
the PLL locked at 148.5 MHz, Pipe A enabled at 1920x1080/60, and fbdev registered.
Those results established mode setup progress, but did not prove pixel delivery.
The later 1.0.4 hardware results below confirm visible patterns and TTY drawing.

Before changing the installed boot driver, stop the display manager over SSH
and use the connector/CRTC IDs from your own modetest output. In the supplied
log they were 43 and 36:

```sh
sudo systemctl stop display-manager
sudo modetest -M gma3600 -s 43@36:1024x768-60
```

Report whether the pattern is visible, stays black, or causes a timing error.
Exit with Enter. If modetest fails, send its exact error; do not assume the
pattern was displayed. A visible pattern isolates the problem to console or
desktop content; a black pattern at a valid mode points to scanout/data delivery.
This stops the desktop session, so save work beforehand.

Version 1.0.2 added a fatal GMCH enable-bit readback check. On the D2500HN,
probe then failed with -EIO before DRM registration. Version 1.0.3 removes that
strict requirement: like Linux gma500, it checks the PCI write result, while
readback is diagnostic. Actual PCI access errors still fail with an explicit
operation-specific message. Regression tests cover a non-sticking bit and a
failed optional readback, as well as real PCI access failures.

The 1.0.1 modetest pattern was also black, so the blank image was not limited to
console content. Version 1.0.3 uses the DRM enable-before-planes commit helper:
first program and enable the pipe clocks, then latch plane control and SURF.
Previously the default helper programmed the plane before PLL/pipe enable.
This follows the ordering of Cedarview's pipe setup and base programming, but
whether it resolves the board's black scanout still needs a hardware test.

It logs `GTT: GMCH ... -> ...`, plane pitch/surface/linear offset, the first
GTT PTE, and a `scanout latched` snapshot after the plane update. The `VGA enabled`
snapshot now precedes plane programming; use `scanout latched` for the final
plane register values. A valid image is the pass condition; successful modetest
return or a software framebuffer binding alone does not prove working scanout.
The user confirmed that 1.0.3's modetest mode switch completed but the image
remained black. A successful mode switch is therefore not a hardware pass.
Version 1.0.4 aligns display memory with Linux gma500: allocate physical pages
below 4 GB for GTT PTEs, set their direct-map aliases to write-combining, and use
WC for both kernel vmap and userspace mmap. Remove streaming DMA mappings that
were never synchronized with later CPU framebuffer writes. Restore WB caching
before releasing pages. After these changes, the user confirmed visible test
patterns on the board. The tests do not isolate which individual memory change
was responsible for the improvement.

Host tests cover physical-address bounds, WC/WB transitions, partial allocation
failures, and cache-conversion failure cleanup. On each modeset the driver logs
`WC=1`, the first PTE, and a read-only `RAM center pixel` sample. Initial console
pixels may be zero; collect the sample from the modetest pattern too. A nonzero
sample demonstrates CPU-visible content, not successful display fetches.

After a clean manual-load test of 1.0.4, collect:

```sh
cat /sys/module/gma3600/version
cat /proc/fb
sudo dmesg | grep -Ei 'gma3600|drm|VGA|PLL|GTT|Plane'
sudo modetest -M gma3600 -c -p
```

### Confirmed 1.0.4 hardware results

On Intel D2500HN, Debian 13 / Linux 6.12.117, connected by VGA to a Dell P2417H:

* User visually confirmed clean colour bars at 1024x768/60 and 1920x1080/60.
* New TTY characters render correctly. The block of garbled old content seen
  after manual takeover disappears with Ctrl+L or `clear` on the physical TTY.
* No ongoing TTY corruption was reported after clearing. This observation is
  consistent with stale console contents during late takeover, rather than
  evidence of persistent pixel-format or stride corruption.
* Desktop operation, automatic boot-time takeover, suspend/resume, and other
  outputs remain unverified. Keep 1.0.4 as the current hardware test revision.

After exiting modetest, clear the physical console if needed and test the desktop:

```sh
sudo systemctl start display-manager
```

Starting the display manager does not reboot or reinstall the driver. Report
whether its login screen and desktop render correctly before treating desktop
support as validated. Follow the installation and recovery steps below to test
boot-time loading separately.

## 4. Install only after the late-load test passes

Check `dkms status` if DKMS is installed. If it lists gma3600/1.0.0, remove that
old registration with `sudo dkms remove -m gma3600 -v 1.0.0 --all` first, so an old
copy in updates/dkms cannot override the manual build. Then:

```sh
sudo make -C kernel/gma3600 install
sudo depmod -a
modinfo -n gma3600
```

Compare the installed file with the new build (they should match):

```sh
sha256sum kernel/gma3600/gma3600.ko "$(modinfo -n gma3600)"
```

If module compression/signing is configured, compare `modinfo` metadata instead
of expecting identical file hashes. Rebuild/sign the module through your normal
Secure Boot process if the kernel rejects its key.

If needed, persist the conflict blacklist and autoload:

```sh
printf 'blacklist gma500_gfx\n' | sudo tee /etc/modprobe.d/gma3600-test.conf
printf 'gma3600\n' | sudo tee /etc/modules-load.d/gma3600-test.conf
sudo update-initramfs -u -k "$(uname -r)"
```

Reboot normally and repeat binding, console, and mode checks. Test the boot once
with the default reference selection as well. Verify
`cat /sys/module/gma3600/version` reports `1.0.4`; an older version or a missing
version file means you are still testing an older module, possibly in initramfs. Do not overwrite an existing Xorg
profile just to test binding; the modesetting driver normally detects KMS.

## 5. Results and recovery

Collect `uname -a`, `lspci -nnk -s 00:02.0`, `modinfo gma3600`,
`sudo modetest -M gma3600 -c -p`, and `sudo journalctl -b -k`.
After a failed reboot, check `journalctl --list-boots` and
`sudo journalctl -b -1 -k`. Send the monitor model, requested mode, OSD message,
and the PLL diagnostic line. Streaming SSH logs remain useful when the journal
has no failed-boot entry. Suspend, HDMI, LVDS, and multiple displays have not been
validated by the cloud build or the clock tests.

To recover, temporarily boot with `module_blacklist=gma3600` and without the
`module_blacklist=gma500_gfx` test argument. From a console/SSH, remove only the
`gma3600-test.conf` files you created under modprobe.d and modules-load.d, restore
any previous configuration you changed, run `sudo update-initramfs -u`, and reboot.
Do not unload the active graphics driver under a running display server.

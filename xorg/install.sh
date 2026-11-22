#!/usr/bin/env bash
#
# Intel GMA 3600 (Cedar Trail) Modern Linux Driver Installer
# Copyright (C) 2026 Antigravity Project
#
# Builds and installs the gma3600 DRM/KMS kernel module and
# configures modern X.Org Server (1.20+, 21.1+).
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE}   Intel GMA 3600 Modern Linux Driver Installer       ${NC}"
echo -e "${BLUE}======================================================${NC}"

if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}Error: This script must be run as root (sudo ./install.sh)${NC}" 1>&2
   exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_SRC_DIR="${SCRIPT_DIR}/../kernel/gma3600"
KVER="$(uname -r)"

echo -e "Target Kernel: ${GREEN}${KVER}${NC}"

# 1. Install prerequisites check
echo -e "\n${BLUE}[1/5] Checking build dependencies...${NC}"
if [ ! -d "/lib/modules/${KVER}/build" ]; then
    echo -e "${RED}Kernel headers for ${KVER} not found!${NC}"
    echo "Please install kernel headers:"
    echo "  Debian/Ubuntu: apt install build-essential linux-headers-${KVER} dkms"
    echo "  Fedora/RHEL:   dnf install gcc kernel-devel-${KVER} dkms"
    echo "  Arch Linux:    pacman -S base-devel linux-headers dkms"
    exit 1
fi

# 2. Blacklist legacy broken in-tree driver
echo -e "\n${BLUE}[2/5] Blacklisting legacy gma500_gfx driver...${NC}"
mkdir -p /etc/modprobe.d
cp "${SCRIPT_DIR}/blacklist-gma500.conf" /etc/modprobe.d/blacklist-gma500.conf
echo "Installed /etc/modprobe.d/blacklist-gma500.conf"

# 3. Build kernel module
echo -e "\n${BLUE}[3/5] Building gma3600 kernel module...${NC}"
make -C "${KERNEL_SRC_DIR}" clean
make -C "${KERNEL_SRC_DIR}" -j"$(nproc)"

# Install module into updates directory
DEST_DIR="/lib/modules/${KVER}/updates/dkms"
mkdir -p "${DEST_DIR}"
cp "${KERNEL_SRC_DIR}/gma3600.ko" "${DEST_DIR}/"
depmod -a "${KVER}"
echo -e "${GREEN}Module installed to ${DEST_DIR}/gma3600.ko${NC}"

# Optional DKMS registration if dkms is present
if command -v dkms >/dev/null 2>&1; then
    echo "Registering with DKMS for automatic kernel upgrade builds..."
    DKMS_SRC="/usr/src/gma3600-1.0.1"
    mkdir -p "${DKMS_SRC}"
    cp -r "${KERNEL_SRC_DIR}"/* "${DKMS_SRC}/"
    if ! dkms status -m gma3600 -v 1.0.1 | grep -q .; then
        dkms add -m gma3600 -v 1.0.1
    fi
    dkms build -m gma3600 -v 1.0.1
    dkms install -m gma3600 -v 1.0.1 --force
fi

# Configure autoload on boot
mkdir -p /etc/modules-load.d
echo "gma3600" > /etc/modules-load.d/gma3600.conf

# Update initramfs if available
if command -v update-initramfs >/dev/null 2>&1; then
    echo "Updating initramfs (Debian/Ubuntu)..."
    update-initramfs -u
elif command -v dracut >/dev/null 2>&1; then
    echo "Updating initramfs (Dracut/Fedora)..."
    dracut -f
fi

# 4. Configure Modern X11 Server
echo -e "\n${BLUE}[4/5] Configuring modern X.Org Server...${NC}"
mkdir -p /etc/X11/xorg.conf.d/
cp "${SCRIPT_DIR}/20-gma3600.conf" /etc/X11/xorg.conf.d/20-gma3600.conf
echo "Installed /etc/X11/xorg.conf.d/20-gma3600.conf"

# 5. Load module
echo -e "\n${BLUE}[5/5] Loading gma3600 module into current kernel...${NC}"
if lsmod | grep -q '^gma500_gfx '; then
    echo "Installed. gma500_gfx currently owns the device; reboot to switch drivers."
    echo "Do not unload a graphics driver while the desktop is using it."
    exit 0
fi

modprobe gma3600
bound=false
for device in /sys/bus/pci/drivers/gma3600/????:??:??.?; do
    if [ -e "$device" ]; then bound=true; break; fi
done
if [ "$bound" != true ]; then
    echo "Module loaded, but no PCI device is bound to gma3600. Check lspci -nnk and journalctl -b -k." >&2
    exit 1
fi

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${GREEN}   Installation Complete!                             ${NC}"
echo -e "${GREEN}======================================================${NC}"
echo "Verify status with:"
echo "  dmesg | grep -i gma3600"
echo "  ls -l /dev/dri/card*"
echo "  xrandr (inside X11)"

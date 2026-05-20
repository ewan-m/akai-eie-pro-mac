#!/bin/bash
# Permanent installer for AKAI EIE Pro driver
set -e

KERNEL_VERSION=$(uname -r)
MODULE_NAME="akai_eie_pro"
MODULE_FILE="${MODULE_NAME}.ko"
INSTALL_DIR="/lib/modules/${KERNEL_VERSION}/kernel/drivers/usb/misc"
CONFIG_FILE="/etc/modules-load.d/${MODULE_NAME}.conf"

echo "=== AKAI EIE Pro Permanent Driver Installer ==="

# Check if running as root for installation steps
if [[ $EUID -eq 0 ]]; then
    echo "Don't run this script as root directly. It will use sudo when needed."
    exit 1
fi

# Detect distribution and install dependencies
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
else
    echo "Cannot detect distribution"
    exit 1
fi

echo "Detected distribution: $DISTRO"

# Install dependencies based on distro
case $DISTRO in
    "ubuntu"|"debian"|"pop"|"kubuntu")
        echo "Installing dependencies for Debian/Ubuntu..."
        sudo apt update
        sudo apt install -y build-essential linux-headers-${KERNEL_VERSION} alsa-utils
        ;;
    "arch"|"manjaro")
        echo "Installing dependencies for Arch Linux..."
        sudo pacman -Sy --needed base-devel linux-headers alsa-utils
        ;;
    "fedora")
        echo "Installing dependencies for Fedora..."
        sudo dnf install -y kernel-devel gcc make alsa-utils
        ;;
    "opensuse"|"opensuse-leap"|"opensuse-tumbleweed")
        echo "Installing dependencies for openSUSE..."
        sudo zypper install -y kernel-default-devel gcc make alsa-utils
        ;;
    *)
        echo "Unsupported distribution: $DISTRO"
        echo "Please install manually: build tools, kernel headers, alsa-utils"
        read -p "Continue anyway? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
        ;;
esac

# Build the module
echo "Building driver..."
make clean
make

if [ ! -f "$MODULE_FILE" ]; then
    echo "ERROR: Build failed - $MODULE_FILE not found"
    exit 1
fi

# Stop any currently running module
if lsmod | grep -q "$MODULE_NAME"; then
    echo "Unloading existing module..."
    sudo rmmod $MODULE_NAME || true
fi

# Create installation directory
echo "Creating installation directory..."
sudo mkdir -p "$INSTALL_DIR"

# Copy module to system location
echo "Installing module to $INSTALL_DIR..."
sudo cp "$MODULE_FILE" "$INSTALL_DIR/"

# Update module dependencies
echo "Updating module dependencies..."
sudo depmod -a

# Create auto-load configuration
echo "Setting up automatic module loading..."
echo "$MODULE_NAME" | sudo tee "$CONFIG_FILE" > /dev/null

# Load the module now
echo "Loading module..."
sudo modprobe "$MODULE_NAME"

# Verify installation
if lsmod | grep -q "$MODULE_NAME"; then
    echo "✅ SUCCESS: AKAI EIE Pro driver installed permanently!"
    echo "✅ Module will automatically load on boot"
    echo "✅ Module is currently active"
else
    echo "❌ ERROR: Module failed to load"
    exit 1
fi

# Show audio devices
echo ""
echo "=== Audio Devices ==="
aplay -l | grep -i eie || echo "No EIE playback device found"
arecord -l | grep -i eie || echo "No EIE capture device found"

echo ""
echo "=== Installation Complete ==="
echo "• Driver location: $INSTALL_DIR/$MODULE_FILE"
echo "• Auto-load config: $CONFIG_FILE"
echo "• Restart PipeWire: systemctl --user restart pipewire pipewire-pulse"
echo "• The driver will survive reboots!"
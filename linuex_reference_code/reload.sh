#!/bin/bash
# AKAI EIE Pro driver reload script with optional force mode

# Parse command line arguments
FORCE_MODE=false
if [[ "$1" == "--force" || "$1" == "-f" ]]; then
    FORCE_MODE=true
fi

# Display usage if help requested
if [[ "$1" == "--help" || "$1" == "-h" ]]; then
    echo "Usage: $0 [--force|-f] [--help|-h]"
    echo ""
    echo "Options:"
    echo "  --force, -f    Use force reload (aggressive mode for stubborn cases)"
    echo "  --help, -h     Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0             Normal gentle reload"
    echo "  $0 --force     Force reload (unbind USB, force rmmod, kill processes)"
    exit 0
fi

if [[ "$FORCE_MODE" == "true" ]]; then
    echo "=== Force Reloading AKAI EIE Pro Driver ==="
else
    echo "=== Reloading AKAI EIE Pro Driver ==="
fi

# Step 1: USB device handling (force mode only)
if [[ "$FORCE_MODE" == "true" ]]; then
    echo "1. Unbinding USB device..."
    echo "1-6.4.4" | sudo tee /sys/bus/usb/drivers/usb/unbind >/dev/null 2>&1 || true
    step=2
else
    step=1
fi

# Step 1/2: Stop audio processes
echo "$step. Stopping audio processes..."
if [[ "$FORCE_MODE" == "true" ]]; then
    # Force mode: aggressive shutdown
    systemctl --user stop pipewire pipewire-pulse 2>/dev/null || true
    sudo killall -9 pipewire pipewire-pulse 2>/dev/null || true
    sleep 2
else
    # Normal mode: gentle shutdown
    sudo fuser -k /dev/snd/pcmC4D0c /dev/snd/pcmC4D0p 2>/dev/null || true
    systemctl --user stop pipewire pipewire-pulse 2>/dev/null || true
    sleep 1
fi

# Step 2/3: Remove the old module
((step++))
echo "$step. Removing old module..."
if [[ "$FORCE_MODE" == "true" ]]; then
    sudo rmmod -f akai_eie_pro 2>/dev/null || true
else
    sudo rmmod akai_eie_pro 2>/dev/null || true
fi

# Step 3/4: Build the driver
((step++))
echo "$step. Building driver..."
make clean && make

# Step 4/5: USB rebind (force mode only)
if [[ "$FORCE_MODE" == "true" ]]; then
    ((step++))
    echo "$step. Rebinding USB device..."
    echo "1-6.4.4" | sudo tee /sys/bus/usb/drivers/usb/bind >/dev/null
    sleep 1
fi

# Step 4/5/6: Load the new module
((step++))
echo "$step. Loading new module..."
sudo insmod akai_eie_pro.ko

# Step 5/6/7: Restart audio system
((step++))
echo "$step. Restarting audio system..."
systemctl --user start pipewire pipewire-pulse
if [[ "$FORCE_MODE" == "true" ]]; then
    sleep 3
else
    sleep 2
fi

# Force mode: Set audio defaults
if [[ "$FORCE_MODE" == "true" ]]; then
    ((step++))
    echo "$step. Setting up audio defaults..."
    pactl set-default-source alsa_input.pci-0000_16_00.0-usb-0_6.4.4.stereo-fallback 2>/dev/null || true
fi

# Final step: Verify
((step++))
echo "$step. Verification:"
lsmod | grep akai_eie_pro
echo "Audio devices:"
arecord -l | grep -A1 "card 4"

if [[ "$FORCE_MODE" == "true" ]]; then
    echo "=== Force reload complete! ==="
else
    echo "=== Driver reload complete! ==="
fi
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

bundle_name="AkaiEiePro.driver"
install_dir="/Library/Audio/Plug-Ins/HAL"

make sign

sudo mkdir -p "$install_dir"
sudo rm -rf "$install_dir/$bundle_name"
sudo cp -R "build/$bundle_name" "$install_dir/"
sudo chown -R root:wheel "$install_dir/$bundle_name"

sudo killall coreaudiod 2>/dev/null || true

echo "Installed $install_dir/$bundle_name and restarted coreaudiod."

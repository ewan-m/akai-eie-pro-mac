#!/bin/bash

# AKAI EIE Pro Driver Uninstall Script
# This script removes the AKAI EIE Pro USB audio driver from the system
# and cleans up all configuration files

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print functions
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
check_root() {
    if [[ $EUID -eq 0 ]]; then
        print_warning "Running as root. This is acceptable for uninstallation."
    else
        print_info "This script will use sudo for privileged operations."
    fi
}

# Function to run command with sudo if not root
run_privileged() {
    if [[ $EUID -eq 0 ]]; then
        "$@"
    else
        sudo "$@"
    fi
}

# Unload the module if it's currently loaded
unload_module() {
    print_info "Checking if AKAI EIE Pro module is loaded..."
    
    if lsmod | grep -q "akai_eie_pro"; then
        print_info "Unloading akai_eie_pro module..."
        if run_privileged rmmod akai_eie_pro; then
            print_success "Module unloaded successfully"
        else
            print_warning "Failed to unload module (it may be in use)"
            print_info "You may need to close audio applications and try again"
        fi
    else
        print_info "Module is not currently loaded"
    fi
}

# Remove auto-loading configuration
remove_autoload_config() {
    print_info "Removing auto-loading configuration..."
    
    local config_files=(
        "/etc/modules-load.d/akai-eie-pro.conf"
        "/etc/modules-load.d/akai_eie_pro.conf"
        "/etc/modprobe.d/akai-eie-pro.conf"
        "/etc/modprobe.d/akai_eie_pro.conf"
    )
    
    local removed_any=false
    for config_file in "${config_files[@]}"; do
        if [[ -f "$config_file" ]]; then
            print_info "Removing $config_file"
            if run_privileged rm -f "$config_file"; then
                print_success "Removed $config_file"
                removed_any=true
            else
                print_error "Failed to remove $config_file"
            fi
        fi
    done
    
    if [[ "$removed_any" == "true" ]]; then
        print_success "Auto-loading configuration removed"
    else
        print_info "No auto-loading configuration found"
    fi
}

# Remove module files from system
remove_module_files() {
    print_info "Removing module files from system..."
    
    local kernel_version=$(uname -r)
    local module_paths=(
        "/lib/modules/$kernel_version/extra/akai_eie_pro.ko"
        "/lib/modules/$kernel_version/kernel/drivers/usb/audio/akai_eie_pro.ko"
        "/lib/modules/$kernel_version/updates/akai_eie_pro.ko"
        "/usr/lib/modules/$kernel_version/extra/akai_eie_pro.ko"
        "/usr/lib/modules/$kernel_version/kernel/drivers/usb/audio/akai_eie_pro.ko"
    )
    
    local removed_any=false
    for module_path in "${module_paths[@]}"; do
        if [[ -f "$module_path" ]]; then
            print_info "Removing $module_path"
            if run_privileged rm -f "$module_path"; then
                print_success "Removed $module_path"
                removed_any=true
            else
                print_error "Failed to remove $module_path"
            fi
        fi
    done
    
    if [[ "$removed_any" == "true" ]]; then
        print_success "Module files removed from system"
    else
        print_warning "No installed module files found"
    fi
}

# Update module dependencies
update_module_database() {
    print_info "Updating module dependency database..."
    
    if run_privileged depmod -a; then
        print_success "Module database updated"
    else
        print_error "Failed to update module database"
        return 1
    fi
}

# Remove any systemd service files (if they exist)
remove_systemd_services() {
    print_info "Checking for systemd service files..."
    
    local service_files=(
        "/etc/systemd/system/akai-eie-pro.service"
        "/usr/lib/systemd/system/akai-eie-pro.service"
        "/lib/systemd/system/akai-eie-pro.service"
    )
    
    local removed_any=false
    for service_file in "${service_files[@]}"; do
        if [[ -f "$service_file" ]]; then
            print_info "Removing $service_file"
            run_privileged systemctl disable akai-eie-pro.service 2>/dev/null || true
            if run_privileged rm -f "$service_file"; then
                print_success "Removed $service_file"
                removed_any=true
            else
                print_error "Failed to remove $service_file"
            fi
        fi
    done
    
    if [[ "$removed_any" == "true" ]]; then
        run_privileged systemctl daemon-reload
        print_success "SystemD services removed and reloaded"
    else
        print_info "No systemd service files found"
    fi
}

# Clean up any temporary files in the build directory
cleanup_build_files() {
    print_info "Cleaning up build files in current directory..."
    
    if [[ -f "Makefile" ]]; then
        if make clean 2>/dev/null; then
            print_success "Build artifacts cleaned"
        else
            print_info "Manual cleanup of build files..."
            rm -f *.ko *.o *.mod *.mod.c Module.symvers modules.order .*.cmd
            rm -rf .tmp_versions/
            print_success "Build files cleaned manually"
        fi
    else
        print_info "No Makefile found, skipping build cleanup"
    fi
}

# Verify complete removal
verify_removal() {
    print_info "Verifying complete removal..."
    
    local issues=0
    
    # Check if module is still loaded
    if lsmod | grep -q "akai_eie_pro"; then
        print_warning "Module is still loaded in kernel"
        ((issues++))
    fi
    
    # Check for remaining module files
    local kernel_version=$(uname -r)
    if find /lib/modules/$kernel_version -name "*akai*eie*" -o -name "*eie*pro*" 2>/dev/null | grep -q .; then
        print_warning "Some module files may still exist:"
        find /lib/modules/$kernel_version -name "*akai*eie*" -o -name "*eie*pro*" 2>/dev/null || true
        ((issues++))
    fi
    
    # Check for remaining config files
    if find /etc -name "*akai*eie*" 2>/dev/null | grep -q .; then
        print_warning "Some configuration files may still exist:"
        find /etc -name "*akai*eie*" 2>/dev/null || true
        ((issues++))
    fi
    
    if [[ $issues -eq 0 ]]; then
        print_success "Complete removal verified - no traces found"
    else
        print_warning "Found $issues potential issues - manual cleanup may be needed"
    fi
}

# Main uninstall function
main() {
    echo "======================================"
    echo "AKAI EIE Pro Driver Uninstall Script"
    echo "======================================"
    echo ""
    
    check_root
    
    echo ""
    print_info "Starting uninstallation process..."
    echo ""
    
    # Perform uninstallation steps
    unload_module
    echo ""
    
    remove_autoload_config
    echo ""
    
    remove_module_files
    echo ""
    
    update_module_database
    echo ""
    
    remove_systemd_services
    echo ""
    
    cleanup_build_files
    echo ""
    
    verify_removal
    echo ""
    
    print_success "AKAI EIE Pro driver uninstallation completed!"
    echo ""
    print_info "The driver has been removed from your system."
    print_info "If you had any audio applications using the device, you may need to restart them."
    print_info "A system reboot will ensure all changes take effect."
    echo ""
    
    # Ask about reboot
    read -p "Would you like to reboot now? (y/N): " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        print_info "Rebooting system..."
        run_privileged reboot
    else
        print_info "Reboot skipped. Remember to reboot later for complete cleanup."
    fi
}

# Run main function
main "$@"
#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# pcie_bind.sh — Helper script to bind/unbind PCI devices to pcie_stub driver.
#
# Usage:
#   ./scripts/pcie_bind.sh bind   0000:03:00.0    # unbind current driver, bind to pcie_access
#   ./scripts/pcie_bind.sh unbind 0000:03:00.0    # unbind from pcie_access
#   ./scripts/pcie_bind.sh status 0000:03:00.0    # show current driver
#   ./scripts/pcie_bind.sh list                    # list all pcie_access-bound devices

set -euo pipefail

DRIVER="pcie_access"
SYSFS_DRIVERS="/sys/bus/pci/drivers"
SYSFS_DEVICES="/sys/bus/pci/devices"

die() { echo "Error: $*" >&2; exit 1; }

usage() {
    echo "Usage: $0 { bind | unbind | status | list } [BDF]"
    echo ""
    echo "  bind   DDDD:BB:DD.F   Unbind device from current driver and bind to $DRIVER"
    echo "  unbind DDDD:BB:DD.F   Unbind device from $DRIVER"
    echo "  status DDDD:BB:DD.F   Show which driver owns the device"
    echo "  list                   List all devices bound to $DRIVER"
    exit 1
}

check_bdf() {
    local bdf="$1"
    [[ -d "$SYSFS_DEVICES/$bdf" ]] || die "Device $bdf not found in sysfs"
}

cmd_bind() {
    local bdf="$1"
    check_bdf "$bdf"

    # Unbind from current driver if any
    if [[ -L "$SYSFS_DEVICES/$bdf/driver" ]]; then
        local current
        current=$(basename "$(readlink "$SYSFS_DEVICES/$bdf/driver")")
        echo "Unbinding $bdf from $current..."
        echo "$bdf" > "$SYSFS_DEVICES/$bdf/driver/unbind" 2>/dev/null || true
    fi

    # Get VID:PID for new_id
    local vid pid
    vid=$(cat "$SYSFS_DEVICES/$bdf/vendor" | sed 's/0x//')
    pid=$(cat "$SYSFS_DEVICES/$bdf/device" | sed 's/0x//')

    # Ensure driver is loaded
    if [[ ! -d "$SYSFS_DRIVERS/$DRIVER" ]]; then
        die "$DRIVER driver not loaded. Run: sudo insmod pcie_stub.ko"
    fi

    # Add device ID and bind
    echo "Adding $vid $pid to $DRIVER new_id..."
    echo "$vid $pid" > "$SYSFS_DRIVERS/$DRIVER/new_id" 2>/dev/null || true

    echo "Binding $bdf to $DRIVER..."
    echo "$bdf" > "$SYSFS_DRIVERS/$DRIVER/bind" 2>/dev/null || true

    # Verify
    if [[ -L "$SYSFS_DEVICES/$bdf/driver" ]]; then
        local bound
        bound=$(basename "$(readlink "$SYSFS_DEVICES/$bdf/driver")")
        echo "✓ $bdf now bound to $bound"
        [[ -c "/dev/pcie_ctrl-$bdf" ]] && echo "✓ Device node: /dev/pcie_ctrl-$bdf"
    else
        die "Binding failed — check dmesg for details"
    fi
}

cmd_unbind() {
    local bdf="$1"
    check_bdf "$bdf"

    if [[ -L "$SYSFS_DEVICES/$bdf/driver" ]]; then
        echo "Unbinding $bdf..."
        echo "$bdf" > "$SYSFS_DEVICES/$bdf/driver/unbind"
        echo "✓ $bdf unbound"
    else
        echo "$bdf is not bound to any driver"
    fi
}

cmd_status() {
    local bdf="$1"
    check_bdf "$bdf"

    local vid pid
    vid=$(cat "$SYSFS_DEVICES/$bdf/vendor")
    pid=$(cat "$SYSFS_DEVICES/$bdf/device")
    echo "$bdf [$vid:$pid]"

    if [[ -L "$SYSFS_DEVICES/$bdf/driver" ]]; then
        local driver
        driver=$(basename "$(readlink "$SYSFS_DEVICES/$bdf/driver")")
        echo "  Driver: $driver"
    else
        echo "  Driver: (none)"
    fi

    if [[ -c "/dev/pcie_ctrl-$bdf" ]]; then
        echo "  Device: /dev/pcie_ctrl-$bdf"
    fi
}

cmd_list() {
    if [[ ! -d "$SYSFS_DRIVERS/$DRIVER" ]]; then
        echo "$DRIVER driver not loaded"
        return
    fi

    echo "Devices bound to $DRIVER:"
    local found=0
    for dev in "$SYSFS_DRIVERS/$DRIVER"/0*; do
        [[ -L "$dev" ]] || continue
        local bdf
        bdf=$(basename "$dev")
        echo "  $bdf → /dev/pcie_ctrl-$bdf"
        found=1
    done
    [[ $found -eq 0 ]] && echo "  (none)"
}

# Main
[[ $# -lt 1 ]] && usage

case "$1" in
    bind)   [[ $# -lt 2 ]] && usage; cmd_bind "$2" ;;
    unbind) [[ $# -lt 2 ]] && usage; cmd_unbind "$2" ;;
    status) [[ $# -lt 2 ]] && usage; cmd_status "$2" ;;
    list)   cmd_list ;;
    *)      usage ;;
esac

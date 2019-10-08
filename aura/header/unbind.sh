#!/bin/bash

# This is a simple utility to find the usb interface controlling
# the headers on an aura capable motherboard and unbind them
# from the driver. In practice a udev rule should be created.
#
# Note - This script requires root privelages

for path in /sys/bus/usb/devices/*; do
    vendor="$(cat "$path/idVendor" 2>/dev/null)"
    product="$(cat "$path/idProduct" 2>/dev/null)"

    if [ "$vendor" == "0b05" ] && [ "$product" == "1867" -o "$product" == "1872" ]; then
        if [ -f $path/driver/unbind ]; then

            # unbinding the device will prevent our driver from discovering it
            # so we need to unbind each interface (should only be one)
            for intf in $path:?.?; do
                device="$(basename $intf)"
                if echo "$device" > $intf/driver/unbind; then
                    echo "USB $vendor:$product unbound"
                fi
            done
        else
            echo "USB $vendor:$product not bound"
        fi
    fi
done

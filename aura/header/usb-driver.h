/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_AURA_HEADER_USB_DRIVER_H
#define _UAPI_AURA_HEADER_USB_DRIVER_H

/*
* A usb driver for the aura argb headers on asus motherboards.
*
* Copyright (C) 2019 Owen Parry
*
* Authors:
* Owen Parry <twifty@zoho.com>
*/
#include <include/types.h>

// #include "usb-transfer.h"
//
// struct usb_controller {
//
// };

#define PACKET_SIZE 65

struct usb_packet {
    char    data[PACKET_SIZE];
};

struct usb_controller {
    struct usb_device_id    *ids;
    const char              *name;
    uint8_t                 index;
    size_t                  packet_size;

    /* Private */
    struct usb_container    *container;
};

#define dump_packet(msg, p) ( \
    print_hex_dump_bytes( \
        msg, \
        DUMP_PREFIX_NONE, \
        p, \
        PACKET_SIZE ) \
)

// struct usb_transfer *usb_transfer_alloc (
//     struct usb_controller *ctrl
// );

error_t usb_read_packet (
    struct usb_controller * ctrl,
    struct usb_packet * packet
);

error_t usb_write_packet (
    struct usb_controller * ctrl,
    const struct usb_packet * packet
);

// error_t usb_send_transfer (
//     struct usb_controller *ctrl,
//     struct usb_transfer *xfer
// );

// struct usb_controller * usb_controller_create (
//     void
// );
//
// void usb_controller_destroy (
//     struct usb_controller * ctrl
// );

error_t usb_controller_register (
    struct usb_controller * ctrl
);

void usb_controller_unregister (
    struct usb_controller * ctrl
);

#endif

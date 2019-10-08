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
#include "../aura.h"
#include "usb-transfer.h"

struct usb_controller {

};

#define dump_packet(msg, p) print_hex_dump_bytes( \
    msg, \
    DUMP_PREFIX_NONE, \
    p, \
    PACKET_SIZE )

struct usb_transfer *usb_transfer_alloc (
    struct usb_controller *ctrl
);

error_t usb_read_packet (
    struct usb_controller *ctrl,
    void *packet
);

error_t usb_write_packet (
    struct usb_controller *ctrl,
    const void *packet
);

error_t usb_send_transfer (
    struct usb_controller *ctrl,
    struct usb_transfer *xfer
);

struct usb_controller *usb_controller_create (
    void
);

void usb_controller_destroy (
    struct usb_controller *ctrl
);

#endif

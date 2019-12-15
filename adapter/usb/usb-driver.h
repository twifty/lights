/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_USB_DRIVER_H
#define _UAPI_LIGHTS_ADAPTER_USB_DRIVER_H

/*
* A usb driver for the aura argb headers on asus motherboards.
*
* Copyright (C) 2019 Owen Parry
*
* Authors:
* Owen Parry <twifty@zoho.com>
*/
#include <linux/usb.h>
#include <include/types.h>

struct usb_client;

/**
 * struct usb_packet - Data transfered to device
 *
 * @length: Length of @data
 * @data:   Raw byte array
 */
struct usb_packet {
    size_t  length;
    char    *data;
};

/**
 * typedef usb_callback_t - Callback prototype
 */
typedef void (*usb_callback_t)(struct usb_client *);

/**
 * struct usb_client - Client data
 *
 * @ids:          Zero terminated array of ids to create a driver for
 * @name:         Name of created driver
 * @index:        Zero based index of mult device drivers
 * @packet_size:  Max data packet size accepted by the device
 * @onConnect:    Callback for connections
 * @onDisconnect: Callback for disconnections
 * @onSuspend:    Callback for suspends
 * @onResume:     Callback for resume from suspend
 * @controller:   Internal driver data
 */
struct usb_client {
    struct usb_device_id  const *ids;
    const char                  *name;
    uint8_t                     index;
    size_t                      packet_size;

    usb_callback_t              on_connect;
    usb_callback_t              on_disconnect;
    usb_callback_t              on_suspend;
    usb_callback_t              on_resume;

    /* Private */
    struct usb_controller       *controller;
};

/**
 * usb_read_packet() - Reads a packet from the device
 *
 * @client: Previously registered client
 * @packet: Data buffer to transfer
 *
 * @return: Error code
 *
 * This function is blocking.
 *
 * The given packet is first written to the device then populated with a
 * response. Individual devices, with the same product/vendor ids, can be
 * accessed by setting the index member of the client.
 */
error_t usb_read_packet (
    struct usb_client const *client,
    struct usb_packet *packet
);

/**
 * usb_write_packet() - Sends a packet to the device
 *
 * @client: Previously registered client
 * @packet: Data buffer to transfer
 *
 * @return: Error code
 *
 * This function is blocking.
 */
error_t usb_write_packet (
    struct usb_client const *client,
    struct usb_packet const *packet
);

/**
 * usb_controller_register() - Registers a client
 *
 * @client: Client to register
 *
 * @return: Error code.
 *
 * If a driver has not previously been created for the device ids contained
 * within the client, one will be created and reference counted. Each call
 * to register must have an accompanying call to unregister.
 *
 * Unless an onConnect() callback is registered, this function will wait for
 * up to 5 seconds for any device to be bound. The onConnect() function will
 * be called once for each device in the system. One client can be registered
 * to multiple devices, if they share the same vendor/product ids.
 */
error_t usb_controller_register (
    struct usb_client *client
);

/**
 * usb_controller_unregister() - Unregisters a client
 *
 * @client: Client to unregister
 *
 * If no more clients are bound to a device, the driver for that device
 * will be removed from the system.
 */
void usb_controller_unregister (
    struct usb_client *client
);

#endif

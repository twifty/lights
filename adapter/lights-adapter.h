/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_DRIVER_SMBUS_ADAPTER_H
#define _UAPI_DRIVER_SMBUS_ADAPTER_H

#include <linux/module.h>
#include <linux/i2c.h>

#include <include/quirks.h>
#include <include/types.h>

struct lights_adapter_msg;

typedef void (*lights_adapter_done_t) (struct lights_adapter_msg const * const result, void *data, error_t error);

/* Used to sanity check while loops */
#define LIGHTS_ADAPTER_MAX_MSGS 32

enum lights_adapter_flags {
    LIGHTS_CLIENT_PEC           = I2C_CLIENT_PEC,           /* Use Packet Error Checking */
    LIGHTS_CLIENT_TEN           = I2C_CLIENT_TEN,           /* we have a ten bit chip address */
    LIGHTS_CLIENT_SLAVE         = I2C_CLIENT_SLAVE,	        /* we are the slave */
    LIGHTS_CLIENT_HOST_NOTIFY   = I2C_CLIENT_HOST_NOTIFY,   /* We want to use I2C host notify */
    LIGHTS_CLIENT_WAKE          = I2C_CLIENT_WAKE,	        /* for board_info; true iff can wake */
    LIGHTS_CLIENT_SCCB          = I2C_CLIENT_SCCB,          /* Use Omnivision SCCB protocol */
};

enum lights_adapter_msg_type {
    MSG_QUICK       = I2C_SMBUS_QUICK,      /* 0 */
    MSG_BYTE        = I2C_SMBUS_BYTE,       /* 1 */
    MSG_BYTE_DATA   = I2C_SMBUS_BYTE_DATA,  /* 2 */
    MSG_WORD_DATA   = I2C_SMBUS_WORD_DATA,  /* 3 */
    MSG_BLOCK_DATA  = I2C_SMBUS_BLOCK_DATA, /* 5 */
};

enum lights_adapter_protocol {
    LIGHTS_PROTOCOL_SMBUS = 1,
    LIGHTS_PROTOCOL_I2C   = 2,
    LIGHTS_PROTOCOL_USB   = 3,
};

/**
 * struct lights_adapter - Storage for hardware access
 *
 * @proto:        One of the LIGHTS_PROTOCOL_ constants
 * @i2c_client:   I2C access data
 * @smbus_client: SMBUS access data
 * @adapter:      Async access data
 *
 * An instance of this may be created on the stack and used with
 * calls to @lights_adapter_xfer. However, @lights_adapter_xfer_asyns
 * calls will fail if the object has not been initialized with
 * @lights_adapter_register.
 */
struct lights_adapter_client {
    enum lights_adapter_protocol proto;
    union {
        struct i2c_client i2c_client;
        struct i2c_client smbus_client;
    };
    struct lights_adapter_context *adapter;
};
#define LIGHTS_I2C_CLIENT(_adapter, _addr, _flags) \
(struct lights_adapter_client){ \
    .proto = LIGHTS_PROTOCOL_I2C, \
    .i2c_client = { \
        .adapter = (_adapter), \
        .addr = (_addr), \
        .flags = (_flags) \
    } \
}
#define LIGHTS_I2C_CLIENT_UPDATE(_client, _addr) ( \
    (_client)->i2c_client.addr = (_addr) \
)
#define LIGHTS_SMBUS_CLIENT(_adapter, _addr, _flags) \
(struct lights_adapter_client){ \
     .proto = LIGHTS_PROTOCOL_SMBUS, \
     .smbus_client = { \
         .adapter = (_adapter), \
         .addr = (_addr), \
         .flags = (_flags) \
     } \
 }
#define LIGHTS_SMBUS_CLIENT_UPDATE(_client, _addr) ( \
    (_client)->smbus_client.addr = (_addr) \
)

/**
 * lights_adapter_xfer() - Synchronous reads/writes
 *
 * @client:    Hardware parameters
 * @msgs:      One or more messages to send
 * @msg_count: Number of messages to send
 *
 * @return: Zero or a negative error code
 *
 * There is no need to call @lights_adapter_register before using
 * this function.
 */
error_t lights_adapter_xfer (
    const struct lights_adapter_client *client,
    struct lights_adapter_msg *msgs,
    size_t msg_count
);

/**
 * lights_adapter_xfer_async() - Asynchronous reads/writes
 *
 * @client:    Hardware parameters
 * @msgs:      One or more messages to send
 * @msg_count: Number of messages to send
 * @cb_data:   Second parameter of @callback
 * @callback:  Completion function
 *
 * @return: Zero or a negative error code
 *
 * The async messages are queued until any running synchronous code
 * completes. Those blocking calls are then paused. Order of completion
 * for both async and sync is defined by whichever thread is given
 * the mutex lock.
 */
error_t lights_adapter_xfer_async (
    const struct lights_adapter_client *client,
    struct lights_adapter_msg *msgs,
    size_t msg_count,
    void *cb_data,
    lights_adapter_done_t callback
);

/**
 * lights_adapter_unregister() - Releases an async adapter
 *
 * @client: The client which holds the adapter
 *
 * See @lights_context_adapter_create for more info
 */
void lights_adapter_unregister (
    struct lights_adapter_client *client
);

/**
 * lights_context_adapter_create - Associates an I2C/SMBUS adapter
 *
 * @i2c_adapter: The adapter being wrapped
 * @max_async:   A maximum number of pending jobs
 *
 * @return: Zero or a negative error number
 *
 * This function MUST be called for any @lights_adapter_xfer_async
 * calls to succeed. It will create a reference counted adapter tied
 * to the underlying i2c/smbus/usb adapter. The reference to the adapter
 * is then stored in the given client.
 *
 * This was done so that clients can be created on the stack and hardware
 * probed without the need to allocate memory. One a bus has been validated
 * to contain the relevent device, the caller may then setup full async
 * access to the device.
 *
 * The actual async allocations are done on the first call to
 * @lights_adapter_xfer_async.
 *
 * Each call to @lights_adapter_register must be paired with a call to
 * @lights_adapter_unregister.
 */
error_t lights_adapter_register (
    struct lights_adapter_client *client,
    size_t max_async
);

/**
 * struct adapter_msg - I2c/SMBUS message data
 *
 * @read:    Truthy read/write
 * @command: Required for SMBUS transactions
 * @type:    One of the I2C_SMBUS_ constants
 * @length:  Only required for I2C_SMBUS_BLOCK_DATA
 * @swapped: Only for I2C_SMBUS_WORD_DATA
 * @data:    The value being sent between caller and hardware
 * @next:    The next message value
 */
struct lights_adapter_msg {
    u8      read;
    u8      command;
    u8      type;
    u8      length;
    u8      swapped;

    union {
        u8  byte;
        u16 word;
        u8  block[I2C_SMBUS_BLOCK_MAX];
    }       data;

    struct lights_adapter_msg *next;
};

/**
 * adapter_seek_msg - Access linked list as an array
 *
 * @head:  First linked list item
 * @index: Message offset from the head
 *
 * @return: The message or NULL if index doesn't exist
 */
static inline struct lights_adapter_msg const *adapter_seek_msg (
    struct lights_adapter_msg const *head,
    size_t index
){
    if (!head)
        return NULL;

    while (head && index > 0) {
        index--;
        head = head->next;
    }

    return head;
}

#define ADAPTER_READ_MSG(_reg, _type, _swapped) \
(struct lights_adapter_msg){ \
    .read = true, \
    .type = (_type), \
    .command = (_reg), \
    .swapped = (_swapped) \
}

#define ADAPTER_WRITE_MSG(_reg, _type, _data, _swapped) \
(struct lights_adapter_msg){ \
    .type = (_type), \
    .command = (_reg), \
    .data = _data, \
    .swapped = (_swapped) \
}

#define ADAPTER_READ_BYTE() \
    ADAPTER_READ_MSG(0, I2C_SMBUS_BYTE, false)

#define ADAPTER_READ_BYTE_DATA(_reg) \
    ADAPTER_READ_MSG((_reg), I2C_SMBUS_BYTE_DATA, false)

#define ADAPTER_READ_WORD_DATA(_reg) \
    ADAPTER_READ_MSG((_reg), I2C_SMBUS_WORD_DATA, false)

#define ADAPTER_READ_WORD_DATA_SWAPPED(_reg) \
    ADAPTER_READ_MSG((_reg), I2C_SMBUS_WORD_DATA, true)

#define ADAPTER_READ_BLOCK_DATA(_reg) \
    ADAPTER_READ_MSG((_reg), I2C_SMBUS_BLOCK_DATA, false)

#define ADAPTER_WRITE_BYTE(_val) \
    ADAPTER_WRITE_MSG(0, I2C_SMBUS_BYTE, {.byte = (u8)(_val)}, false)

#define ADAPTER_WRITE_BYTE_DATA(_reg, _val) \
    ADAPTER_WRITE_MSG((_reg), I2C_SMBUS_BYTE_DATA, {.byte = (u8)(_val)}, false)

#define ADAPTER_WRITE_WORD_DATA(_reg, _val) \
    ADAPTER_WRITE_MSG((_reg), I2C_SMBUS_WORD_DATA, {.word = (u16)(_val)}, false)

#define ADAPTER_WRITE_WORD_DATA_SWAPPED(_reg, _val) \
    ADAPTER_WRITE_MSG((_reg), I2C_SMBUS_WORD_DATA, {.word = (u16)(_val)}, true)

/* NOTE: The caller is required to populate the data */
#define ADAPTER_WRITE_BLOCK_DATA(_reg, _len) \
(struct lights_adapter_msg){ \
    .type = I2C_SMBUS_BLOCK_DATA, \
    .command = (_reg), \
    .length = (_len), \
}
#define adapter_assign_block_data(_msg, _data, _len)( \
    memcpy((_msg)->data.block, (_data), (_len) > I2C_SMBUS_BLOCK_MAX ? I2C_SMBUS_BLOCK_MAX : (_len)) \
)

#endif

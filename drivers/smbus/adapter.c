// SPDX-License-Identifier: GPL-2.0
#include "adapter.h"

int smbus_read (
    struct hw_device *const hw_dev,
    void *_msgs,
    size_t count
){
    struct smbus_adapter * const adapter = to_smbus_adapter(hw_dev);
    struct smbus_msg *msgs = _msgs, *msg;
    struct i2c_client client = {0};
    s32 result;
	int i;

    if (WARN_ON(NULL == hw_dev || NULL == msgs || 0 == count))
        return -EINVAL;

    client.adapter = adapter->i2c_adapter;

    async_queue_pause(adapter->async_queue);

    for (i = 0; i < count; i++) {
        msg = &msgs[i];

        client.addr    = msg->addr;
        client.flags   = msg->flags;

        switch (msgs->type) {
            case I2C_SMBUS_BYTE:
                result = i2c_smbus_read_byte(&client);
                msg->data.byte = (u8)result;
                break;
            case I2C_SMBUS_BYTE_DATA:
                result = i2c_smbus_read_byte_data(&client, msg->command);
                msg->data.byte = (u8)result;
                break;
            case I2C_SMBUS_WORD_DATA:
                if (msg->swapped)
                    result = i2c_smbus_read_word_swapped(&client, msg->command);
                else
                    result = i2c_smbus_read_word_data(&client, msg->command);
                msg->data.word = (u16)result;
                break;
            case I2C_SMBUS_BLOCK_DATA:
                result = i2c_smbus_read_block_data(&client, msg->command, msg->data.block);
                msg->length = (u8)result;
                break;
            default:
                return -EINVAL;
        }

        if (result < 0)
            goto error;
    }

error:
    async_queue_resume(adapter->async_queue);

    return result < 0 ? result : 0;
}

int smbus_write (
    struct hw_device *const hw_dev,
    void const *_msgs,
    size_t count
){
    struct smbus_adapter * const adapter = to_smbus_adapter(hw_dev);
    struct smbus_msg const *msgs = _msgs, *msg;
    struct i2c_client client = {0};
    s32 result;
	int i;

    if (WARN_ON(NULL == hw_dev || NULL == msgs || 0 == count))
        return -EINVAL;

    client.adapter = adapter->i2c_adapter;

    async_queue_pause(adapter->async_queue);

    for (i = 0; i < count; i++) {
        msg = &msgs[i];

        client.addr    = msg->addr;
        client.flags   = msg->flags;

        switch (msgs->type) {
            case I2C_SMBUS_BYTE:
                result = i2c_smbus_write_byte(&client, msg->data.byte);
                break;
            case I2C_SMBUS_BYTE_DATA:
                result = i2c_smbus_write_byte_data(&client, msg->command, msg->data.byte);
                break;
            case I2C_SMBUS_WORD_DATA:
                if (msg->swapped)
                    result = i2c_smbus_write_word_swapped(&client, msg->command, msg->data.word);
                else
                    result = i2c_smbus_write_word_data(&client, msg->command, msg->data.word);
                break;
            case I2C_SMBUS_BLOCK_DATA:
                result = i2c_smbus_write_block_data(&client, msg->command, msg->length, msg->data.block);
                break;
            default:
                return -EINVAL;
        }

        if (result < 0)
            goto error;
    }

error:
    async_queue_resume(adapter->async_queue);

    return result < 0 ? result : 0;
}

static void __async_execute (void *data) {

}

int smbus_read_async (
    struct hw_device* const hw_dev,
    void *_msgs,
    size_t count,
    void *cb_data,
    hw_done_t cb
){
    struct smbus_adapter * const adapter = to_smbus_adapter(hw_dev);
    struct smbus_msg const *msgs = _msgs, *msg;

    if (WARN_ON(NULL == hw_dev || NULL == _msgs || 0 == count))
        return -EINVAL;

    async_queue_add(adapter->async_queue, );

    return 0;
}

int smbus_write_async (
    struct hw_device* const hw_dev,
    void const *_msgs,
    size_t count,
    void *cb_data,
    hw_done_t cb
){
    struct smbus_msg *msgs = _msgs;
    union i2c_smbus_data data;
	int status, i;

    if (WARN_ON(NULL == msgs))
        return -EINVAL;

    for (i = 0; i < count; i++) {
        // switch (msgs[i].type) {
        //     case I2C_SMBUS_QUICK:
        //     case I2C_SMBUS_BYTE:
        //     case I2C_SMBUS_BYTE_DATA:
        //     case I2C_SMBUS_WORD_DATA:
        //     case I2C_SMBUS_PROC_CALL:
        //     case I2C_SMBUS_BLOCK_DATA:
        //     case I2C_SMBUS_I2C_BLOCK_BROKEN:
        //     case I2C_SMBUS_BLOCK_PROC_CALL:
        //     case I2C_SMBUS_I2C_BLOCK_DATA:
        // }
    }

    return 0;
};

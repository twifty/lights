// SPDX-License-Identifier: GPL-2.0
#include "aura-smbus.h"

#define MUX_NAME "i2c-nct6775"

/* Nuvoton SMBus address offsets */
#define SMBHSTDAT               (0x0 + smba)
#define SMBBLKSZ                (0x1 + smba)
#define SMBHSTCMD               (0x2 + smba)
#define SMBHSTIDX               (0x3 + smba)
#define SMBHSTCTL               (0x4 + smba)
#define SMBHSTADD               (0x5 + smba)
#define SMBHSTERR               (0x9 + smba)
#define SMBHSTSTS               (0xE + smba)

/* request_region size */
#define SMBIOSIZE               0xE

/* Command register */
#define NUVOTON_READ_BYTE       0
#define NUVOTON_READ_WORD       1
#define NUVOTON_READ_BLOCK      2
#define NUVOTON_WRITE_BYTE      8
#define NUVOTON_WRITE_WORD      9
#define NUVOTON_WRITE_BLOCK     10

/* Control register */
#define NUVOTON_MANUAL_START    128
#define NUVOTON_SOFT_RESET      64

/* Error register */
#define NUVOTON_NO_ACK          32

/* Status register */
#define NUVOTON_FIFO_EMPTY      1
#define NUVOTON_MANUAL_ACTIVE   4

// Delay period in usecs
#define DELAY_PERIOD            10

const struct chip_id {
    const char *name;
    uint16_t id;
} nuvoton_chips[] = {
    { "NCT6106", 0xc450 },
    { "NCT6775", 0xb470 },
    { "NCT6776", 0xc330 },
    { "NCT6779", 0xc560 },
    { "NCT6791", 0xc800 },
    { "NCT6792", 0xc910 },
    { "NCT6793", 0xd120 },
    { "NCT6795", 0xd350 },
    { "NCT6796", 0xd420 },
    { "NCT6798", 0xd428 }
};

struct aura_smbus_context {
    struct aura_smbus_adapter   smbus_adapter;
    uint16_t                    smba;
    struct i2c_adapter          adapter;
};

#define smbus_context(ptr)\
    container_of(ptr, struct aura_smbus_context, smbus_adapter)

static void fill_output_buffer (
    uint16_t smba,
    union i2c_smbus_data *data
){
    uint8_t len;
    uint8_t *ptr;
    int i;

    /* First byte is always the byte count */
    len = data->block[0];
    ptr = &data->block[1];

    outb_p(len, SMBBLKSZ);

    do {
        /* Buffer takes 4 bytes at a time */
        for (i = 0; i < 4 && len > 0; i++, len--)
            outb_p((*ptr)++, SMBHSTDAT);

        while ((inb_p(SMBHSTSTS) & NUVOTON_FIFO_EMPTY) == 0)
            udelay(DELAY_PERIOD);
    } while (len);
}

static error_t aura_smbus_nuvoton_transfer (
    struct i2c_adapter * adapter,
    uint16_t addr,
    uint16_t flags,
    char read_write,
    uint8_t command,
    int size,
    union i2c_smbus_data * data
){
    struct aura_smbus_context *ctx = i2c_get_adapdata(adapter);
    uint16_t smba = ctx->smba;

    /* Reset the bus */
    outb_p(NUVOTON_SOFT_RESET, SMBHSTCTL);

    /* Configure the transaction */
    switch (size) {
    case I2C_SMBUS_QUICK:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        break;
    case I2C_SMBUS_BYTE:
    case I2C_SMBUS_BYTE_DATA:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        outb_p(command, SMBHSTIDX);
        if (read_write == I2C_SMBUS_WRITE) {
            outb_p(data->byte, SMBHSTDAT);
            outb_p(NUVOTON_WRITE_BYTE, SMBHSTCMD);
        }
        else {
            outb_p(NUVOTON_READ_BYTE, SMBHSTCMD);
        }
        break;
    case I2C_SMBUS_WORD_DATA:
        outb_p((addr << 1) | read_write, SMBHSTADD);
        outb_p(command, SMBHSTIDX);
        if (read_write == I2C_SMBUS_WRITE) {
            outb_p(data->word & 0xff, SMBHSTDAT);
            outb_p((data->word & 0xff00) >> 8, SMBHSTDAT);
            outb_p(NUVOTON_WRITE_WORD, SMBHSTCMD);
        }
        else {
            outb_p(NUVOTON_READ_WORD, SMBHSTCMD);
        }
        break;
    case I2C_SMBUS_BLOCK_DATA:
        if (read_write != I2C_SMBUS_WRITE)
            return -ENOTSUPP;
        if (data->block[0] == 0 || data->block[0] > I2C_SMBUS_BLOCK_MAX)
            return -EINVAL;
        outb_p((addr << 1) | read_write, SMBHSTADD);
        outb_p(command, SMBHSTIDX);
        outb_p(NUVOTON_WRITE_BLOCK, SMBHSTCMD);
        fill_output_buffer(smba, data);
        break;
    default:
        dev_warn(&adapter->dev, "Unsupported transaction %d", size);
        return -EOPNOTSUPP;
    }

    /* Begin and wait for transaction completion */
    outb_p(NUVOTON_MANUAL_START, SMBHSTCTL);
    while ((inb_p(SMBHSTSTS) & NUVOTON_MANUAL_ACTIVE) != 0)
        udelay(DELAY_PERIOD);

    /* Check the status */
    if ((inb_p(SMBHSTERR) & NUVOTON_NO_ACK) != 0)
        return -ENXIO;
    else if ((read_write == I2C_SMBUS_WRITE) || (size == I2C_SMBUS_QUICK))
        return 0;

    /* Read a response data */
    switch (size) {
    case I2C_SMBUS_QUICK:
    case I2C_SMBUS_BYTE_DATA:
        data->byte = inb_p(SMBHSTDAT);
        break;
    case I2C_SMBUS_WORD_DATA:
        data->word = inb_p(SMBHSTDAT) + (inb_p(SMBHSTDAT) << 8);
        break;
    }

    return 0;
}

static uint32_t aura_smbus_nuvoton_func (
    struct i2c_adapter *adapter
){
    return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
        I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
        I2C_FUNC_SMBUS_BLOCK_DATA;
}

static const struct i2c_algorithm aura_smbus_nuvoton_algorithm = {
    .smbus_xfer     = aura_smbus_nuvoton_transfer,
    .functionality  = aura_smbus_nuvoton_func,
};

static error_t aura_smbus_nuvoton_read_smba (
    uint16_t *smba
){
    const struct chip_id *chip;
    uint8_t super_io_addr[2] = { 0x2e, 0x4e };
    uint8_t addr;
    uint16_t value;
    int i, c;

    chip = NULL;
    for (i = 0; i < ARRAY_SIZE(super_io_addr); i++) {
        addr = super_io_addr[i];

        /* Use the hwmon name */
        if (!request_muxed_region(addr, 2, MUX_NAME))
            return -EBUSY;

        // Enter the super i/o
        outb(0x87, addr);
        outb(0x87, addr);

        // The device id should be offset 0x20 and 0x21
        outb(0x20, addr);
        value = (inb(addr + 1) << 8);
        outb(0x21, addr);
        value |= (inb(addr + 1) & 0xff);

        for (c = 0; c < ARRAY_SIZE(nuvoton_chips); c++) {
            if (value == nuvoton_chips[c].id) {
                chip = &nuvoton_chips[c];
                break;
            }
        }

        if (NULL != chip) {
            // Select logical device B
            outb(0x07, addr);      // Logical device selection
            outb(0x0b, addr + 1);  // SMBus selection

            // The SMBus addr should be offset 0x62 and 0x63
            outb(0x62, addr);
            value = (inb(addr + 1) << 8);
            outb(0x63, addr);
            value |= (inb(addr + 1) & 0xff);
            value &= 0xfff8;

            if (value) {
                // AURA_DBG("Detected a Nuvoton %s SMBus at 0x%04x", chip->name, value);
                *smba = value;
            } else {
                chip = NULL;
            }
        }

        // Exit the super i/o
        outb(0xaa, addr);
        outb(0x02, addr);
        outb(0x02, addr + 1);

        release_region(addr, 2);

        // Select the first chip found
        if (chip)
            return 0;
    }

    return -ENODEV;
}

static struct aura_smbus_context *aura_smbus_nuvoton_context_create (
    void
){
    struct aura_smbus_context *ctx;
    uint16_t smba;
    error_t err;

    /* Determine the address of the SMBus areas */
    err = aura_smbus_nuvoton_read_smba(&smba);
    if (err)
        return ERR_PTR(err);

    if (acpi_check_region(smba, SMBIOSIZE, "nuvoton_smbus"))
        return ERR_PTR(-ENODEV);

    AURA_INFO("Nuvoton SMBus Host Controller at 0x%x", smba);

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return ERR_PTR(-ENOMEM);

    ctx->smba = smba;

    return ctx;
}

static void aura_smbus_nuvoton_adapter_destroy (
    struct aura_smbus_adapter *smbus_adapter
){
    struct aura_smbus_context *context = smbus_context(smbus_adapter);

    i2c_del_adapter(&context->adapter);
    kfree(context);
}

struct aura_smbus_adapter *aura_smbus_nuvoton_adapter_create (
    void
){
    struct aura_smbus_context *context;
    error_t err;

    context = aura_smbus_nuvoton_context_create();
    if (IS_ERR(context))
        return ERR_CAST(context);

    context->adapter.owner = THIS_MODULE;
    context->adapter.class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
    context->adapter.algo  = &aura_smbus_nuvoton_algorithm;
    // context->adapter.dev.parent = &pci_dev->dev;

    snprintf(context->adapter.name, sizeof(context->adapter.name),
        "AURA MB adapter (nuvoton) at %04x", context->smba);

    i2c_set_adapdata(&context->adapter, context);

    err = i2c_add_adapter(&context->adapter);
    if (err) {
        kfree(context);
        return ERR_PTR(err);
    }

    context->smbus_adapter.adapter = &context->adapter;
    context->smbus_adapter.destroy = aura_smbus_nuvoton_adapter_destroy;

    return &context->smbus_adapter;
}

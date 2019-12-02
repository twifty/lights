/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_INTERFACE_H
#define _UAPI_LIGHTS_ADAPTER_INTERFACE_H

#include <linux/types.h>
#include <linux/sysfs.h>
#include <include/types.h>

#define LIGHTS_MAX_FILENAME_LENGTH  64
#define LIGHTS_MAX_MODENAME_LENGTH  32

#define LIGHTS_IO_MODE  "mode"
#define LIGHTS_IO_COLOR "color"
#define LIGHTS_IO_SPEED "speed"
#define LIGHTS_IO_LEDS  "leds"

/*
    These ids represent common modes supported by a majority
    of devices. A third party may extend upon these by using
    the hi byte. Modes are considered equal if the low byte
    id is equal (disregards string) OR hi byte id is equal
    AND string name is equal (case sensitive).
 */
enum lights_mode_id {
    LIGHTS_MODE_ENDOFARRAY   = 0x0000, /* The last item of an array should be a zeroed object*/
    LIGHTS_MODE_OFF          = 0x0001,
    LIGHTS_MODE_STATIC       = 0x0002,
    LIGHTS_MODE_BREATHING    = 0x0003,
    LIGHTS_MODE_FLASHING     = 0x0004,
    LIGHTS_MODE_CYCLE        = 0x0005,
    LIGHTS_MODE_RAINBOW      = 0x0006,
};

#define LIGHTS_MODE_LABEL_OFF       "off"
#define LIGHTS_MODE_LABEL_STATIC    "static"
#define LIGHTS_MODE_LABEL_BREATHING "breathing"
#define LIGHTS_MODE_LABEL_FLASHING  "flashing"
#define LIGHTS_MODE_LABEL_CYCLE     "cycle"
#define LIGHTS_MODE_LABEL_RAINBOW   "rainbow"

#define LIGHTS_MODE(_name) \
    { .id = LIGHTS_MODE_ ## _name, .name = LIGHTS_MODE_LABEL_ ## _name }

#define LIGHTS_CUSTOM_MODE(_id, _name) \
    { .id = ((_id & 0xff) << 8), .name = _name }

#define LIGHTS_MODE_LAST_ENTRY() \
    { .id = LIGHTS_MODE_ENDOFARRAY }

#define lights_is_custom_mode(mode) ( \
    ((mode)->id & 0xff00) \
)

#define lights_custom_mode_id(mode) ( \
    (((mode)->id & 0xff00) >> 8) \
)

#define lights_mode_id(mode) ( \
    ((mode)->id & 0xff) \
)

#define lights_for_each_mode(p, a)                      \
    for (p = a;                                         \
         p->id != LIGHTS_MODE_ENDOFARRAY && p->name;    \
         p++)

/**
 * struct lights_color - Storage for 3 color values
 *
 * @a: The alpha value (currently unused)
 * @r: The red value
 * @g: The green value
 * @b: The blue value
 * @value: Combine a r g b value (in that order)
 */
struct lights_color {
    union {
        struct {
#ifdef __LITTLE_ENDIAN
            uint8_t b;
            uint8_t g;
            uint8_t r;
            uint8_t a;
#else
            uint8_t a;
            uint8_t r;
            uint8_t g;
            uint8_t b;
#endif
        };
        uint32_t value;
    };
};

#define lights_color_equal(p1, p2) ( \
    (p1)->value == (p2)->value \
)

/**
 * struct lights_mode
 *
 * @id:   The numeric value of the mode
 * @name: max LIGHTS_MAX_MODENAME_LENGTH, snake_case name of the mode
 *
 * Global values (defined within this module) use the lo byte
 * All third party extensions must use the hi byte
 */
struct lights_mode {
    uint16_t    id;
    const char  *name;
};

#define lights_mode_equal(p1, p2) ( \
    ((p1)->id == (p2)->id) \
)

struct lights_buffer {
    ssize_t         length;
    uint8_t         *data;
    loff_t          offset;
};

/**
 * struct lights_state
 *
 * @color: The global color value
 * @mode:  The global mode value
 * @speed: The global speed value. This value ranges from 1 to 5 each
 *         representing how a second is to be divided
 */
struct lights_state {
    struct lights_color     color;
    struct lights_mode      mode;
    uint8_t                 speed;
};

/**
 * enum lights_io_type - @lights_io data type flag
 *
 * @LIGHTS_TYPE_CUSTOM: @lights_buffer is used (For future use)
 * @LIGHTS_TYPE_MODE:   @lights_mode is used
 * @LIGHTS_TYPE_COLOR:  @lights_color is used
 * @LIGHTS_TYPE_SPEED:  uint8_t is used
 * @LIGHTS_TYPE_LEDS:   @lights_buffer is used
 */
enum lights_io_type {
    LIGHTS_TYPE_CUSTOM,
    LIGHTS_TYPE_MODE,
    LIGHTS_TYPE_COLOR,
    LIGHTS_TYPE_SPEED,
    LIGHTS_TYPE_LEDS,
};

/**
 * struct lights_io - Used during read/write callbacks
 *
 * @type:       Indicates which of the data members is in use
 * @data.color: A color value being read from or written to the file
 * @data.mode:  A mode value being read from or written to the file
 * @data.speed: A speed value being read from or written to the file
 * @data.raw:   The raw data, in kernel space
 *
 * NOTE - LIGHTS_TYPE_LEDS uses the raw buffer. The size of the buffer
 * is dependant on the led_count member of lights_dev.
 */
struct lights_io {
    enum lights_io_type         type;

    union {
        struct lights_color     color;
        struct lights_mode      mode;
        uint8_t                 speed;
        struct lights_buffer    raw;
    } data;
};

/**
 * struct lights_io_attribute - Specifies how a file is to be created
 *
 * @owner:        The owning module (THIS_MODULE)
 * @attr:         Specifies the name and mode of a file
 * @type:         Specifies how data written/read is translated
 * @private_data: A user defined pointer to data associated with the file
 * @read:         Called to populate the io object
 * @write:        Called with populated io
 *
 * The @private_data member is passed to the callbacks. This allows
 * for extensions to associate arbitrary data with each file.
 */
struct lights_io_attribute {
    struct module           *owner;
    struct attribute        attr;
    enum lights_io_type     type;
    void                    *private_data;

    error_t (*read)(void *data, struct lights_io *io);
    error_t (*write)(void *data, const struct lights_io *io);
};

#define LIGHTS_ATTR(_name, _mode, _type, _data, _read, _write)      \
(struct lights_io_attribute) {                                      \
    .owner = THIS_MODULE,                                           \
    .attr = {.name = _name,                                         \
             .mode = VERIFY_OCTAL_PERMISSIONS(_mode) },             \
    .type          = _type,                                         \
    .private_data  = _data,                                         \
    .read          = _read,                                         \
    .write         = _write,                                        \
}

/**
 * Helper macros for creating lights_io_attribute instances.
 */
#define LIGHTS_ATTR_RO(_name, _type, _data, _read) (    \
    LIGHTS_ATTR(_name, 0444, _type, _data, _read, NULL) \
)

#define LIGHTS_ATTR_WO(_name, _type, _data, _write) ( \
    LIGHTS_ATTR(_name, 0200, _type, _data, NULL, _write) \
)

#define LIGHTS_ATTR_RW(_name, _type, _data, _read, _write) ( \
    LIGHTS_ATTR(_name, 0644, _type, _data, _read, _write)    \
)

#define LIGHTS_CUSTOM_ATTR(_name, _data, _read, _write) ( \
    LIGHTS_ATTR_RW(_name, LIGHTS_TYPE_CUSTOM, _data, _read, _write) \
)

#define LIGHTS_MODE_ATTR(_data, _read, _write) ( \
    LIGHTS_ATTR_RW(LIGHTS_IO_MODE, LIGHTS_TYPE_MODE, _data, _read, _write) \
)

#define LIGHTS_COLOR_ATTR(_data, _read, _write) ( \
    LIGHTS_ATTR_RW(LIGHTS_IO_COLOR, LIGHTS_TYPE_COLOR, _data, _read, _write) \
)

#define LIGHTS_SPEED_ATTR(_data, _read, _write) ( \
    LIGHTS_ATTR_RW(LIGHTS_IO_SPEED, LIGHTS_TYPE_SPEED, _data, _read, _write) \
)

#define LIGHTS_LEDS_ATTR(_data, _write) ( \
    LIGHTS_ATTR_WO(LIGHTS_IO_LEDS, LIGHTS_TYPE_LEDS, _data, _write) \
)

/**
 * struct lights_dev - Describes the device files to be created
 *
 * @name:         max LIGHTS_MAX_FILENAME_LENGTH, of directory name within
 *                the /dev/lights/ directory
 * @led_count:    The number of leds supported by the device
 * @caps:         A list of modes supported by the device
 * @attrs:        A null terminated array of io attributes
 * @update_color: A hook into the writing of /dev/lights/all/color
 * @update_mode:  A hook into the writing of /dev/lights/all/mode
 * @update_speed: A hook into the writing of /dev/lights/all/speed
 *
 * The modes listed here are available to userland in the 'caps' file. This
 * file is created for each device when modes are given. Each mode is also
 * added to a global list, available at /dev/lights/all/caps. The modes listed
 * in this file are those which are defined by THIS module AND which are common
 * to each extension.
 */
struct lights_dev {
    const char                          *name;
    uint16_t                            led_count;
    const struct lights_mode            *caps;
    const struct lights_io_attribute    **attrs;

    error_t (*update_color)(const struct lights_state *);
    error_t (*update_mode)(const struct lights_state *);
    error_t (*update_speed)(const struct lights_state *);
};

/**
 * lights_device_register() - Registers a new lights device
 *
 * @dev:    A decriptor of the device and its files
 * @Return: A negative error number on failure
 *
 * When a device with the same exists, a value of EEXISTS is returned.
 * The best practice for naming, is to append a hyphen and digit to the
 * names of devices for which multiple instances may exist ("dimm-0").
 */
error_t lights_device_register (
    struct lights_dev * dev
);

/**
 * lights_device_unregister() - Removes a device
 *
 * @dev: A device previously registered with lights_device_register()
 */
void lights_device_unregister (
    struct lights_dev * dev
);

/**
 * lights_create_file() - Adds a file to the devices directory
 *
 * @dev:  A previously registered device
 * @attr: A description of the file to create
 *
 * @Return: A negative error number on failure
 *
 * The given attribute may exist on the stack. Internally it is copied
 * so the user need not keep a reference to it.
 */
error_t lights_create_file (
    struct lights_dev *dev,
    struct lights_io_attribute *attr
);

/**
 * lights_read_color() - Helper for reading color value strings
 *
 * @buffer: A kernel/user buffer containing the string
 * @len:    The length of the buffer
 * @color:  A color object to populate
 *
 * @Return: The number of characters read or a negative error number
 */
ssize_t lights_read_color (
    const char *buffer,
    size_t len,
    struct lights_color *color
);

/**
 * lights_read_mode() - Helper for reading mode value strings
 *
 * @buffer:   A kernel/user buffer containing the string
 * @len:      The length of the buffer
 * @haystack: A list of valid modes to match against
 * @mode:     A mode object to populate
 *
 * @Return: The number of characters read or a negative error number
 */
ssize_t lights_read_mode (
    const char *buffer,
    size_t len,
    const struct lights_mode *haystack,
    struct lights_mode *mode
);

/**
 * lights_read_speed() - Helper for reading speed value strings
 *
 * @buffer: A kernel/user buffer containing the string
 * @len:    The length of the buffer
 * @speed:  A value to populate with the speed
 *
 * @Return: The number of characters read or a negative error number
 */
ssize_t lights_read_speed (
    const char *buffer,
    size_t len,
    uint8_t *speed
);

/**
 * lights_get_params() - Retrieves the current global state
 *
 * @state: An object to populate with the state
 */
void lights_get_state (
    struct lights_state *state
);

#endif /* _UAPI_LIGHTS_H */

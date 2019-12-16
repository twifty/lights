/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_INTERFACE_H
#define _UAPI_LIGHTS_ADAPTER_INTERFACE_H

#include <linux/types.h>
#include <linux/sysfs.h>
#include <include/types.h>

#include "lights-thunk.h"

#define LIGHTS_MAX_FILENAME_LENGTH  64
#define LIGHTS_MAX_MODENAME_LENGTH  32

#define LIGHTS_IO_MODE      "mode"
#define LIGHTS_IO_COLOR     "color"
#define LIGHTS_IO_SPEED     "speed"
#define LIGHTS_IO_DIRECTION "direction"
#define LIGHTS_IO_LEDS      "leds"
#define LIGHTS_IO_SYNC      "sync"
#define LIGHTS_IO_UPDATE    "update"

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

static inline void lights_color_read_rgb (
    struct lights_color *color,
    const uint8_t buf[3]
){
    color->r = buf[0];
    color->g = buf[1];
    color->b = buf[2];
}

static inline void lights_color_write_rgb (
    struct lights_color const *color,
    uint8_t buf[3]
){
    buf[0] = color->r;
    buf[1] = color->g;
    buf[2] = color->b;
}

static inline void lights_color_read_rbg (
    struct lights_color *color,
    const uint8_t buf[3]
){
    color->r = buf[0];
    color->b = buf[1];
    color->g = buf[2];
}

static inline void lights_color_write_rbg (
    struct lights_color const *color,
    uint8_t buf[3]
){
    buf[0] = color->r;
    buf[1] = color->b;
    buf[2] = color->g;
}

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
    void            *data;
    loff_t          offset;
};

/**
 * enum lights_state_type - @lights_io data type flag
 *
 * These types are intended to be used in a bitmap. Each bit signifies
 * which member of struct lights_state has been initialized.
 *
 * During file IO operations, a combination of these values will be passed
 * to callback function. With a few exceptions:
 *    LIGHTS_TYPE_LEDS can never be combined with LIGHTS_TYPE_CUSTOM
 *    LIGHTS_TYPE_UPDATE is replaced with all except LEDS and CUSTOM
 *
 * TODO - remove UPDATE, the file should be default for all
 */
enum lights_state_type {
    LIGHTS_TYPE_MODE        = 0x01,
    LIGHTS_TYPE_COLOR       = 0x02,
    LIGHTS_TYPE_SPEED       = 0x04,
    LIGHTS_TYPE_DIRECTION   = 0x08,
    LIGHTS_TYPE_LEDS        = 0x10,
    LIGHTS_TYPE_CUSTOM      = 0x20,
    LIGHTS_TYPE_SYNC        = 0x40,
    LIGHTS_TYPE_UPDATE      = 0x80,
};

/**
 * struct lights_state
 *
 * @mode:      The global mode value (requires LIGHTS_TYPE_MODE)
 * @color:     The global color value (requires LIGHTS_TYPE_COLOR)
 * @raw:       Custom or LED data (requires LIGHTS_TYPE_LEDS or LIGHTS_TYPE_CUSTOM
 *             it's an error for these two values to appear together)
 * @speed:     The global speed value. (requires LIGHTS_TYPE_SPEED) This value
 *             ranges from 0 to 5 each representing how a second is to be divided
 * @direction: 0 or 1 to indicate clockwise or anti-clockwise. (requires LIGHTS_TYPE_DIRECTION)
 * @sync:      A value between 0 and 256, set to multiple devices simultaniously.
 *             The value indicates at which step a particular mode cycle should
 *             be in. The device should adjust accordingly.
 *                 Not all devices run at the same speed and not all same-named
 *             effects run for the same duration. The helps keep those devices
 *             on different busses in sync with each other.
 * @type:      One or more type values.
 */
struct lights_state {
    struct lights_mode      mode;
    struct lights_color     color;
    struct lights_buffer    raw;
    uint8_t                 speed;
    uint8_t                 direction;
    uint8_t                 sync;
    enum lights_state_type  type;
};

typedef error_t (*lights_read_t)(struct lights_thunk *, struct lights_state *);
typedef error_t (*lights_write_t)(struct lights_thunk *, struct lights_state const *);

/**
 * struct lights_io_attribute - Specifies how a file is to be created
 *
 * @owner: The owning module (THIS_MODULE)
 * @attr:  Specifies the name and mode of a file
 * @type:  Specifies how data written/read is translated
 * @data:  A user defined pointer to data associated with the file
 * @read:  Called to populate the io object
 * @write: Called with populated io
 *
 * The @private_thunk member is passed to the callbacks. This allows
 * for extensions to associate arbitrary data with each file.
 */
struct lights_io_attribute {
    struct module           *owner;
    struct attribute        attr;
    enum lights_state_type  type;
    struct lights_thunk     *thunk;

    lights_read_t           read;
    lights_write_t          write;
};

/**
 * struct lights_dev - Describes the device files to be created
 *
 * @name:         max LIGHTS_MAX_FILENAME_LENGTH, of directory name within
 *                the /dev/lights/ directory
 * @led_count:    The number of leds supported by the device
 * @caps:         A list of modes supported by the device
 * @attrs:        A null terminated array of io attributes
 *
 * The modes listed here are available to userland in the 'caps' file. This
 * file is created for each device when modes are given. Each mode is also
 * added to a global list, available at /sys/class/lights/all/caps. The modes listed
 * in this file are those which are defined by THIS module AND which are common
 * to each extension.
 */
struct lights_dev {
    const char                                  *name;
    uint16_t                                    led_count;
    struct lights_mode const                    *caps;
    struct lights_io_attribute const * const    *attrs;
};

#define VERIFY_LIGHTS_TYPE(_type) ( \
    BUILD_BUG_ON_ZERO(((_type) & ((_type) - 1)) != 0) + \
    BUILD_BUG_ON_ZERO((_type) > LIGHTS_TYPE_UPDATE) + \
    (_type) \
)

#define LIGHTS_ATTR(_name, _mode, _type, _thunk, _read, _write)     \
(struct lights_io_attribute) {                                      \
    .owner = THIS_MODULE,                                           \
    .attr = {.name = _name,                                         \
             .mode = VERIFY_OCTAL_PERMISSIONS(_mode) },             \
    .type          = VERIFY_LIGHTS_TYPE(_type),                     \
    .thunk         = _thunk,                                        \
    .read          = _read,                                         \
    .write         = _write,                                        \
}

/**
 * Helper macros for creating lights_io_attribute instances.
 */
#define LIGHTS_ATTR_RO(_name, _type, _thunk, _read) ( \
    LIGHTS_ATTR(_name, 0444, _type, _thunk, _read, NULL) \
)

#define LIGHTS_ATTR_WO(_name, _type, _thunk, _write) ( \
    LIGHTS_ATTR(_name, 0200, _type, _thunk, NULL, _write) \
)

#define LIGHTS_ATTR_RW(_name, _type, _thunk, _read, _write) ( \
    LIGHTS_ATTR(_name, 0644, _type, _thunk, _read, _write) \
)

#define LIGHTS_CUSTOM_ATTR(_name, _thunk, _read, _write) ( \
    LIGHTS_ATTR_RW(_name, LIGHTS_TYPE_CUSTOM, _thunk, _read, _write) \
)

#define LIGHTS_MODE_ATTR(_thunk, _read, _write) ( \
    LIGHTS_ATTR_RW(LIGHTS_IO_MODE, LIGHTS_TYPE_MODE, _thunk, _read, _write) \
)

#define LIGHTS_COLOR_ATTR(_thunk, _read, _write) ( \
    LIGHTS_ATTR_RW(LIGHTS_IO_COLOR, LIGHTS_TYPE_COLOR, _thunk, _read, _write) \
)

#define LIGHTS_SPEED_ATTR(_thunk, _read, _write) ( \
    LIGHTS_ATTR_RW(LIGHTS_IO_SPEED, LIGHTS_TYPE_SPEED, _thunk, _read, _write) \
)

#define LIGHTS_DIRECTION_ATTR(_thunk, _read, _write) ( \
    LIGHTS_ATTR_RW(LIGHTS_IO_DIRECTION, LIGHTS_TYPE_DIRECTION, _thunk, _read, _write) \
)

#define LIGHTS_LEDS_ATTR(_thunk, _write) ( \
    LIGHTS_ATTR_WO(LIGHTS_IO_LEDS, LIGHTS_TYPE_LEDS, _thunk, _write) \
)

#define LIGHTS_SYNC_ATTR(_thunk, _write) ( \
    LIGHTS_ATTR_WO(LIGHTS_IO_SYNC, LIGHTS_TYPE_SYNC, _thunk, _write) \
)

#define LIGHTS_UPDATE_ATTR(_thunk, _write) ( \
    LIGHTS_ATTR_WO(LIGHTS_IO_UPDATE, LIGHTS_TYPE_UPDATE, _thunk, _write) \
)

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
    struct lights_dev *dev
);

/**
 * lights_device_unregister() - Removes a device
 *
 * @dev: A device previously registered with lights_device_register()
 */
void lights_device_unregister (
    struct lights_dev *dev
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
    struct lights_dev const *dev,
    struct lights_io_attribute const *attr
);

/**
 * lights_create_file() - Adds a file to the devices directory
 *
 * @dev:   A previously registered device
 * @attrs: Array of descriptions of the files to create
 * @count: Number of elements in @attrs
 *
 * @Return: A negative error number on failure
 *
 * The given attribute may exist on the stack. Internally it is copied
 * so the user need not keep a reference to it.
 */
error_t lights_create_files (
    struct lights_dev const *dev,
    struct lights_io_attribute const * const attrs,
    size_t count
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
    struct lights_mode const *haystack,
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

#endif

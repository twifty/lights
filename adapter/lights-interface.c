// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>

#include <adapter/debug.h>
#include <include/quirks.h>

#include "lights-interface.h"

#define LIGHTS_FIRST_MINOR          0
#define LIGHTS_MAX_MINORS           512
#define LIGHTS_MAX_DEVICES          512

static struct {
    struct class        *class;
    struct lights_state state;
    struct lights_dev   all;

    struct {
        struct list_head    list;
        spinlock_t          lock;
        size_t              count;
    }                   interface,
                        caps;
    spinlock_t          state_lock;
    atomic_t            next_id;
    int                 major;
    spinlock_t          minor_lock;
    unsigned long       minor_map[BITS_TO_LONGS(LIGHTS_MAX_MINORS)];
} lights_global = {
    .interface = {
        .list = LIST_HEAD_INIT(lights_global.interface.list),
        .lock = __SPIN_LOCK_UNLOCKED(lights_global.interface.lock),
        .count = 0,
    },
    .caps = {
        .list = LIST_HEAD_INIT(lights_global.caps.list),
        .lock = __SPIN_LOCK_UNLOCKED(lights_global.caps.lock),
        .count = 0,
    },
    .state_lock = __SPIN_LOCK_UNLOCKED(lights_global.state_lock),
    .minor_lock = __SPIN_LOCK_UNLOCKED(lights_global.minor_lock),
    .next_id = ATOMIC_INIT(0),
    .all = { .name = "all" },
};

static struct lights_effect lights_available_effects[] = {
    LIGHTS_EFFECT_NAMED(OFF),
    LIGHTS_EFFECT_NAMED(STATIC),
    LIGHTS_EFFECT_NAMED(BREATHING),
    LIGHTS_EFFECT_NAMED(FLASHING),
    LIGHTS_EFFECT_NAMED(CYCLE),
    LIGHTS_EFFECT_NAMED(RAINBOW),
    {},
};

/**
 * struct lights_caps - Tracker for accumulated effects
 *
 * @siblings:  Next and prev pointers
 * @effect:      Copy of effect
 * @ref_count: Number of interfaces using this effect
 */
struct lights_caps {
    struct list_head        siblings;
    struct lights_effect    effect;
    uint32_t                ref_count;
};

/**
 * struct lights_file - Character device wrapper
 *
 * @minor:    Minor number of device
 * @cdev:     Character device
 * @dev:      Device instance
 * @siblings: Next and prev pointers
 * @attr:     Attributes the device was created with
 * @intf:     Owning interface
 * @fops:     File operations of the device
 */
struct lights_file {
    unsigned long                       minor;
    struct cdev                         cdev;
    struct device                       *dev;
    struct list_head                    siblings;
    struct lights_attribute     attr;
    struct lights_interface             *intf;
    struct file_operations              fops;
};
#define file_from_attr(ptr) ( \
    container_of(ptr, struct lights_file, attr) \
)

/**
 * struct lights_interface - Interface storage
 *
 * @name:      Name of directory/interface
 * @file_lock: Lock for file list (TODO remove, not required)
 * @file_list: Linked list of character devices
 * @siblings:  Next and prev pointers
 * @ldev:      Public handle
 * @kdev:      Kernel device
 */
struct lights_interface {
    struct list_head        siblings;
    struct lights_dev       *ldev;
    struct device           kdev;
    struct kref             refs;
    struct lights_color     *led_buffer;
    struct lights_file      update;
    struct lights_thunk     thunk;
    struct list_head        file_list;
    spinlock_t              file_lock;
    uint16_t                id;
    char                    name[LIGHTS_MAX_FILENAME_LENGTH];
};
#define INTERFACE_MAGIC 'INTF'
#define interface_from_thunk(ptr) ( \
    lights_thunk_container(ptr, struct lights_interface, thunk, INTERFACE_MAGIC) \
)
#define interface_from_dev(dev)( \
    container_of(dev, struct lights_interface, kdev) \
)

/*
 * Forward declaration for reference counters
 */
static void lights_interface_destroy (struct lights_interface *intf);
static void lights_interface_put (
    struct kref *ref
){
    struct lights_interface *intf = container_of(ref, struct lights_interface, refs);

    lights_interface_destroy(intf);
}

/**
 * lights_add_caps() - Adds a effect to accumulated list
 *
 * @effect: Mode to add
 *
 * @return: Zero or a negative error number
 *
 * The given @effect is copied and if it already exists it
 * reference count is increased.
 *
 * We keep this list in order to determine which effects are shared
 * by all interfaces, output in /sys/class/lights/all/caps
 *
 * This function also ensures that all name-id pairs are unique.
 */
static error_t lights_add_caps (
    struct lights_effect const *effect
){
    struct lights_caps *iter, *entry;
    error_t err = 0;

    if (IS_NULL(effect, effect->name))
        return -EINVAL;


    if (lights_effect_is_custom(effect))
        return 0;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;

    entry->effect = *effect;
    entry->ref_count = 2;

    spin_lock(&lights_global.caps.lock);

    list_for_each_entry(iter, &lights_global.caps.list, siblings) {
        if (iter->effect.id == effect->id) {
            if (0 == strcmp(iter->effect.name, effect->name)) {
                iter->ref_count++;
            } else {
                LIGHTS_ERR(
                    "effect %d:%s conflicts with known effect %d:%s",
                    effect->id, effect->name,
                    iter->effect.id, iter->effect.name
                );
                err = -EINVAL;
            }

            kfree(entry);
            goto exit;
        }
    }

    lights_global.caps.count++;
    list_add_tail(&entry->siblings, &lights_global.caps.list);

exit:
    spin_unlock(&lights_global.caps.lock);

    return 0;
}

/**
 * lights_del_caps() - Removes a effect from the accumulated list
 *
 * @effect: Mode to remove
 *
 * If more than one interface is using the same effect its
 * reference count is decreased, otherwise the effect is
 * removed entirely.
 */
static void lights_del_caps (
    struct lights_effect const *effect
){
    struct lights_caps *iter, *safe;

    if (IS_NULL(effect))
        return;

    spin_lock(&lights_global.caps.lock);

    list_for_each_entry_safe(iter, safe, &lights_global.caps.list, siblings) {
        if (iter->effect.id == effect->id) {
            iter->ref_count--;
            if (1 == iter->ref_count) {
                list_del(&iter->siblings);
                kfree(iter);
                lights_global.caps.count--;
                goto exit;
            }
        }
    }

exit:
    spin_unlock(&lights_global.caps.lock);
}

/**
 * lights_find_caps() - Searches for a named effect
 *
 * @name: Name of effect to find
 *
 * @return: Found effect or a negative error number
 */
static struct lights_effect const *lights_find_caps (
    const char *name
){
    struct lights_effect const *effect;
    struct lights_caps *iter;

    if (IS_NULL(name))
        return ERR_PTR(-EINVAL);

    spin_lock(&lights_global.caps.lock);

    list_for_each_entry(iter, &lights_global.caps.list, siblings) {
        if (0 == strcmp(iter->effect.name, name)) {
            effect = &iter->effect;
            goto exit;
        }
    }

    effect = ERR_PTR(-ENOENT);

exit:
    spin_unlock(&lights_global.caps.lock);

    return effect;
}

/**
 * lights_find_effect() - Finds a effect from a userland buffer
 *
 * @intf: Interface to search within
 * @effect: Target buffer to write
 * @buf:  Userland input buffer
 * @len:  Length of @buf
 *
 * @return: Error code
 */
static error_t lights_find_effect (
    struct lights_interface *intf,
    struct lights_effect *effect,
    const char __user *buf,
    size_t len
){
    struct lights_effect const *iter;
    char kern_buf[LIGHTS_EFFECT_MAX_NAME_LENGTH + 1];
    char *name;
    size_t count;

    if (!len || len > LIGHTS_EFFECT_MAX_NAME_LENGTH)
        return -EINVAL;

    count = min_t(size_t, len, LIGHTS_EFFECT_MAX_NAME_LENGTH);
    copy_from_user(kern_buf, buf, count);
    kern_buf[count] = 0;
    name = strim(kern_buf);

    if (0 == strcmp("all", intf->name)) {
        iter = lights_find_caps(name);
        if (IS_ERR(iter))
            return CLEAR_ERR(iter);

        memcpy(effect, iter, sizeof(*effect));
        return 0;
    }

    iter = lights_effect_find_by_name(intf->ldev->caps, name);
    if (iter) {
        memcpy(effect, iter, sizeof(*effect));
        return 0;
    }

    // iter = intf->ldev->caps;
    // if (iter) {
    //     while (iter->id != LIGHTS_EFFECT_ID_INVALID) {
    //         if (0 == strcmp(iter->name, name)) {
    //             memcpy(effect, iter, sizeof(*effect));
    //             return 0;
    //         }
    //         iter++;
    //     }
    // }

    LIGHTS_ERR("Mode '%s' not found in '%s'", name, intf->name);

    return -ENOENT;
}

/**
 * lights_dump_caps() - Writes a list of accumulated effects
 *
 * @buffer: Buffer to write to (PAGE_SIZE length)
 *
 * @return: Number of bytes written or a negative error code
 *
 * Only the effects sgared by ALL interfaces are written.
 */
static ssize_t lights_dump_caps (
    char *buffer
){
    struct lights_caps *iter;
    size_t effect_len;
    ssize_t written = 0;

    spin_lock(&lights_global.caps.lock);

    list_for_each_entry(iter, &lights_global.caps.list, siblings) {
        if (iter->ref_count != lights_global.interface.count)
            continue;

        effect_len = strlen(iter->effect.name);

        if (written + effect_len + 1 > PAGE_SIZE) {
            written = -ENOMEM;
            goto exit;
        }

        memcpy(buffer, iter->effect.name, effect_len);
        buffer[effect_len] = '\n';

        effect_len++;
        buffer += effect_len;
        written += effect_len;
    }

exit:
    spin_unlock(&lights_global.caps.lock);

    return written;
}

/**
 * lights_dump_effects() - Writes a list of effects
 *
 * @effects:  Zero terminated array of effects to write
 * @buffer: Buffer to write into (PAGE_SIZE length)
 *
 * @return: Number of bytes written or a negative error code
 */
static ssize_t lights_dump_effects (
    struct lights_effect const *effects,
    char *buffer
){
    struct lights_effect const *iter = effects;
    size_t effect_len;
    ssize_t written = 0;

    while (iter->id != LIGHTS_EFFECT_ID_INVALID) {
        if (!iter->name || 0 == iter->name[0])
            return -EIO;

        effect_len = strlen(iter->name);

        if (written + effect_len + 1 > PAGE_SIZE) {
            written = -ENOMEM;
            break;
        }

        memcpy(buffer, iter->name, effect_len);
        buffer[effect_len] = '\n';

        effect_len++;
        buffer += effect_len;
        written += effect_len;

        iter++;
    }

    return written;
}

/**
 * lights_append_caps() - Adds array of effects to accumulated list
 *
 * @effects: Zero terminated array of effects
 *
 * @return: Zero or a negative error number
 */
static error_t lights_append_caps (
    struct lights_effect const *effects
){
    struct lights_effect const *iter = effects, *rem;
    error_t err;

    if (IS_NULL(effects))
        return -EINVAL;

    while (iter->id != LIGHTS_EFFECT_ID_INVALID) {
        err = lights_add_caps(iter);
        if (err) {
            rem = effects;
            while (rem != iter) {
                lights_del_caps(rem);
                rem++;
            }
            return err;
        }
        iter++;
    }

    return 0;
}

/**
 * lights_remove_caps() - Removes array of effects from accumulated list
 *
 * @effects: Zero terminated array of effects
 */
static void lights_remove_caps (
    struct lights_effect const *effects
){
    while (effects->id != LIGHTS_EFFECT_ID_INVALID) {
        lights_del_caps(effects);
        effects++;
    }
}


/**
 * lights_read_hex() - Converts a hex string into a hex value
 *
 * @value:  Target buffer
 * @buffer: Input buffer
 * @length: Length of @buffer
 *
 * @return: Error code
 */
static error_t lights_read_hex (
    uint32_t *value,
    const char *buffer,
    size_t length
){
    char c;
    int shift, n, i;

    switch (length) {
        case 2: shift = 4; break;
        case 4: shift = 12; break;
        case 6: shift = 20; break;
        case 8: shift = 28; break;
        default:
            return -EINVAL;
    }

    *value = 0;

    for (i = 0; i < length; i++, shift -= 4) {
        c = buffer[i];

        if (c >= '0' && c <= '9')
            n = c - '0';
        else if (c >= 'a' && c <= 'f')
            n = (c - 'a') + 10;
        else if (c >= 'A' && c <= 'F')
            n = (c - 'A') + 10;
        else
            return -EINVAL;

        *value |= n << shift;
    }

    return 0;
}

/**
 * lights_read_color - Helper for reading color value strings
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
){
    char kern_buf[9];
    const char *p;
    size_t count;
    uint8_t n;
    u32 value;
    error_t i;

    if (IS_NULL(buffer, color) || IS_TRUE(len <= 0))
        return -EINVAL;

    count = len < 8 ? len : 8;
    if (is_user_memory(buffer, count)) {
        copy_from_user(kern_buf, buffer, count);
        buffer = kern_buf;
    }

    // If the string beings with '#' or '0x' expect 6 hex values
    p = buffer;
    value = 0;

    if (count >= 7) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        else if (p[0] == '#')
            p += 1;

        if (p != buffer) {
            for (i = 20; i >= 0 && *p && p - buffer < count; i -= 4) {
                if (*p >= '0' && *p <= '9')
                    n = *p - '0';
                else if (*p >= 'a' && *p <= 'f')
                    n = (*p - 'a') + 10;
                else if (*p >= 'A' && *p <= 'F')
                    n = (*p - 'A') + 10;
                else
                    return -EINVAL;

                value |= n << i;
                p++;
            }

            color->r = (value >> 16) & 0xFF;
            color->g = (value >> 8) & 0xFF;
            color->b = value & 0xFF;

            return p - buffer;
        }
    }

    return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(lights_read_color, LIGHTS);

/**
 * lights_read_effect - Helper for reading effect value strings
 *
 * @buffer:   A kernel/user buffer containing the string
 * @len:      The length of the buffer
 * @haystack: A list of valid effects to match against
 * @effect:     A effect object to populate
 *
 * @Return: The number of characters read or a negative error number
 */
ssize_t lights_read_effect (
    const char *buffer,
    size_t len,
    struct lights_effect const *haystack,
    struct lights_effect *effect
){
    struct lights_effect const *found;
    char kern_buf[LIGHTS_EFFECT_MAX_NAME_LENGTH];
    size_t count;

    if (IS_NULL(buffer, effect) || IS_TRUE(len <= 0))
        return -EINVAL;

    if (!haystack)
        haystack = lights_available_effects;

    count = len < LIGHTS_EFFECT_MAX_NAME_LENGTH ? len : LIGHTS_EFFECT_MAX_NAME_LENGTH;
    if (is_user_memory(buffer, count)) {
        copy_from_user(kern_buf, buffer, count);
        buffer = kern_buf;
    }

    found = lights_effect_find_by_name(haystack, buffer);
    if (found) {
        *effect = *found;
        return 0;
    }
    // for (p = haystack; p->id != LIGHTS_EFFECT_ID_INVALID && p->name; p++) {
    //     if (strcmp(buffer, p->name) == 0) {
    //         *effect = *p;
    //         return 0;
    //     }
    // }

    return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(lights_read_effect, LIGHTS);

/**
 * lights_read_speed - Helper for reading speed value strings
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
){
    char tmp;

    if (IS_NULL(buffer, speed) || IS_TRUE(len <= 0))
        return -EINVAL;

    if (len < 1)
        return -EINVAL;

    if (is_user_memory(buffer, 1)) {
        if (get_user(tmp, buffer))
            return -EFAULT;
    } else {
        tmp = buffer[0];
    }

    if (tmp < '0' || tmp > '5')
        return -EINVAL;

    *speed = tmp - '0';

    return 1;
}
EXPORT_SYMBOL_NS_GPL(lights_read_speed, LIGHTS);

/**
 * lights_read_direction - Helper for reading direction value strings
 *
 * @buffer:    A kernel/user buffer containing the string
 * @len:       The length of the buffer
 * @direction: A value to populate with the direction
 *
 * @Return: The number of characters read or a negative error number
 */
ssize_t lights_read_direction (
    const char *buffer,
    size_t len,
    uint8_t *direction
){
    char tmp;

    if (IS_NULL(buffer, direction) || IS_TRUE(len <= 0))
        return -EINVAL;

    if (len < 1)
        return -EINVAL;

    if (is_user_memory(buffer, 1)) {
        if (get_user(tmp, buffer))
            return -EFAULT;
    } else {
        tmp = buffer[0];
    }

    if (tmp == '0')
        *direction = 0;
    else if (tmp == '1')
        *direction = 1;
    else
        return -EINVAL;

    return 1;
}
EXPORT_SYMBOL_NS_GPL(lights_read_direction, LIGHTS);

/**
 * lights_read_sync - Helper for reading sync value strings
 *
 * @buffer: A kernel/user buffer containing the string
 * @len:    The length of the buffer
 * @sync:   A value to populate with the speed
 *
 * @Return: The number of characters read or a negative error number
 */
ssize_t lights_read_sync (
    const char *buffer,
    size_t len,
    uint8_t *sync
){
    char kern_buf[4];
    uint32_t value;

    if (IS_NULL(buffer, sync) || IS_TRUE(len <= 0))
        return -EINVAL;

    if (len < 4)
        return -EINVAL;

    if (is_user_memory(buffer, ARRAY_SIZE(kern_buf))) {
        copy_from_user(kern_buf, buffer, ARRAY_SIZE(kern_buf));
        buffer = kern_buf;
    }

    if (buffer[0] == '0' && (buffer[1] == 'x' || buffer[1] == 'X')) {
        if (0 == lights_read_hex(&value, &buffer[2], 2)) {
            *sync = (uint8_t)value;
            return 4;
        }
    }

    return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(lights_read_sync, LIGHTS);

/**
 * lights_get_params - Retrieves the current global state
 *
 * @state: An object to populate with the state
 */
void lights_get_state (
    struct lights_state *state
){
    if (IS_NULL(state))
        return;

    spin_lock(&lights_global.state_lock);

    memcpy(state, &lights_global.state, sizeof(*state));
    state->type = LIGHTS_TYPE_EFFECT | LIGHTS_TYPE_COLOR | LIGHTS_TYPE_SPEED | LIGHTS_TYPE_DIRECTION;

    spin_unlock(&lights_global.state_lock);
}
EXPORT_SYMBOL_NS_GPL(lights_get_state, LIGHTS);

/**
 * lights_get_effects() - Returns a zero terminated array of global effects
 *
 * @return: Effect array
 */
struct lights_effect const *lights_get_effects (
    void
){
    return lights_available_effects;
}
EXPORT_SYMBOL_NS_GPL(lights_get_effects, LIGHTS);

/**
 * find_attribute_for_file() - Finds the user attributes for the given character device
 *
 * @filp: Character device handle (/dev/lights/___/)
 *
 * @return: NULL or the file containing the attributes
 *
 * NOTE, The reference count is increased on the owning interface. When the
 * caller is done with the object it MUST decrease the reference counter.
 */
static struct lights_file *find_attribute_for_file (
    struct file *filp
){
    struct lights_interface *interface;
    struct lights_file *iter;
    struct cdev *cdev;

    cdev = filp->f_inode->i_cdev;

    spin_lock(&lights_global.interface.lock);

    list_for_each_entry(interface, &lights_global.interface.list, siblings) {
        if (!list_empty(&interface->file_list)) {
            list_for_each_entry(iter, &interface->file_list, siblings) {
                if (cdev == &iter->cdev) {
                    kref_get(&iter->intf->refs);
                    goto found;
                }
            }
        }
    }

    iter = NULL;

found:
    spin_unlock(&lights_global.interface.lock);

    return iter;
}

/**
 * find_attribute_for_type() - Searches for a given attribute type in the interface
 *
 * @interface: Interface to search
 * @type:      Type of attribute to find
 *
 * @return: NULL or the file containing the attributes
 *
 * NOTE, The reference count is increased on the owning interface. When the
 * caller is done with the object it MUST decrease the reference counter.
 */
static struct lights_file *find_attribute_for_type (
    struct lights_interface *intf,
    enum lights_state_type type
){
    struct lights_file *iter;

    list_for_each_entry(iter, &intf->file_list, siblings) {
        if (type == iter->attr.type) {
            kref_get(&intf->refs);
            return iter;
        }
    }

    return NULL;
}

/**
 * update_each_interface() - Invokes the write method in all relevant attributes
 *
 * @state: Buffer of data to write
 *
 * @return: Error code
 */
static error_t update_each_interface (
    struct lights_state const *state
){
    struct lights_file const **files = NULL;
    struct lights_interface *intf;
    size_t count, i;
    error_t err = 0;

    /*
     * We cannot hold a spinlock while invoking the read callback or while
     * allocating memory. Reading the count in an unlocked state is risky
     * since an interface may be removed right after reading.
     */

repeat:
    count = lights_global.interface.count;
    kfree(files);
    files = kcalloc(count, sizeof(*files), GFP_KERNEL);
    if (!files)
        return -ENOMEM;

    spin_lock(&lights_global.interface.lock);

    if (count < lights_global.interface.count) {
        spin_unlock(&lights_global.interface.lock);
        goto repeat;
    }

    count = 0;
    list_for_each_entry(intf, &lights_global.interface.list, siblings) {
        /* Exclude the "all" interface */
        if (intf->id == 0)
            continue;

        files[count] = find_attribute_for_type(intf, state->type);
        if (files[count])
            count++;
    }

    spin_unlock(&lights_global.interface.lock);

    for (i = 0; i < count; i++) {
        if (files[i]->attr.write) {
            err = files[i]->attr.write(files[i]->attr.thunk, state);

            if (err) {
                LIGHTS_ERR(
                    "Failed to update '%s/%s': %s",
                    files[i]->intf->name,
                    files[i]->attr.attr.name,
                    ERR_NAME(err)
                );
            }
        }

        kref_put(&files[i]->intf->refs, lights_interface_put);
    }

    kfree(files);

    return 0;
}

/**
 * io_read() - File output handler
 *
 * @thunk: Unused
 * @state: Buffer to populate
 *
 * @return: Zero
 *
 * This function is the read handler for all device files
 * under the "all" interface.
 */
static error_t io_read (
    struct lights_thunk *thunk,
    struct lights_state *state
){
    lights_get_state(state);

    return 0;
}

/**
 * io_write() - File input handler
 *
 * @thunk: Unused
 * @state: Data written to the file
 *
 * @return: Error code
 *
 * This function is the write handler for all device files
 * under the "all" interface. When invoked it will call the
 * write method of all other interfaces for the same data type.
 */
static error_t io_write (
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    if (IS_NULL(state))
        return -EINVAL;

    spin_lock(&lights_global.state_lock);

    if (state->type & LIGHTS_TYPE_EFFECT)
        lights_global.state.effect = state->effect;
    if (state->type & LIGHTS_TYPE_COLOR)
        lights_global.state.color = state->color;
    if (state->type & LIGHTS_TYPE_SPEED)
        lights_global.state.speed = state->speed;
    if (state->type & LIGHTS_TYPE_DIRECTION)
        lights_global.state.direction = state->direction;
    if (state->type & LIGHTS_TYPE_SYNC)
        lights_global.state.sync = state->sync;

    spin_unlock(&lights_global.state_lock);

    return update_each_interface(state);
}

/**
 * caps_show() - File IO handler for /sys/class/lights/___/caps
 *
 * @dev:  Device being read
 * @attr: Unused
 * @buf:  Buffer to write into (PAGE_SIZE length)
 *
 * @return: Bytes written or a negative error code
 */
static ssize_t caps_show (
    struct device *dev,
    struct device_attribute *attr,
    char *buf
){
    struct lights_interface *intf = interface_from_dev(dev);
    ssize_t written = 0;

    if (0 == strcmp(intf->name, "all")) {
        written = lights_dump_caps(buf);
    } else if (intf->ldev->caps) {
        written = lights_dump_effects(intf->ldev->caps, buf);
    }

    return written;
}
DEVICE_ATTR_RO(caps);

/**
 * led_count_show() - File IO handler for /sys/class/lights/___/led_count
 *
 * @dev:  Device being read
 * @attr: Unused
 * @buf:  Buffer to write into (PAGE_SIZE length)
 *
 * @return: Bytes written or a negative error code
 */
static ssize_t led_count_show (
    struct device *dev,
    struct device_attribute *attr,
    char *buf
){
    struct lights_interface *intf = interface_from_dev(dev);

    if (intf->ldev)
        return sprintf(buf, "%d", intf->ldev->led_count);

    /* The 'all' interface has no associated dev, so 0 leds */
    return 0;
}
DEVICE_ATTR_RO(led_count);

static struct attribute *lights_class_attrs[] = {
	&dev_attr_caps.attr,
    &dev_attr_led_count.attr,
	NULL,
};

static struct attribute_group const lights_class_group = {
	.attrs = lights_class_attrs,
};

static struct attribute_group const *lights_class_groups[] = {
	&lights_class_group,
	NULL,
};

/**
 * lights_attribute_read() - Helper method for invoking write
 *
 * @filp:  Handle to the cdev file
 * @state: Data to write
 *
 * @return: Error code
 */
static error_t lights_attribute_read (
    struct file *filp,
    struct lights_state *state
){
    struct lights_file *file;
    error_t err = -ENODEV;

    file = find_attribute_for_file(filp);
    if (!file)
        return -ENODEV;

    if (file->attr.read)
        err = file->attr.read(file->attr.thunk, state);

    kref_put(&file->intf->refs, lights_interface_put);

    return err;
}

/**
 * lights_attribute_write() - Helper method for invoking read
 *
 * @filp:  Handle to the cdev file
 * @state: Buffer to fill
 *
 * @return: Error code
 */
static error_t lights_attribute_write (
    struct file *filp,
    struct lights_state const *state
){
    struct lights_file *file;
    error_t err = -ENODEV;

    file = find_attribute_for_file(filp);
    if (!file)
        return -ENODEV;

    if (file->attr.write)
        err = file->attr.write(file->attr.thunk, state);

    kref_put(&file->intf->refs, lights_interface_put);

    return err;
}

/**
 * lights_effect_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Target buffer
 * @len:  Length of @buf
 * @off:  Offset to begin reading
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_effect_attribute_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_EFFECT
    };
    struct lights_effect *effect = &state.effect;
    char effect_buf[LIGHTS_EFFECT_MAX_NAME_LENGTH];
    ssize_t count, err;

    err = lights_attribute_read(filp, &state);
    if (err)
        return err;

    if (IS_NULL(effect->name))
        return -EIO;

    count = strlen(effect->name) + 2;
    if (*off >= count)
        return 0;

    snprintf(effect_buf, count, "%s\n", effect->name);
    err = copy_to_user(buf, effect_buf, len < count ? len : count);
    if (err)
        return -EFAULT;

    return *off = count;
}

/**
 * lights_effect_attribute_write() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_effect_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_file const *file;
    struct lights_state state = {
        .type = LIGHTS_TYPE_EFFECT
    };
    error_t err;

    file = find_attribute_for_file(filp);
    if (!file)
        return -ENODEV;

    err = lights_find_effect(file->intf, &state.effect, buf, len);
    if (err)
        goto exit;

    if (file->attr.write)
        err = file->attr.write(file->attr.thunk, &state);
    else
        err = -ENODEV;

exit:
    kref_put(&file->intf->refs, lights_interface_put);

    return err ? err : len;
}

/**
 * lights_color_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Target buffer
 * @len:  Length of @buf
 * @off:  Offset to begin reading
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_color_attribute_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_COLOR
    };
    struct lights_color *c = &state.color;
    char color_buf[9];
    ssize_t err;

    if (*off >= 9)
        return 0;

    err = lights_attribute_read(filp, &state);
    if (err)
        return err;

    snprintf(color_buf, 9, "#%02X%02X%02X\n", c->r, c->g, c->b);
    err = copy_to_user(buf, color_buf, 9);
    if (err)
        return -EFAULT;

    return *off = 9;
}

/**
 * lights_color_attribute_write() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_color_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_COLOR
    };
    ssize_t err;

    err = lights_read_color(buf, len, &state.color);
    if (err < 0)
        return err;

    err = lights_attribute_write(filp, &state);
    if (err)
        return err;

    return len;
}

/**
 * lights_speed_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Target buffer
 * @len:  Length of @buf
 * @off:  Offset to begin reading
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_speed_attribute_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_SPEED
    };
    char speed_buf[3];
    ssize_t err;

    if (*off >= 2)
        return 0;

    err = lights_attribute_read(filp, &state);
    if (err)
        return err;

    speed_buf[0] = state.speed + '0';
    speed_buf[1] = '\n';
    speed_buf[2] = 0;

    err = copy_to_user(buf, speed_buf, len < 3 ? len : 3);
    if (err)
        return -EFAULT;

    return *off = 2;
}

/**
 * lights_speed_attribute_write() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_speed_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_SPEED
    };
    ssize_t err;

    err = lights_read_speed(buf, len, &state.speed);
    if (err < 0)
        return err;

    err = lights_attribute_write(filp, &state);
    if (err)
        return err;

    return len;
}

/**
 * lights_direction_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Target buffer
 * @len:  Length of @buf
 * @off:  Offset to begin reading
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_direction_attribute_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_DIRECTION
    };
    char output[3];
    ssize_t err;

    if (*off >= 2)
        return 0;

    err = lights_attribute_read(filp, &state);
    if (err)
        return err;

    output[0] = state.direction + '0';
    output[1] = '\n';
    output[2] = 0;

    err = copy_to_user(buf, output, len < 3 ? len : 3);
    if (err)
        return -EFAULT;

    return *off = 2;
}

/**
 * lights_direction_attribute_write() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_direction_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_DIRECTION
    };
    ssize_t err;

    err = lights_read_direction(buf, len, &state.speed);
    if (err < 0)
        return err;

    err = lights_attribute_write(filp, &state);
    if (err)
        return err;

    return len;
}

/**
 * lights_color_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Target buffer
 * @len:  Length of @buf
 * @off:  Offset to begin reading
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_raw_attribute_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_CUSTOM
    };
    struct lights_buffer *buffer = &state.raw;
    ssize_t err;

    buffer->offset = *off;
    buffer->length = len;
    buffer->data = kmalloc(len, GFP_KERNEL);
    if (!buffer->data)
        return -ENOMEM;

    err = lights_attribute_read(filp, &state);
    if (err)
        goto error;

    // TODO - Keep reading from callback until length is 0 or > len
    copy_to_user(buf, buffer->data, buffer->length);
    *off = buffer->offset;

error:
    kfree(buffer->data);

    return err ? err : buffer->length;
}

/**
 * lights_color_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_raw_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_CUSTOM
    };
    struct lights_buffer *buffer = &state.raw;
    ssize_t err;

    buffer->offset = *off;
    buffer->length = len;
    buffer->data = kmalloc(len, GFP_KERNEL);
    if (!buffer->data) {
        err = -ENOMEM;
        goto exit;
    }

    err = copy_from_user(buffer->data, buf, len);
    if (err) {
        err = -EIO;
        goto exit;
    }

    err = lights_attribute_write(filp, &state);

exit:
    kfree(buffer->data);

    return err ? err : buffer->length;
}

/**
 * lights_color_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_leds_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_file const *file;
    struct lights_state state = {
        .type = LIGHTS_TYPE_LEDS
    };
    struct lights_buffer *buffer = &state.raw;
    struct lights_color *color;
    uint8_t kern_buf[3];
    uint16_t led_count;
    ssize_t err;
    int i;

    file = find_attribute_for_file(filp);
    if (!file)
        return -ENODEV;

    if (!file->attr.write) {
        err = -ENODEV;
        goto exit;
    }

    /* The buffer must account for every led */
    led_count = file->intf->ldev->led_count;
    if (!led_count || led_count * 3 != len) {
        err = -EINVAL;
        goto exit;
    }

    if (!file->intf->led_buffer) {
        file->intf->led_buffer = kcalloc(led_count, sizeof(struct lights_color), GFP_KERNEL);
        if (!file->intf->led_buffer) {
            err = -ENOMEM;
            goto exit;
        }
    }

    buffer->offset = *off;
    buffer->length = led_count;
    buffer->data   = file->intf->led_buffer;

    color = file->intf->led_buffer;

    for (i = 0; i < led_count; i++) {
        err = copy_from_user(kern_buf, buf, 3);
        if (err) {
            err = -EIO;
            goto exit;
        }

        lights_color_read_rgb(color, kern_buf);

        color++;
        buf += 3;
    }

    err = file->attr.write(file->attr.thunk, &state);

exit:
    kref_put(&file->intf->refs, lights_interface_put);

    return err ? err : buffer->length;
}

/**
 * lights_color_attribute_read() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 */
static ssize_t lights_sync_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_state state = {
        .type = LIGHTS_TYPE_SYNC
    };
    ssize_t err;

    err = lights_read_sync(buf, len, &state.sync);
    if (err < 0)
        return err;

    err = lights_attribute_write(filp, &state);
    if (err)
        return err;

    return len;
}

/**
 * lights_update_attribute_write() - File IO handler
 *
 * @filp: Character device handle
 * @buf:  Source buffer
 * @len:  Length of @buf
 * @off:  Offset to begin writing
 *
 * @return: Number of bytes or a negative error code
 *
 * This is a special function which allows another process to
 * pass in a struct lights_state. This should enable a service
 * to update multiple properties in a single call.
 */
static ssize_t lights_update_attribute_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_file const *file;
    enum lights_state_type allowed = (
        LIGHTS_TYPE_EFFECT | LIGHTS_TYPE_COLOR | LIGHTS_TYPE_SPEED | LIGHTS_TYPE_DIRECTION | LIGHTS_TYPE_SYNC
    );
    struct lights_state state;
    error_t err;

    if (len != sizeof(state)) {
        LIGHTS_ERR("Unexpected 'update' length: %ld", len);
        return -EINVAL;
    }

    if (0 != copy_from_user(&state, buf, len)) {
        LIGHTS_ERR("Failed to copy user buffer");
        return -EIO;
    }

    if ((state.type & ~allowed) != 0) {
        LIGHTS_ERR("state.type contains unsupported flags");
        return -EINVAL;
    }

    /* Trying to sneak in a pointer??? */
    memset(&state.raw, 0, sizeof(state.raw));

    file = find_attribute_for_file(filp);
    if (!file)
        return -ENODEV;

    /* Fix effect name */
    if (state.type & LIGHTS_TYPE_EFFECT) {
        if (!state.effect.name || state.effect.id > LIGHTS_EFFECT_MAX_NAME_LENGTH) {
            LIGHTS_ERR("userland buffer error");
            err = -EINVAL;
            goto exit;
        }

        err = lights_find_effect(file->intf, &state.effect, state.effect.name, state.effect.id);
        if (err)
            goto exit;
    }

    if (state.type & LIGHTS_TYPE_SPEED) {
        if (state.speed > 5) {
            LIGHTS_ERR("Invalid speed value: 0x%02x", state.speed);
            err = -EINVAL;
            goto exit;
        }
    }

    if (state.type & LIGHTS_TYPE_DIRECTION) {
        if (state.direction > 1) {
            LIGHTS_ERR("Invalid direction value: 0x%02x", state.direction);
            err = -EINVAL;
            goto exit;
        }
    }

    if (file->attr.write)
        err = file->attr.write(file->attr.thunk, &state);
    else
        err = -ENODEV;

exit:
    kref_put(&file->intf->refs, lights_interface_put);

    return -ENODEV;
}


static inline error_t lights_minor_get (
    unsigned long *minor
){
    error_t err = 0;

    spin_lock(&lights_global.minor_lock);

    *minor = find_first_zero_bit(lights_global.minor_map, LIGHTS_MAX_MINORS);
    if (*minor < LIGHTS_MAX_MINORS)
        set_bit(*minor, lights_global.minor_map);
    else
        err = -EBUSY;

    spin_unlock(&lights_global.minor_lock);

    return err;
}

static inline error_t lights_minor_put (
    unsigned long minor
){
    error_t err = 0;

    spin_lock(&lights_global.minor_lock);

    if (minor < LIGHTS_MAX_MINORS)
        clear_bit(minor, lights_global.minor_map);
    else
        err = -EINVAL;

    spin_unlock(&lights_global.minor_lock);

    return err;
}

/**
 * lights_device_release() - Dummy function
 *
 * @dev: Unused
 */
static void lights_device_release (
    struct device *dev
){
    /* Nothing to do here */
}

/**
 * file_operations_create() - Builds read/write functions for attributes
 *
 * @file: Target, contains the file_operations struct
 * @attr: Attributes for the file
 *
 * @return: Zero or a negative error number
 */
static error_t file_operations_create (
    struct lights_file *file,
    struct lights_attribute const *attr
){
    memset(&file->fops, 0, sizeof(file->fops));

    /*
        The fops structure contains local red/write methods. Each of these
        methods will retrieve the lights_file, associated with the cdev,
        which in turn contains the user read/write functions and any
        private data associated with it.
     */
    switch (attr->type) {
        case LIGHTS_TYPE_EFFECT:
            file->fops.read = lights_effect_attribute_read;
            if (attr->write)
                file->fops.write = lights_effect_attribute_write;
            break;
        case LIGHTS_TYPE_COLOR:
            file->fops.read = lights_color_attribute_read;
            if (attr->write)
                file->fops.write = lights_color_attribute_write;
            break;
        case LIGHTS_TYPE_SPEED:
            file->fops.read = lights_speed_attribute_read;
            if (attr->write)
                file->fops.write = lights_speed_attribute_write;
            break;
        case LIGHTS_TYPE_DIRECTION:
            file->fops.read = lights_direction_attribute_read;
            if (attr->write)
                file->fops.write = lights_direction_attribute_write;
            break;
        case LIGHTS_TYPE_CUSTOM:
            file->fops.read = lights_raw_attribute_read;
            if (attr->write)
                file->fops.write = lights_raw_attribute_write;
            break;
        case LIGHTS_TYPE_LEDS:
            if (!attr->write || attr->read) {
                LIGHTS_ERR("LIGHTS_TYPE_LEDS is write only");
                return -EINVAL;
            }
            file->fops.write = lights_leds_attribute_write;
            break;
        case LIGHTS_TYPE_UPDATE:
            if (!attr->write || attr->read) {
                LIGHTS_ERR("LIGHTS_TYPE_UPDATE is write only");
                return -EINVAL;
            }
            file->fops.write = lights_update_attribute_write;
            break;
        case LIGHTS_TYPE_SYNC:
            if (!attr->write || attr->read) {
                LIGHTS_ERR("LIGHTS_TYPE_SYNC is write only");
                return -EINVAL;
            }
            file->fops.write = lights_sync_attribute_write;
            break;
        default:
            return -EINVAL;
    }

    file->fops.owner = attr->owner;
    file->attr = *attr;
    // memcpy((void*)&file->attr, attr, sizeof(file->attr));

    return 0;
}

/**
 * lights_update_attribute_default() - Generic update handler
 *
 * @thunk: The interface
 * @state: New state
 *
 * @return: Error code
 *
 * The default implementation is for each file in the interface
 * to be iterated and tested if it accepts the state type. Any
 * error in the chain will abort the searching.
 *
 * Ideally, each interface would have one update handler as a catch
 * all for the possible types.
 */
static error_t lights_update_attribute_default(
    struct lights_thunk *thunk,
    struct lights_state const *state
){
    struct lights_interface *intf = interface_from_thunk(thunk);
    struct lights_file *file;
    enum lights_state_type accepted[] = {
        LIGHTS_TYPE_EFFECT,
        LIGHTS_TYPE_COLOR,
        LIGHTS_TYPE_SPEED,
        LIGHTS_TYPE_DIRECTION,
        LIGHTS_TYPE_SYNC,
    };
    int i;
    error_t err = 0;

    if (IS_NULL(thunk, state, intf))
        return -EINVAL;

    for (i = 0; i < ARRAY_SIZE(accepted) && !err; i++) {
        if (state->type & accepted[i]) {
            if ((file = find_attribute_for_type(intf, accepted[i]))) {
                if (file->attr.write)
                    err = file->attr.write(file->attr.thunk, state);

                kref_put(&file->intf->refs, lights_interface_put);
            }
        }
    }

    return err;
}

/**
 * lights_file_init() - Character device creation
 *
 * @file: Pre created file
 * @intf: Owning interface
 * @attr: Attributes of the file
 *
 * @return: Error code
 */
static error_t lights_file_init (
    struct lights_file *file,
    struct lights_interface *intf,
    struct lights_attribute const *attr
){
    dev_t ver;
    error_t err;

    if (IS_NULL(file, attr, intf, attr->attr.name) || IS_TRUE(attr->attr.name[0] == 0))
        return -EINVAL;

    err = lights_minor_get(&file->minor);
    if (err) {
        LIGHTS_ERR("Failed to allocate minor number");
        goto error_exit;
    }

    err = file_operations_create(file, attr);
    if (err) {
        LIGHTS_ERR("Failed to create file operations: %s", ERR_NAME(err));
        goto error_exit;
    }

    file->intf = intf;
    ver = MKDEV(lights_global.major, file->minor);

    /* Create a character device with a unique major:minor */
    cdev_init(&file->cdev, &file->fops);
    file->cdev.owner = attr->owner;
    err = cdev_add(&file->cdev, ver, 1);
    if (err) {
        LIGHTS_ERR("Failed to add character device: %s", ERR_NAME(err));
        goto error_exit;
    }

    /*
     * Create a device with the same major:minor,
     * the cdev is automatically associated with it.
     *
     * The name configured here will be converted to the correct
     * path within lights_devnode().
     */
    file->dev = device_create(
        lights_global.class,
        &intf->kdev,
        ver,
        NULL,
        "%s:%s", intf->name, attr->attr.name
    );

    if (IS_ERR(file->dev)) {
        err = CLEAR_ERR(file->dev);
        LIGHTS_ERR("Failed to create device '%s:%s': %s", intf->name, attr->attr.name, ERR_NAME(err));
        goto error_free_cdev;
    }

    LIGHTS_DBG("created device '/dev/lights/%s/%s'", intf->name, attr->attr.name);

    return 0;

error_free_cdev:
    cdev_del(&file->cdev);

error_exit:
    lights_minor_put(file->minor);

    return err;
}

/**
 * lights_file_create() - Character device creation
 *
 * @intf: Owning interface
 * @attr: Attributes of the file
 *
 * @return: Character device wrapper or negative error code
 *
 * sysfs will not allow a same named file to be created, so error codes
 * will be returned.
 */
static struct lights_file *lights_file_create (
    struct lights_interface *intf,
    struct lights_attribute const *attr
){
    struct lights_file *file;
    error_t err;

    if (IS_NULL(attr, intf, attr->attr.name) || IS_TRUE(attr->attr.name[0] == 0))
        return ERR_PTR(-EINVAL);

    /* Special handling of update file */
    if (0 == strcmp(attr->attr.name, LIGHTS_IO_UPDATE)) {
        if (intf->update.attr.write == lights_update_attribute_default) {
            /* Does it need validating? */
            intf->update.attr = *attr;
            return &intf->update;
        }
        return ERR_PTR(-EEXIST);
    }

    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (!file)
        return ERR_PTR(-ENOMEM);

    err = lights_file_init(file, intf, attr);
    if (!err)
        return file;

    kfree(file);

    return ERR_PTR(err);
}

/**
 * lights_file_destroy() - Destroys a character device
 *
 * @file: Wrapper to be destroyed
 */
static void lights_file_destroy (
    struct lights_file *file
){
    if (IS_NULL(file))
        return;

    device_destroy(lights_global.class, MKDEV(lights_global.major, file->minor));
    cdev_del(&file->cdev);
    lights_minor_put(file->minor);

    LIGHTS_DBG("removed device '/dev/lights/%s/%s'", file->intf->name, file->attr.attr.name);

    if (file == &file->intf->update)
        memset(file, 0, sizeof(*file));
    else
        kfree(file);
}

/**
 * lights_interface_find() - Finds the owner of the device
 *
 * @dev: user handle to the interface
 *
 * @return: NULL or a reference counted interface
 *
 * Only called when creating files and unregsitering device.
 */
static struct lights_interface *lights_interface_find (
    struct lights_dev const *dev
){
    struct lights_interface *interface;

    if (IS_NULL(dev))
        return NULL;

    spin_lock(&lights_global.interface.lock);

    list_for_each_entry(interface, &lights_global.interface.list, siblings) {
        if (interface->ldev == dev) {
            kref_get(&interface->refs);
            goto found;
        }
    }

    interface = NULL;

found:
    spin_unlock(&lights_global.interface.lock);

    return interface;
}

/**
 * lights_interface_destroy() - Destroys an interface
 *
 * @intf: Interface to destroy
 *
 * The interface is expected to have been removed from any list
 * and have no open references.
 */
static void lights_interface_destroy (
    struct lights_interface *intf
){
    struct lights_file *file, *safe;

    if (IS_NULL(intf))
        return;

    /* Possible when owner didn't create an update attr */
    if (intf->update.siblings.next == 0)
        list_add_tail(&intf->update.siblings, &intf->file_list);

    if (!list_empty(&intf->file_list)) {
        list_for_each_entry_safe(file, safe, &intf->file_list, siblings) {
            list_del(&file->siblings);
            lights_file_destroy(file);
        }
    }

    if (intf->ldev->caps)
        lights_remove_caps(intf->ldev->caps);

    device_unregister(&intf->kdev);

    LIGHTS_DBG("removed interface '%s'", intf->name);

    kfree(intf->led_buffer);
    kfree(intf);
}

/**
 * lights_interface_create() - Interface creator
 *
 * @name: Name of the interface (and directory within /dev/lights/)
 *
 * @return: New interface or a negative error code
 */
static struct lights_interface *lights_interface_create (
    struct lights_dev *lights
){
    struct lights_attribute const * const *attr;
    struct lights_interface *intf;
    struct lights_file *file;
    error_t err;

    if (IS_NULL(lights, lights->name) || IS_TRUE(lights->name[0] == 0))
        return ERR_PTR(-EINVAL);

    intf = kzalloc(sizeof(*intf), GFP_KERNEL);
    if (!intf)
        return ERR_PTR(-ENOMEM);

    intf->ldev = lights;
    intf->id = atomic_fetch_inc(&lights_global.next_id);

    lights_thunk_init(&intf->thunk, INTERFACE_MAGIC);
    spin_lock_init(&intf->file_lock);
    INIT_LIST_HEAD(&intf->file_list);
    kref_init(&intf->refs);
    strncpy(intf->name, lights->name, LIGHTS_MAX_FILENAME_LENGTH);

    dev_set_name(&intf->kdev, intf->name);
    intf->kdev.class = lights_global.class;
    intf->kdev.release = lights_device_release;
    intf->kdev.groups = lights_class_groups;

    device_register(&intf->kdev);

    /* Register the only default attribute */
    err = lights_file_init(
        &intf->update,
        intf,
        &LIGHTS_UPDATE_ATTR(&intf->thunk, lights_update_attribute_default)
    );
    if (err) {
        LIGHTS_ERR("Failed to initialize master update: %s", ERR_NAME(err));
        goto error;
    }

    if (lights->attrs) {
        for (attr = lights->attrs; *attr; attr++) {
            file = lights_file_create(intf, *attr);

            if (IS_ERR(file)) {
                err = PTR_ERR(file);
                LIGHTS_ERR("Failed to create file: %s", ERR_NAME(err));
                goto error;
            }

            list_add_tail(&file->siblings, &intf->file_list);
        }
    }

    LIGHTS_DBG("created interface '%s' with id '%d'", intf->name, intf->id);

    return intf;

error:
    lights_interface_destroy(intf);

    return ERR_PTR(err);
}

/**
 * lights_device_register() - Registers a new lights device
 *
 * @dev:    A decriptor of the device and its files
 * @Return: A negative error number on failure
 */
error_t lights_device_register (
    struct lights_dev *lights
){
    struct lights_interface *intf, *iter;
    error_t err = 0;

    if (IS_NULL(lights))
        return -EINVAL;

    intf = lights_interface_create(lights);
    if (IS_ERR(intf)) {
        err = PTR_ERR(intf);
        LIGHTS_ERR("create_lights_interface() returned %s", ERR_NAME(err));
        return err;
    }

    if (lights->caps) {
        err = lights_append_caps(lights->caps);
        if (err)
            goto error;
    }

    spin_lock(&lights_global.interface.lock);

    list_for_each_entry(iter, &lights_global.interface.list, siblings) {
        if (strcmp(iter->name, intf->name) == 0) {
            spin_unlock(&lights_global.interface.lock);
            err = -EEXIST;
            goto error;
        }
    }

    list_add_tail(&intf->siblings, &lights_global.interface.list);
    lights_global.interface.count++;

    spin_unlock(&lights_global.interface.lock);

    return 0;

error:
    lights_interface_destroy(intf);

    return err;
}
EXPORT_SYMBOL_NS_GPL(lights_device_register, LIGHTS);

/**
 * lights_device_unregister() - Removes a device
 *
 * @dev: A device previously registered with lights_device_register()
 */
void lights_device_unregister (
    struct lights_dev *lights
){
    struct lights_interface *intf;

    if (IS_NULL(lights))
        return;

    intf = lights_interface_find(lights);
    if (!intf) {
        LIGHTS_ERR("lights_device_unregister() failed to find interface for '%s'!", lights->name);
        return;
    }

    spin_lock(&lights_global.interface.lock);

    list_del(&intf->siblings);
    /* Remove the ref held by the list */
    kref_put(&intf->refs, lights_interface_put);
    lights_global.interface.count--;

    spin_unlock(&lights_global.interface.lock);

    /* Remove the ref created by lights_interface_find() */
    kref_put(&intf->refs, lights_interface_put);
}
EXPORT_SYMBOL_NS_GPL(lights_device_unregister, LIGHTS);

/**
 * lights_device_create_file() - Adds a file to the devices directory
 *
 * @dev:  A previously registered device
 * @attr: A description of the file to create
 *
 * @Return: A negative error number on failure
 *
 * The given attribute may exist on the stack. Internally it is copied
 * so the user need not keep a reference to it.
 */
error_t lights_device_create_file (
    struct lights_dev const *dev,
    struct lights_attribute const *attr
){
    struct lights_interface *intf;
    struct lights_file *file;
    error_t err = 0;

    if (IS_NULL(dev, attr))
        return -EINVAL;

    intf = lights_interface_find(dev);
    if (!intf) {
        LIGHTS_ERR("lights device not found (was it registered?)");
        return -ENODEV;
    }

    file = lights_file_create(intf, attr);
    if (IS_ERR(file)) {
        err = PTR_ERR(file);
        LIGHTS_ERR("Failed to create file: %s", ERR_NAME(err));
        goto exit;
    }

    spin_lock(&intf->file_lock);
    list_add_tail(&file->siblings, &intf->file_list);
    spin_unlock(&intf->file_lock);

exit:
    /* Remove the ref created by lights_interface_find() */
    kref_put(&intf->refs, lights_interface_put);

    return err;
}
EXPORT_SYMBOL_NS_GPL(lights_device_create_file, LIGHTS);

/**
 * lights_device_create_file() - Adds a file to the devices directory
 *
 * @dev:   A previously registered device
 * @attrs: Array of descriptions of the files to create
 * @count: Number of elements in @attrs
 *
 * @Return: A negative error number on failure
 */
error_t lights_device_create_files (
    struct lights_dev const *dev,
    struct lights_attribute const * const attrs,
    size_t count
){
    LIST_HEAD(file_list);
    struct lights_interface *intf;
    struct lights_file *file, *safe;
    error_t err;
    int i;

    if (IS_NULL(dev, attrs))
        return -EINVAL;

    intf = lights_interface_find(dev);
    if (!intf) {
        LIGHTS_ERR("lights device not found (was it registered?)");
        return -ENODEV;
    }

    for (i = 0; i < count; i++) {
        file = lights_file_create(intf, &attrs[i]);
        if (IS_ERR(file)) {
            err = CLEAR_ERR(file);
            LIGHTS_ERR("Failed to create file: %s", ERR_NAME(err));
            goto error;
        }

        list_add_tail(&file->siblings, &file_list);
    }

    spin_lock(&intf->file_lock);
    list_splice(&file_list, &intf->file_list);
    spin_unlock(&intf->file_lock);

    /* Remove the ref created by lights_interface_find() */
    kref_put(&intf->refs, lights_interface_put);

    return 0;

error:
    list_for_each_entry_safe(file, safe, &file_list, siblings) {
        list_del(&file->siblings);
        lights_file_destroy(file);
    }

    /* Remove the ref created by lights_interface_find() */
    kref_put(&intf->refs, lights_interface_put);

    return err;
}
EXPORT_SYMBOL_NS_GPL(lights_device_create_files, LIGHTS);


/**
 * lights_devnode() - Creates a hierarchy within the /dev directory
 * @dev:  The device in question
 * @mode: The acces flags
 *
 * @Return: The path name
 */
static char *lights_devnode (
    struct device *dev,
    umode_t *mode
){
    size_t len, i;
    const char *name;
    char *buf;

    name = dev_name(dev);
    len = strlen(name) + 9;
    buf = kmalloc(len, GFP_KERNEL);

    if (buf) {
        snprintf(buf, len, "lights/%s", name);
        for (i = 8; i < len; i++) {
            if (buf[i] == ':')
                buf[i] = '/';
        }
    }

    return buf;
}

/**
 * init_default_attributes() - Creates the character devices in /dev/lights/all/
 *
 * @return: Zero or a negative error code
 */
static error_t init_default_attributes (
    void
){
    error_t err;
    struct lights_attribute attrs[] = {
        LIGHTS_EFFECT_ATTR(NULL, io_read, io_write),
        LIGHTS_COLOR_ATTR(NULL, io_read, io_write),
        LIGHTS_SPEED_ATTR(NULL, io_read, io_write),
        LIGHTS_DIRECTION_ATTR(NULL, io_read, io_write),
        LIGHTS_UPDATE_ATTR(NULL, io_write),
        LIGHTS_SYNC_ATTR(NULL, io_write),
    };

    err = lights_device_register(&lights_global.all);
    if (err)
        return err;

    return lights_device_create_files(&lights_global.all, attrs, ARRAY_SIZE(attrs));
}

/**
 * lights_destroy() - Destroys everything
 */
void lights_destroy (
    void
){
    struct lights_interface *intf;
    struct lights_interface *intf_safe;
    dev_t dev_id = MKDEV(lights_global.major, 0);

    lights_device_unregister(&lights_global.all);

    if (!list_empty(&lights_global.interface.list)) {
        LIGHTS_WARN("Not all interfaces have been unregistered.");
        spin_lock(&lights_global.interface.lock);

        list_for_each_entry_safe(intf, intf_safe, &lights_global.interface.list, siblings) {
            list_del(&intf->siblings);
            kref_put(&intf->refs, lights_interface_put);
        }

        spin_unlock(&lights_global.interface.lock);
    }

    // lights_unregister_all_devices();
    unregister_chrdev_region(dev_id, LIGHTS_MAX_DEVICES);
    class_destroy(lights_global.class);
}

error_t lights_init (
    struct lights_state *state
){
    int err;
    dev_t dev_id;

    err = alloc_chrdev_region(&dev_id, LIGHTS_FIRST_MINOR, LIGHTS_MAX_DEVICES, "lights");
    if (err < 0) {
        LIGHTS_ERR("can't get major number");
        return err;
    }

    lights_global.state = *state;
    lights_global.major = MAJOR(dev_id);
    lights_global.class = class_create(THIS_MODULE, "lights");

    if (IS_ERR(lights_global.class)) {
        err = PTR_ERR(lights_global.class);
        unregister_chrdev_region(dev_id, LIGHTS_MAX_DEVICES);
        LIGHTS_ERR("failed to create lights_class");
        return err;
    }

    lights_global.class->devnode = lights_devnode;

    err = init_default_attributes();
    if (err)
        lights_destroy();

    return err;
}

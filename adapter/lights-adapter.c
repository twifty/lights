// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include "lights-adapter.h"

#define LIGHTS_ERR(_fmt, ...)\
    pr_err("lights: " _fmt "\n", ##__VA_ARGS__)
#define LIGHTS_WARN(_fmt, ...)\
    pr_warn("lights: " _fmt "\n", ##__VA_ARGS__)
#define LIGHTS_DBG(_fmt, ...)\
    pr_debug("lights: " _fmt "\n", ##__VA_ARGS__)
#define LIGHTS_INFO(_fmt, ...)\
    pr_info("lights: " _fmt "\n", ##__VA_ARGS__)

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(5,1,18))
static inline void list_swap(struct list_head *entry1, struct list_head *entry2)
{
    struct list_head *pos = entry2->prev;

    list_del(entry2);
    list_replace(entry1, entry2);
    if (pos == entry1)
        pos = entry2;
    list_add(entry1, pos);
}
#endif

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(5,0,0))
#define is_user_memory(_ptr, _size) access_ok(VERIFY_READ, _ptr, _size)
#else
#define is_user_memory access_ok
#endif

#ifndef EXPORT_SYMBOL_NS_GPL
#define EXPORT_SYMBOL_NS_GPL(sym, ns) EXPORT_SYMBOL_GPL(sym)
#endif

#define LIGHTS_FIRST_MINOR          0
#define LIGHTS_MAX_MINORS           512
#define LIGHTS_MAX_DEVICES          64

/*
 * Assigned numbers, used for dynamic minors
 */
static DECLARE_BITMAP(lights_minors, LIGHTS_MAX_MINORS);
static LIST_HEAD(lights_interface_list);
static DEFINE_MUTEX(lights_interface_lock);
static LIST_HEAD(lights_caps_list);

static struct lights_mode lights_available_modes[] = {
    { LIGHTS_MODE_OFF,       LIGHTS_MODE_LABEL_OFF,      },
    { LIGHTS_MODE_STATIC,    LIGHTS_MODE_LABEL_STATIC,   },
    { LIGHTS_MODE_BREATHING, LIGHTS_MODE_LABEL_BREATHING },
    { LIGHTS_MODE_FLASHING,  LIGHTS_MODE_LABEL_FLASHING  },
    { LIGHTS_MODE_CYCLE,     LIGHTS_MODE_LABEL_CYCLE     },
    { LIGHTS_MODE_RAINBOW,   LIGHTS_MODE_LABEL_RAINBOW   },
    { LIGHTS_MODE_ENDOFARRAY }
};

struct lights_caps {
    struct list_head    siblings;
    struct lights_mode  mode;
    uint32_t            ref_count;
};

/*
    A container for each character device
 */
struct lights_file {
    int                                 minor;     /* A LIGHT_CLASS minor number */
    struct cdev                         cdev;      /* The files character device */
    struct device                       *dev;      /* The files driver device */
    struct list_head                    siblings;  /* Pointers to prev and next light_dev_file */
    const struct lights_io_attribute    attr;      /* Pointer to the attributes it was created with */
    struct lights_interface             *intf;     /* The owning interface */
    struct file_operations              fops;      /* The cdev fops */
};
#define attr_to_file(ptr) \
    container_of(ptr, struct lights_file, attr)

/*
    Each interface represents a directory within the root class directory. Each
    directory may contain user defined character devices for accessing individual
    light settings.
 */
struct lights_interface {
    char                    name[LIGHTS_MAX_FILENAME_LENGTH]; /* The dir name of the light device */
    spinlock_t              file_lock;                        /* Lock for the file_list */
    struct list_head        file_list;                        /* A linked list of lights_file */
    struct list_head        siblings;                         /* Pointers to prev and next lights_interface */
    struct lights_dev       *ldev;                            /* The users object */
    struct device           kdev;
};
#define dev_to_interface(dev) \
    container_of(dev, struct lights_interface, kdev)

static int lights_major;
static struct class *lights_class;
static struct lights_state lights_global_state;
static DEFINE_SPINLOCK(lights_state_lock);

static char *default_color = "#FF0000";
static char *default_mode  = "static";
static char *default_speed = "2";


static error_t lights_add_caps (
    const struct lights_mode *mode
){
    struct lights_caps *caps;
    struct lights_caps *insert;
    int equality;

    if (WARN_ON(NULL == mode || NULL == mode->name)) {
        LIGHTS_DBG("NULL ptr detected");
        return -EINVAL;
    }

    if (lights_is_custom_mode(mode))
        return 0;

    insert = NULL;
    if (!list_empty(&lights_caps_list)) {
        list_for_each_entry(caps, &lights_caps_list, siblings) {
            equality = strcmp(caps->mode.name, mode->name);

            if (caps->mode.id == mode->id) {
                if (equality == 0) {
                    caps->ref_count++;
                    return 0;
                } else {
                    LIGHTS_ERR(
                        "mode %d:%s conflicts with known mode %d:%s",
                        mode->id, mode->name,
                        caps->mode.id, caps->mode.name
                    );
                    return -EINVAL;
                }
            }

            if (!insert && equality > 0)
                insert = caps;
        }
    }

    caps = kzalloc(sizeof(*caps), GFP_KERNEL);
    if (!caps)
        return -ENOMEM;

    caps->mode = *mode;
    caps->ref_count = 1;

    if (insert)
        list_add_tail(&caps->siblings, &insert->siblings);
    else
        list_add_tail(&caps->siblings, &lights_caps_list);

    return 0;
}

static void lights_del_caps (
    const struct lights_mode *mode
){
    struct lights_caps *caps, *safe;

    if (WARN_ON(NULL == mode)) {
        LIGHTS_DBG("NULL ptr detected");
        return;
    }

    list_for_each_entry_safe(caps, safe, &lights_caps_list, siblings) {
        if (caps->mode.id == mode->id) {
            caps->ref_count--;
            if (0 == caps->ref_count) {
                list_del(&caps->siblings);
                kfree(caps);
                return;
            }
        }
    }
}

static const struct lights_mode *lights_find_caps (
    const char *name
){
    struct lights_caps *caps;

    if (WARN_ON(NULL == name)) {
        LIGHTS_DBG("NULL ptr detected");
        return ERR_PTR(-EINVAL);
    }

    list_for_each_entry(caps, &lights_caps_list, siblings) {
        if (0 == strncmp(caps->mode.name, name, strlen(caps->mode.name))) {
            return &caps->mode;
        }
    }

    return ERR_PTR(-ENOENT);
}

static ssize_t lights_dump_caps (
    char *buffer
){
    struct list_head *interface;
    struct lights_caps *caps;
    size_t mode_len;
    ssize_t written = 0;
    uint32_t ref_count = 0;

    list_for_each(interface, &lights_interface_list)
        ref_count++;

    /* The first interface is for 'all' */
    ref_count--;
    if (ref_count == 0)
        return 0;

    list_for_each_entry(caps, &lights_caps_list, siblings) {
        if (caps->ref_count != ref_count)
            continue;

        mode_len = strlen(caps->mode.name);

        if (written + mode_len + 1 > PAGE_SIZE) {
            written = -ENOMEM;
            break;
        }

        memcpy(buffer, caps->mode.name, mode_len);
        buffer[mode_len] = '\n';

        mode_len++;
        buffer += mode_len;
        written += mode_len;
    }

    return written;
}

static ssize_t lights_dump_modes (
    const struct lights_mode *modes,
    char *buffer
){
    const struct lights_mode *iter = modes;
    size_t mode_len;
    ssize_t written = 0;

    while (iter->id != LIGHTS_MODE_ENDOFARRAY) {
        if (!iter->name || 0 == iter->name[0])
            return -EIO;

        mode_len = strlen(iter->name);

        if (written + mode_len + 1 > PAGE_SIZE) {
            written = -ENOMEM;
            break;
        }

        memcpy(buffer, iter->name, mode_len);
        buffer[mode_len] = '\n';

        mode_len++;
        buffer += mode_len;
        written += mode_len;

        iter++;
    }

    return written;
}

static error_t lights_append_caps (
    const struct lights_mode *modes
){
    const struct lights_mode *iter = modes;
    error_t err;

    if (WARN_ON(NULL == modes))
        return -EINVAL;

    while (iter->id != LIGHTS_MODE_ENDOFARRAY) {
        err = lights_add_caps(iter);
        if (err) {
            const struct lights_mode *rem = modes;
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

static void lights_remove_caps (
    const struct lights_mode *modes
){
    while (modes->id != LIGHTS_MODE_ENDOFARRAY) {
        lights_del_caps(modes);
        modes++;
    }
}


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

ssize_t lights_read_mode (
    const char *buffer,
    size_t len,
    const struct lights_mode *haystack,
    struct lights_mode *mode
){
    const struct lights_mode *p;
    char kern_buf[LIGHTS_MAX_MODENAME_LENGTH];
    size_t count;

    count = len < LIGHTS_MAX_MODENAME_LENGTH ? len : LIGHTS_MAX_MODENAME_LENGTH;
    if (is_user_memory(buffer, count)) {
        copy_from_user(kern_buf, buffer, count);
        buffer = kern_buf;
    }

    for (p = haystack; p->id != LIGHTS_MODE_ENDOFARRAY && p->name; p++) {
        if (strcmp(buffer, p->name) == 0) {
            *mode = *p;
            return 0;
        }
    }

    return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(lights_read_mode, LIGHTS);

ssize_t lights_read_speed (
    const char *buffer,
    size_t len,
    uint8_t *speed
){
    char tmp;

    if (len < 1)
        return -EINVAL;

    if (is_user_memory(buffer, 1)) {
        if (get_user(tmp, buffer))
            return -EFAULT;
    } else {
        tmp = buffer[0];
    }

    if (tmp < '1' || tmp > '5')
        return -EINVAL;

    *speed = tmp - '0';

    return 1;
}
EXPORT_SYMBOL_NS_GPL(lights_read_speed, LIGHTS);

void lights_get_state (
    struct lights_state *state
){
    *state = lights_global_state;
}
EXPORT_SYMBOL_NS_GPL(lights_get_state, LIGHTS);


static error_t update_dev_mode (
    struct lights_dev * dev,
    const struct lights_state *params
){
    return (dev->update_mode) ? dev->update_mode(params) : 0;
}

static error_t update_dev_color (
    struct lights_dev * dev,
    const struct lights_state *params
){
    return (dev->update_color) ? dev->update_color(params) : 0;
}

static error_t update_dev_speed (
    struct lights_dev * dev,
    const struct lights_state *params
){
    return (dev->update_speed) ? dev->update_speed(params) : 0;
}

static error_t for_each_device_call (
    error_t (*callback)(struct lights_dev *, const struct lights_state *)
){
    struct lights_interface *interface;
    error_t err = 0;

    mutex_lock(&lights_interface_lock);

    list_for_each_entry(interface, &lights_interface_list, siblings) {
        err = callback(interface->ldev, &lights_global_state);
        if (err)
            break;
    }

    mutex_unlock(&lights_interface_lock);

    return err;
}

static error_t color_read (
    void *data,
    struct lights_io *io
){
    spin_lock(&lights_state_lock);
    io->data.color = lights_global_state.color;
    spin_unlock(&lights_state_lock);

    return 0;
}

static error_t color_write (
    void *data,
    const struct lights_io *io
){
    spin_lock(&lights_state_lock);
    lights_global_state.color = io->data.color;
    spin_unlock(&lights_state_lock);

    // Loop through and update connected devices
    return for_each_device_call(update_dev_color);
}

static error_t mode_read (
    void *data,
    struct lights_io *io
){
    spin_lock(&lights_state_lock);
    io->data.mode = lights_global_state.mode;
    spin_unlock(&lights_state_lock);

    return 0;
}

static error_t mode_write (
    void *data,
    const struct lights_io *io
){
    spin_lock(&lights_state_lock);
    lights_global_state.mode = io->data.mode;
    spin_unlock(&lights_state_lock);

    return for_each_device_call(update_dev_mode);
}

static error_t speed_read (
    void *data,
    struct lights_io *io
){
    spin_lock(&lights_state_lock);
    io->data.speed = lights_global_state.speed;
    spin_unlock(&lights_state_lock);

    return 0;
}

static error_t speed_write (
    void *data,
    const struct lights_io *io
){
    spin_lock(&lights_state_lock);
    lights_global_state.speed = io->data.speed;
    spin_unlock(&lights_state_lock);

    return for_each_device_call(update_dev_speed);
}

static ssize_t caps_show (
    struct device *dev,
    struct device_attribute *attr,
    char *buf
){
    struct lights_interface *intf = dev_to_interface(dev);
    ssize_t written = 0;

    if (0 == strcmp(intf->name, "all")) {
        written = lights_dump_caps(buf);
    } else if (intf->ldev->caps) {
        written = lights_dump_modes(intf->ldev->caps, buf);
    }

    return written;
}
DEVICE_ATTR_RO(caps);

static ssize_t led_count_show (
    struct device *dev,
    struct device_attribute *attr,
    char *buf
){
    struct lights_interface *intf = dev_to_interface(dev);

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

static const struct attribute_group lights_class_group = {
	.attrs = lights_class_attrs,
};

static const struct attribute_group *lights_class_groups[] = {
	&lights_class_group,
	NULL,
};


static const struct lights_io_attribute *find_attribute_for_file (
    struct file *filp
){
    struct lights_interface *interface;
    struct lights_file *file_iter;
    const struct lights_io_attribute *attr;
    struct cdev *cdev;

    cdev = filp->f_inode->i_cdev;

    mutex_lock(&lights_interface_lock);

    attr = NULL;
    list_for_each_entry(interface, &lights_interface_list, siblings) {
        if (!list_empty(&interface->file_list)) {
            list_for_each_entry(file_iter, &interface->file_list, siblings) {
                if (cdev == &file_iter->cdev) {
                    attr = &file_iter->attr;
                    goto found;
                }
            }
        }
    }

found:
    mutex_unlock(&lights_interface_lock);

    return attr;
}

static ssize_t lights_color_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_COLOR
    };
    struct lights_color *c = &io.data.color;
    char color_buf[9];
    ssize_t err;

    if (*off >= 9)
        return 0;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->read)
        return -ENODEV;

    err = attr->read(attr->private_data, &io);
    if (err)
        return err;

    snprintf(color_buf, 9, "#%02X%02X%02X\n", c->r, c->g, c->b);
    err = copy_to_user(buf, color_buf, 9);
    if (err)
        return -EFAULT;

    return *off = 9;
}

static ssize_t lights_color_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_COLOR
    };
    ssize_t count, err;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->write)
        return -ENODEV;

    count = lights_read_color(buf, len, &io.data.color);
    if (count < 0)
        return count;

    err = attr->write(attr->private_data, &io);
    if (err)
        return err;

    return len;
}

static ssize_t lights_mode_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_MODE
    };
    struct lights_mode *mode = &io.data.mode;
    char mode_buf[LIGHTS_MAX_MODENAME_LENGTH];
    ssize_t count, err;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->read)
        return -ENODEV;

    err = attr->read(attr->private_data, &io);
    if (err)
        return err;

    count = strlen(mode->name) + 2;
    if (*off >= count)
        return 0;

    snprintf(mode_buf, count, "%s\n", mode->name);
    err = copy_to_user(buf, mode_buf, len < count ? len : count);
    if (err)
        return -EFAULT;

    return *off = count;
}

static ssize_t lights_mode_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_interface *intf;
    const struct lights_io_attribute *attr;
    const struct lights_mode *mode;
    struct lights_io io = {
        .type = LIGHTS_TYPE_MODE
    };
    // struct lights_mode mode;
    char kern_buf[LIGHTS_MAX_MODENAME_LENGTH + 1];
    char *name;
    size_t count, err;

    if (!len)
        return -EINVAL;

    // count = len < LIGHTS_MAX_MODENAME_LENGTH ? len : LIGHTS_MAX_MODENAME_LENGTH;
    count = min_t(size_t, len, LIGHTS_MAX_MODENAME_LENGTH);
    copy_from_user(kern_buf, buf, count);
    kern_buf[count] = 0;
    name = strim(kern_buf);

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->write)
        return -ENODEV;

    /* mode should exist within the interfaces caps */
    intf = attr_to_file(attr)->intf;

    if (0 == strcmp("all", intf->name)) {
        mode = lights_find_caps(name);
        if (IS_ERR(mode))
            return PTR_ERR(mode);
    } else {
        mode = intf->ldev->caps;
        if (mode) {
            LIGHTS_DBG("Searching '%s' for mode '%s'", intf->name, name);
            while (mode->id != LIGHTS_MODE_ENDOFARRAY) {
                if (0 == strcmp(mode->name, name))
                    goto found;
                mode++;
            }
            mode = NULL;
        }
    }

found:
    if (!mode)
        return -EINVAL;

    io.data.mode = *mode;
    err = attr->write(attr->private_data, &io);
    if (err)
        return err;

    return len;
}

static ssize_t lights_speed_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_SPEED
    };
    char speed_buf[3];
    ssize_t err;

    if (*off >= 2)
        return 0;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->read)
        return -ENODEV;

    err = attr->read(attr->private_data, &io);
    if (err)
        return err;

    speed_buf[0] = io.data.speed + '0';
    speed_buf[1] = '\n';
    speed_buf[2] = 0;

    err = copy_to_user(buf, speed_buf, len < 3 ? len : 3);
    if (err)
        return -EFAULT;

    return *off = 2;
}

static ssize_t lights_speed_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_SPEED
    };
    ssize_t err;

    err = lights_read_speed(buf, len, &io.data.speed);
    if (err)
        return err;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->write)
        return -ENODEV;

    err = attr->write(attr->private_data, &io);
    if (err)
        return err;

    return len;
}

static ssize_t lights_raw_read (
    struct file *filp,
    char __user *buf,
    size_t len,
    loff_t *off
){
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_CUSTOM
    };
    struct lights_buffer *buffer = &io.data.raw;
    ssize_t err;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->read)
        return -ENODEV;

    buffer->offset = *off;
    buffer->length = len;
    buffer->data = kmalloc(len, GFP_KERNEL);
    if (!buffer->data)
        return -ENOMEM;

    // TODO - Keep reading from callback until length is 0 or > len
    err = attr->read(attr->private_data, &io);
    if (!err) {
        copy_to_user(buf, buffer->data, buffer->length);
        *off = buffer->offset;
    }

    kfree(buffer->data);

    return err ? err : buffer->length;
}

static ssize_t lights_raw_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_CUSTOM
    };
    struct lights_buffer *buffer = &io.data.raw;
    ssize_t err;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->write)
        return -ENODEV;

    buffer->offset = *off;
    buffer->length = len;
    buffer->data = kmalloc(len, GFP_KERNEL);
    if (!buffer->data)
        return -ENOMEM;

    err = copy_from_user(buffer->data, buf, len);
    if (err) {
        err = -EIO;
        goto error_free;
    }

    err = attr->write(attr->private_data, &io);

error_free:
    kfree(buffer->data);

    return err ? err : buffer->length;
}

static ssize_t lights_leds_write (
    struct file *filp,
    const char __user *buf,
    size_t len,
    loff_t *off
){
    struct lights_interface *intf;
    const struct lights_io_attribute *attr;
    struct lights_io io = {
        .type = LIGHTS_TYPE_LEDS
    };
    struct lights_buffer *buffer = &io.data.raw;
    uint16_t led_count;
    ssize_t err;

    attr = find_attribute_for_file(filp);
    if (!attr || !attr->write)
        return -ENODEV;

    intf = attr_to_file(attr)->intf;
    led_count = intf->ldev->led_count;

    /* The buffer must account for every led */
    if (!led_count || led_count * 3 != len)
        return -EINVAL;

    buffer->offset = *off;
    buffer->length = len;
    buffer->data = kmalloc(len, GFP_KERNEL);
    if (!buffer->data)
        return -ENOMEM;

    err = copy_from_user(buffer->data, buf, len);
    if (err) {
        err = -EIO;
        goto error_free;
    }

    err = attr->write(attr->private_data, &io);

error_free:
    kfree(buffer->data);

    return err ? err : buffer->length;
}


static void lights_device_release (
    struct device *dev
){
    /* Nothing to do here */
}

static error_t file_operations_create (
    struct lights_file *file,
    const struct lights_io_attribute *attr
){
    memset(&file->fops, 0, sizeof(file->fops));

    /*
        The fops structure contains local red/write methods. Each of these
        methods will retrieve the lights_file, associated with the cdev,
        which in turn contains the user read/write functions and any
        private data associated with it.
     */
    switch (attr->type) {
        case LIGHTS_TYPE_MODE:
            file->fops.read = lights_mode_read;
            if (attr->write)
                file->fops.write = lights_mode_write;
            break;
        case LIGHTS_TYPE_COLOR:
            file->fops.read = lights_color_read;
            if (attr->write)
                file->fops.write = lights_color_write;
            break;
        case LIGHTS_TYPE_SPEED:
            file->fops.read = lights_speed_read;
            if (attr->write)
                file->fops.write = lights_speed_write;
            break;
        case LIGHTS_TYPE_CUSTOM:
            file->fops.read = lights_raw_read;
            if (attr->write)
                file->fops.write = lights_raw_write;
            break;
        case LIGHTS_TYPE_LEDS:
            if (!attr->write || attr->read) {
                LIGHTS_ERR("LIGHTS_TYPE_LEDS is write only");
                return -EINVAL;
            }
            file->fops.write = lights_leds_write;
            break;
        default:
            return -EINVAL;
    }

    file->fops.owner = attr->owner;
    memcpy((void*)&file->attr, attr, sizeof(file->attr));

    return 0;
}

static struct lights_file *lights_file_create (
    struct lights_interface *intf,
    const struct lights_io_attribute *attr
){
    int minor;
    dev_t ver;
    struct lights_file *file;
    error_t err;

    if (!attr || !intf || !attr->attr.name || attr->attr.name[0] == 0) {
        LIGHTS_ERR("create_lights_file() called with NULL ptr!");
        return ERR_PTR(-EINVAL);
    }

    minor = find_first_zero_bit(lights_minors, LIGHTS_MAX_MINORS);
    if (minor >= LIGHTS_MAX_MINORS)
        return ERR_PTR(-EBUSY);

    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (!file)
        return ERR_PTR(-ENOMEM);

    err = file_operations_create(file, attr);
    if (err)
        goto error_free_file;

    file->intf = intf;
    file->minor = minor;
    ver = MKDEV(lights_major, file->minor);

    /* Create a character device with a unique major:minor */
    cdev_init(&file->cdev, &file->fops);
    file->cdev.owner = attr->owner;
    err = cdev_add(&file->cdev, ver, 1);
    if (err)
        goto error_free_file;

    /*
     * Create a device with the same major:minor,
     * the cdev is automatically associated with it.
     *
     * The name configured here will be converted to the correct
     * path within lights_devnode().
     */
    file->dev = device_create(
        lights_class,
        &intf->kdev,
        ver,
        NULL,
        "%s:%s", intf->name, attr->attr.name
    );

    if (IS_ERR(file->dev)) {
        err = PTR_ERR(file->dev);
        goto error_free_cdev;
    }

    set_bit(minor, lights_minors);

    LIGHTS_DBG("created device '/dev/lights/%s/%s'", intf->name, attr->attr.name);

    return file;

error_free_cdev:
    cdev_del(&file->cdev);
error_free_file:
    kfree(file);

    return ERR_PTR(err);
}

static void lights_file_release (
    struct lights_file *file
){
    if (!file) {
        LIGHTS_ERR("release_lights_file() called with NULL ptr!");
        return;
    }

    device_destroy(lights_class, MKDEV(lights_major, file->minor));
    cdev_del(&file->cdev);

    if (file->minor < LIGHTS_MAX_MINORS && file->minor >= 0)
        clear_bit(file->minor, lights_minors);

    LIGHTS_DBG("removed file '/dev/lights/%s/%s'", file->intf->name, file->attr.attr.name);
    kfree(file);
}

static struct lights_interface *lights_interface_find (
    struct lights_dev *dev
){
    struct lights_interface *interface;

    if (!dev) {
        LIGHTS_ERR("find_lights_interface() called with NULL ptr!");
        return NULL;
    }

    list_for_each_entry(interface, &lights_interface_list, siblings) {
        if (interface->ldev == dev)
            goto found;
    }

    interface = NULL;

found:
    return interface;
}

static struct lights_interface *lights_interface_create (
    const char *name
){
    struct lights_interface *intf;

    if (!name || strlen(name) == 0) {
        LIGHTS_ERR("create_lights_interface() called with null or empty string");
        return ERR_PTR(-EINVAL);
    }

    list_for_each_entry(intf, &lights_interface_list, siblings) {
        if (strcmp(name, intf->name) == 0)
            goto found;
    }

    intf = NULL;

found:
    if (intf) {
        LIGHTS_ERR("create_lights_interface() interface already exists");
        return ERR_PTR(-EEXIST);
    }

    intf = kzalloc(sizeof(*intf), GFP_KERNEL);
    if (!intf) {
        return ERR_PTR(-ENOMEM);
    }

    spin_lock_init(&intf->file_lock);
    INIT_LIST_HEAD(&intf->file_list);
    strncpy(intf->name, name, LIGHTS_MAX_FILENAME_LENGTH);

    dev_set_name(&intf->kdev, intf->name);
    intf->kdev.class = lights_class;
    intf->kdev.release = lights_device_release;
    intf->kdev.groups = lights_class_groups;

    device_register(&intf->kdev);

    LIGHTS_DBG("created interface '%s'", intf->name);

    return intf;
}

static void lights_interface_release (
    struct lights_interface *intf
){
    struct lights_file *file, *file_dafe;

    if (!intf) {
        LIGHTS_ERR("release_lights_interface() called with NULL ptr!");
        return;
    }

    if (!list_empty(&intf->file_list)) {
        list_for_each_entry_safe(file, file_dafe, &intf->file_list, siblings) {
            list_del(&file->siblings);
            lights_file_release(file);
        }
    }

    if (intf->ldev->caps)
        lights_remove_caps(intf->ldev->caps);

    device_unregister(&intf->kdev);
    // device_destroy(lights_class, MKDEV(lights_major, intf->minor));
    // put_minor(intf->minor);

    LIGHTS_DBG("removed interface '%s'", intf->name);

    kfree(intf);
}

int lights_device_register (
    struct lights_dev *lights
){
    struct lights_file *file;
    struct lights_interface *intf;
    const struct lights_io_attribute **attr;
    int err = 0;

    if (!lights) {
        LIGHTS_ERR("lights_device_register() called with NULL ptr!");
        return -EINVAL;
    }

    intf = lights_interface_create(lights->name);
    if (IS_ERR(intf)) {
        LIGHTS_ERR("create_lights_interface() returned %ld!", PTR_ERR(intf));
        return PTR_ERR(intf);
    }

    intf->ldev = lights;

    if (lights->caps) {
        err = lights_append_caps(lights->caps);
        if (err)
            goto error_free_intf;
    }

    if (lights->attrs) {
        for (attr = lights->attrs; *attr; attr++) {
            file = lights_file_create(intf, *attr);

            if (IS_ERR(file)) {
                LIGHTS_ERR("Failed to create file with error %ld", PTR_ERR(file));
                err = PTR_ERR(file);
                goto error_free_files;
            }

            list_add_tail(&file->siblings, &intf->file_list);
        }
    }

    mutex_lock(&lights_interface_lock);
    list_add_tail(&intf->siblings, &lights_interface_list);
    mutex_unlock(&lights_interface_lock);

    return 0;

error_free_files:
    list_for_each_entry(file, &intf->file_list, siblings) {
        lights_file_release(file);
    }
error_free_intf:
    kfree(intf);

    return err;
}
EXPORT_SYMBOL_NS_GPL(lights_device_register, LIGHTS);

void lights_device_unregister (
    struct lights_dev *lights
){
    struct lights_interface *intf;

    if (!lights) {
        LIGHTS_ERR("lights_device_unregister() called with NULL ptr!");
        return;
    }

    intf = lights_interface_find(lights);
    if (!intf) {
        LIGHTS_ERR("lights_device_unregister() failed to find interface for '%s'!", lights->name);
        return;
    }

    mutex_lock(&lights_interface_lock);
    list_del(&intf->siblings);
    mutex_unlock(&lights_interface_lock);

    lights_interface_release(intf);
}
EXPORT_SYMBOL_NS_GPL(lights_device_unregister, LIGHTS);

error_t lights_create_file (
    struct lights_dev *dev,
    struct lights_io_attribute *attr
){
    struct lights_interface *intf;
    struct lights_file *file;

    if (WARN_ON(NULL == dev || NULL == attr))
        return -EINVAL;

    intf = lights_interface_find(dev);
    if (!intf) {
        LIGHTS_ERR("lights device not found (was it registered?)");
        return -ENODEV;
    }

    file = lights_file_create(intf, attr);
    if (IS_ERR(file)) {
        LIGHTS_ERR("Failed to create file with error %ld", PTR_ERR(file));
        return PTR_ERR(file);
    }

    list_add_tail(&file->siblings, &intf->file_list);

    return 0;
}
EXPORT_SYMBOL_NS_GPL(lights_create_file, LIGHTS);


static struct lights_dev lights_global_dev = {
    .name = "all",
    // .attr_count = 3,
    // .attributes = lights_global_attributes
};

static error_t init_default_attributes (
    void
){
    error_t err;

    err = lights_device_register(&lights_global_dev);
    if (err)
        return err;

    err = lights_create_file(&lights_global_dev, &LIGHTS_COLOR_ATTR(
        NULL,
        color_read,
        color_write
    ));
    if (err)
        return err;

    err = lights_create_file(&lights_global_dev, &LIGHTS_MODE_ATTR(
        NULL,
        mode_read,
        mode_write
    ));
    if (err)
        return err;

    err = lights_create_file(&lights_global_dev, &LIGHTS_SPEED_ATTR(
        NULL,
        speed_read,
        speed_write
    ));
    if (err)
        return err;

    return err;
}

static void lights_destroy (
    void
){
    struct lights_interface *intf;
    struct lights_interface *intf_safe;
    dev_t dev_id = MKDEV(lights_major, 0);

    lights_device_unregister(&lights_global_dev);

    if (!list_empty(&lights_interface_list)) {
        LIGHTS_WARN("Not all interfaces have been unregistered.");
        mutex_lock(&lights_interface_lock);

        list_for_each_entry_safe(intf, intf_safe, &lights_interface_list, siblings) {
            list_del(&intf->siblings);
            lights_interface_release(intf);
        }

        mutex_unlock(&lights_interface_lock);
    }

    // lights_unregister_all_devices();
    unregister_chrdev_region(dev_id, LIGHTS_MAX_DEVICES);
    class_destroy(lights_class);
}

static void __exit lights_exit (void)
{
    lights_destroy();

    LIGHTS_INFO("exiting");
}

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

static int __init lights_init (void)
{
    int err;
    dev_t dev_id;

    err = lights_read_mode(default_mode, strlen(default_mode), lights_available_modes, &lights_global_state.mode);
    if (err < 0)
        return err;

    err = lights_read_color(default_color, strlen(default_color), &lights_global_state.color);
    if (err < 0)
        return err;

    err = lights_read_speed(default_speed, strlen(default_speed), &lights_global_state.speed);
    if (err < 0)
        return err;

    err = alloc_chrdev_region(&dev_id, LIGHTS_FIRST_MINOR, LIGHTS_MAX_DEVICES, "lights");
    if (err < 0) {
        LIGHTS_WARN("can't get major number");
        return err;
    }

    lights_major = MAJOR(dev_id);
    lights_class = class_create(THIS_MODULE, "lights");

    if (IS_ERR(lights_class)) {
        err = PTR_ERR(lights_class);
        unregister_chrdev_region(dev_id, LIGHTS_MAX_DEVICES);
        LIGHTS_WARN("failed to create lights_class");
        return err;
    }

    lights_class->devnode = lights_devnode;

    err = init_default_attributes();
    if (err)
        lights_destroy();

    return err;
}

module_param(default_color, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(default_mode,  charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_param(default_speed, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
module_init(lights_init);
module_exit(lights_exit);

MODULE_PARM_DESC(default_color, "A hexadecimal color code, eg. #00FF00");
MODULE_PARM_DESC(default_mode, "The name of a color mode");
MODULE_PARM_DESC(default_speed, "The speed of the color cycle, 1-5");
MODULE_AUTHOR("Owen Parry <waldermort@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RGB Lighting Class Interface");

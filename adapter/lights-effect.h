/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_EFFECT_H
#define _UAPI_LIGHTS_ADAPTER_EFFECT_H

#define LIGHTS_EFFECT_MAX_NAME_LENGTH  32

/*
    These ids represent common modes supported by a majority
    of devices. A third party may extend upon these by using
    the hi byte. Modes are considered equal if the low byte
    id is equal (disregards string) OR hi byte id is equal
    AND string name is equal (case sensitive).
 */
enum lights_effect_id {
    LIGHTS_EFFECT_ID_INVALID      = 0x0000, /* The last item of an array should be a zeroed object*/
    LIGHTS_EFFECT_ID_OFF          = 0x0001,
    LIGHTS_EFFECT_ID_STATIC       = 0x0002,
    LIGHTS_EFFECT_ID_BREATHING    = 0x0003,
    LIGHTS_EFFECT_ID_FLASHING     = 0x0004,
    LIGHTS_EFFECT_ID_CYCLE        = 0x0005,
    LIGHTS_EFFECT_ID_RAINBOW      = 0x0006,
};

#define LIGHTS_EFFECT_LABEL_OFF       "off"
#define LIGHTS_EFFECT_LABEL_STATIC    "static"
#define LIGHTS_EFFECT_LABEL_BREATHING "breathing"
#define LIGHTS_EFFECT_LABEL_FLASHING  "flashing"
#define LIGHTS_EFFECT_LABEL_CYCLE     "cycle"
#define LIGHTS_EFFECT_LABEL_RAINBOW   "rainbow"

/**
 * struct lights_mode
 *
 * @id:   The numeric value of the mode
 * @name: max LIGHTS_MAX_MODENAME_LENGTH, snake_case name of the mode
 *
 * ALL id-name pairs must be unique, however the value is usable only
 * by the module that created it it.
 */
struct lights_effect {
    uint16_t    id;
    uint16_t    value;
    char        name[LIGHTS_EFFECT_MAX_NAME_LENGTH];
};

#define LIGHTS_EFFECT_VALUE(_value, _name) \
    { .id = LIGHTS_EFFECT_ID_ ## _name, .name = LIGHTS_EFFECT_LABEL_ ## _name, .value = (_value) }

#define LIGHTS_EFFECT_NAMED(_name) \
    LIGHTS_EFFECT_VALUE(0, _name)

#define LIGHTS_EFFECT_CUSTOM_VALUE(_id, _value, _name) \
    { .id = ((_id) << 8), .value = (_value), .name = (_name) }

#define LIGHTS_EFFECT_CUSTOM(_id, _name) \
    LIGHTS_EFFECT_CUSTOM_VALUE(_id, _id, _name)


#define lights_effect_is_custom(effect) ( \
    ((effect)->id & 0xff00) \
)

#define lights_effect_is_equal(eff1, eff2) ( \
    ((eff1)->id == (eff2)->id && 0 == memcmp((eff1)->name, (eff2)->name, LIGHTS_EFFECT_MAX_NAME_LENGTH)) \
)

#define lights_effect_copy(_head, _src, _dst) ({ \
    struct lights_effect const *__p = lights_effect_find_by_id(_head, (_src)->id); \
    if (__p) \
        _dst = *__p; \
    __p; \
})

#define lights_effect_debug(_as, _msg, effect) ( \
    _as("%sEffect('%s', id:%x, val:%x)", (_msg), (effect)->name, (effect)->id, (effect)->value) \
)

static inline struct lights_effect const *lights_effect_find_by_id (
    struct lights_effect const *haystack,
    uint16_t id
){
    while (haystack && haystack->id != LIGHTS_EFFECT_ID_INVALID) {
        if (id == haystack->id)
            return haystack;
        haystack++;
    }
    return NULL;
}

static inline struct lights_effect const *lights_effect_find_by_name (
    struct lights_effect const *haystack,
    const char *name
){
    while (haystack && haystack->id != LIGHTS_EFFECT_ID_INVALID) {
        if (0 == strcmp(haystack->name, name))
            return haystack;
        haystack++;
    }
    return NULL;
}

static inline struct lights_effect const *lights_effect_find_by_value (
    struct lights_effect const *haystack,
    uint16_t value
){
    while (haystack && haystack->id != LIGHTS_EFFECT_ID_INVALID) {
        if (value == haystack->value)
            return haystack;
        haystack++;
    }
    return NULL;
}

#endif

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_LIGHTS_ADAPTER_COLOR_H
#define _UAPI_LIGHTS_ADAPTER_COLOR_H

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

#endif

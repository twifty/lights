/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_DRIVERS_H
#define _UAPI_DRIVERS_H

#ifndef ERROR_TYPE
#define ERROR_TYPE 1
typedef s32 error_t;
#endif

#define LIGHTS_HW_ERR(_fmt, ...)( \
    pr_err("lights hw: " _fmt "\n", ##__VA_ARGS__) \
)

#define LIGHTS_HW_WARN(_fmt, ...) ( \
    pr_warn("lights hw: " _fmt "\n", ##__VA_ARGS__) \
)

#define LIGHTS_HW_DBG(_fmt, ...) ( \
    pr_debug("lights hw: " _fmt "\n", ##__VA_ARGS__) \
)

#define LIGHTS_HW_INFO(_fmt, ...) ( \
    pr_info("lights hw: " _fmt "\n", ##__VA_ARGS__) \
)

#endif

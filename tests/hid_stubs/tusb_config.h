#pragma once

/* Use the real TinyUSB HID declarations without a target USB controller. */
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUD_ENABLED 0
#define CFG_TUH_ENABLED 0
#define CFG_TUH_DEVICE_MAX 4

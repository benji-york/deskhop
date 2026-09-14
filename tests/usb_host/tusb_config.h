#pragma once
#include "../../src/include/tusb_config.h"
#define CFG_TUSB_MCU OPT_MCU_NONE
#undef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#undef CFG_TUD_ENABLED
#define CFG_TUD_ENABLED 0
#undef CFG_TUSB_RHPORT0_MODE
#undef CFG_TUSB_RHPORT1_MODE
/* A single virtual root port stands in for physical PIO root port 1. */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#undef CFG_TUH_RPI_PIO_USB

#pragma once
/* Keep the firmware's class counts, descriptors, and buffer sizes. Only the
 * controller/OS and unused physical-host side change for this device prototype. */
#include "../../src/include/tusb_config.h"
#define CFG_TUSB_MCU OPT_MCU_NONE
#undef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#undef CFG_TUH_ENABLED
#define CFG_TUH_ENABLED 0
#undef CFG_TUSB_RHPORT1_MODE
#undef CFG_TUH_RPI_PIO_USB
#define TUP_DCD_ENDPOINT_MAX 8
#define CFG_TUD_TASK_QUEUE_SZ 32

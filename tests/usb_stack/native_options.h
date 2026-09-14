#pragma once
#include "tusb_option.h"
/* Match Cortex-M0+ packed-safe access even on unaligned-capable ARM64 hosts.
 * Set after TinyUSB MCU options, before any inline common helpers are parsed. */
#undef TUP_ARCH_STRICT_ALIGN
#define TUP_ARCH_STRICT_ALIGN 1

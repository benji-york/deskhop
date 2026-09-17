#pragma once
/* This isolated HID fixture does not execute configuration transactions.
 * The paired simulator links and exercises the actual command engine. */
#include "main.h"
bool config_confirm_usb_request(const uart_packet_t *, device_t *);
void config_confirm_usb_disconnect(void);

#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t transfer_count; } dma_channel_hw_t;
bool dma_channel_is_busy(uint32_t);
void dma_channel_transfer_from_buffer_now(uint32_t, const void *, uint32_t);
dma_channel_hw_t *dma_channel_hw_addr(uint32_t);

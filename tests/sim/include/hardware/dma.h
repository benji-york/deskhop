#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t transfer_count; uintptr_t write_addr; uint32_t ctrl_trig; } dma_channel_hw_t;
typedef struct { uint32_t abort; } sim_dma_hw_t;
typedef struct { struct { uint32_t ctrdeq, tcr; } ch[3]; } sim_dma_debug_hw_t;
extern sim_dma_hw_t *dma_hw;
extern sim_dma_debug_hw_t *dma_debug_hw;
typedef struct { bool enabled; } dma_channel_config;
bool dma_channel_is_busy(uint32_t);
void dma_channel_transfer_from_buffer_now(uint32_t, const void *, uint32_t);
dma_channel_hw_t *dma_channel_hw_addr(uint32_t);
dma_channel_config dma_get_channel_config(uint32_t);
void channel_config_set_enable(dma_channel_config *, bool);
void dma_channel_set_config(uint32_t, const dma_channel_config *, bool);
void dma_channel_abort(uint32_t);
void dma_channel_set_trans_count(uint32_t, uint32_t, bool);

#pragma once

#include "dolphin/gx/GXEnum.h"
#include "../internal.hpp"

namespace aurora::gx::fifo {

// Reset the CP write-deduplication state whenever the GX shadow state is reinitialized.
void reset_cp_register_cache();

// True while a recorded display list is being replayed. The game's SDK never flushes deferred state (genMode) into a
// list, so genMode copies Aurora recorded must not switch coplanar mode off.
extern bool g_replayingDisplayList;

// Count one GXCopyDisp disarm of a still-armed bounding box (60 fps campaign instruments,
// PHASE1-DESK section 1.4): an armed window the frame end discarded unread.
void note_bbox_disarm();

// Process a buffer of GX FIFO commands
uint32_t process(const uint8_t* data, uint32_t size, bool bigEndian);

// Submit already-packed direct vertex bytes against the current GX state.
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);

} // namespace aurora::gx::fifo

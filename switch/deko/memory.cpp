// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff: switch/deko/memory.cpp
// (the image heap, its tile-size fix, the code block, the fence locking), itself adapted from
// SwitchWakerHD's runtime/src/gfx/deko/memory.cpp. The frame rings became submission serials.
// deko3d memory and submissions (dk.h):
//   - serials: R.cmd records serial N; flush() signals a fence for it, submits and starts N + 1. The
//     fences sit in a ring of kFenceRing; a submit waits only if the ring is full of unfinished work;
//   - command memory and upload space come in chunks, each tagged with the last serial that used it
//     and reused once the GPU has passed that serial;
//   - objects the recorded commands use are kept alive (shared_ptr) until their serial is done;
//   - the image heap (64 MiB chunks, first fit), blocks of their own (buffers), the shader code block
//     and the descriptor block.
#include "dk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>

namespace swdk {
namespace {

constexpr uint32_t kFenceRing = 32;
constexpr uint32_t kCmdChunkSize = 256u << 10;
constexpr uint32_t kUploadChunkSize = 4u << 20;
constexpr uint32_t kImageChunkSize = 64u << 20;

std::mutex g_queueMutex;

// deko3d aborts the process when a creation fails and never returns null: the line before names
// what was being created
DkMemBlock make_block(uint32_t size, uint32_t flags) {
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, R.device, size);
    m.flags = flags;
    DkMemBlock b = dkMemBlockCreate(&m);
    return b;
}

DkMemBlock make_block_logged(uint32_t size, uint32_t flags, const char* what) {
    dklog("creating a memory block: %s, %u KiB (flags 0x%X)", what, size >> 10, flags);
    log_flush();
    DkMemBlock b = make_block(size, flags);
    dklog("memory block: %s at GPU address 0x%llX", what, (unsigned long long)dkMemBlockGetGpuAddr(b));
    return b;
}

// ---- serials and fences (fences under g_fenceMutex: the queue writes them at submit, other threads poll)
struct FenceSlot {
    DkFence fence{};
    uint64_t serial = 0;  // 0: never used
};
std::mutex g_fenceMutex;
FenceSlot g_fences[kFenceRing];
uint64_t g_recording = 1;            // under g_queueMutex
std::atomic<uint64_t> g_completed{0};
std::atomic<uint64_t> g_submitted{0};  // the newest serial submitted

void note_completed(uint64_t serial) {
    uint64_t c = g_completed.load(std::memory_order_relaxed);
    while (serial > c && !g_completed.compare_exchange_weak(c, serial, std::memory_order_acq_rel)) {
    }
}

uint64_t poll_completed() {
    std::lock_guard<std::mutex> lock(g_fenceMutex);
    uint64_t done = g_completed.load(std::memory_order_acquire);
    for (FenceSlot& f : g_fences) {
        if (f.serial > done && dkFenceWait(&f.fence, 0) == DkResult_Success) note_completed(f.serial);
    }
    return g_completed.load(std::memory_order_acquire);
}

// ---- chunks of command memory and upload space (under g_queueMutex)
struct Chunk {
    DkMemBlock block = nullptr;
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
    uint32_t size = 0;
    uint64_t lastSerial = 0;
};
struct ChunkPool {
    const char* what;
    uint32_t chunkSize;
    uint32_t flags;
    std::vector<Chunk> idle;  // lastSerial may still be in flight: checked when taken
    uint64_t created = 0;

    Chunk take(uint32_t minSize) {
        const uint64_t done = poll_completed();
        for (size_t i = 0; i < idle.size(); i++) {
            if (idle[i].lastSerial <= done && idle[i].size >= minSize) {
                Chunk c = idle[i];
                idle[i] = idle.back();
                idle.pop_back();
                return c;
            }
        }
        Chunk c;
        c.size = align_up(std::max(minSize, chunkSize), DK_MEMBLOCK_ALIGNMENT);
        c.block = make_block(c.size, flags);
        c.cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(c.block));
        c.gpu = dkMemBlockGetGpuAddr(c.block);
        created++;
        if (created <= 4 || created % 16 == 0)
            dklog("%s: chunk %llu (%u KiB)", what, (unsigned long long)created, c.size >> 10);
        return c;
    }
    void give_back(Chunk c, uint64_t serial) {
        c.lastSerial = serial;
        idle.push_back(c);
    }
};
ChunkPool g_cmdPool{"command memory", kCmdChunkSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached};
ChunkPool g_uploadPool{"upload space", kUploadChunkSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached};
std::vector<Chunk> g_cmdInUse;  // fed to R.cmd during the recording serial
Chunk g_upload;                 // the current upload chunk
uint32_t g_uploadUsed = 0;

void add_cmd_memory(void*, DkCmdBuf cmd, size_t minReqSize) {
    Chunk c = g_cmdPool.take(uint32_t(minReqSize));
    dkCmdBufAddMemory(cmd, c.block, 0, c.size);
    g_cmdInUse.push_back(c);
}

// ---- objects kept until their serial is done (under g_queueMutex)
std::vector<std::shared_ptr<void>> g_keepNow;
std::deque<std::pair<uint64_t, std::vector<std::shared_ptr<void>>>> g_keepLater;

void release_done(uint64_t done) {
    while (!g_keepLater.empty() && g_keepLater.front().first <= done) {
        // destructors run here: they free image memory, blocks and descriptor slots, never take the queue lock
        g_keepLater.pop_front();
    }
}

// ---- image heap (g_heapMutex)
struct HeapChunk {
    DkMemBlock block;
    uint32_t size;
    std::vector<std::pair<uint32_t, uint32_t>> free;  // offset, size; sorted by offset
};
std::mutex g_heapMutex;
std::vector<HeapChunk> g_chunks;
MemoryStats g_stats;  // image/block figures under g_heapMutex

void add_chunk(uint32_t size) {
    char what[64];
    snprintf(what, sizeof what, "image heap chunk %zu", g_chunks.size());
    g_chunks.push_back(
        {make_block_logged(size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, what), size, {{0, size}}});
    g_stats.imageChunks = g_chunks.size();
}

void chunk_free(HeapChunk& c, uint32_t offset, uint32_t size) {
    auto it = std::lower_bound(c.free.begin(), c.free.end(), std::make_pair(offset, 0u));
    it = c.free.insert(it, {offset, size});
    if (it + 1 != c.free.end() && it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        c.free.erase(it + 1);
    }
    if (it != c.free.begin() && (it - 1)->first + (it - 1)->second == it->first) {
        (it - 1)->second += it->second;
        c.free.erase(it);
    }
}

DkMemBlock g_code = nullptr, g_descriptors = nullptr;
std::mutex g_codeMutex;
uint32_t g_codeUsed = 0;
std::atomic<bool> g_codeWritten{false};

// ---- GPU time (dk.h gpu_time_*; all under g_queueMutex) ------------------------------------------------
// One record per command list (serial), in a ring of kGtRing indexed by serial. Its reports: [0] the
// list's start, [1] its end, then a begin/end pair per render pass. A deko3d counter report is 16 bytes,
// the GPU timestamp at +8 (as spm-nx's gpupass reads it). The fence ring (kFenceRing = 32) keeps the
// recording serial at most 32 ahead of a finished one, and records are drained (in serial order) before
// a new one opens, so a ring of 64 never reuses a slot the GPU may still write.
constexpr uint32_t kGtRing = 64, kGtMaxPasses = 48, kGtReports = 2 + 2 * kGtMaxPasses;
struct GtReport {
    uint64_t payload;
    uint64_t timestamp;
};
static_assert(sizeof(GtReport) == 16, "deko3d counter reports are 16 bytes");
struct GtPass {
    uint32_t w, h, colors;
    bool depth, ended;
};
struct GtRec {
    uint64_t serial = 0;  // 0: free
    uint64_t frame = 0;   // R.presents when the list closed: the present it belongs to
    uint32_t passes = 0;
    bool overflow = false, open = false;
    GtPass pass[kGtMaxPasses];
};
Block g_gtSink;
GtRec g_gtRing[kGtRing];
GtRec* g_gtCur = nullptr;
// the window (since the last gpu_time_summary): frames closed, their GPU time, the lists behind them
struct GtPassKind {
    uint32_t w = 0, h = 0, colors = 0;
    bool depth = false;
    uint64_t count = 0;
    double ns = 0;
};
constexpr uint32_t kGtKinds = 32;
struct GtWindow {
    uint64_t frames = 0, lists = 0, invalid = 0, dropped = 0, passOverflow = 0;
    double ns = 0, maxNs = 0;
    GtPassKind kinds[kGtKinds];
    uint32_t nKinds = 0;
} g_gtWin;
uint64_t g_gtFrame = ~0ull;  // the frame being summed (records drain in serial order)
double g_gtFrameNs = 0;

GtReport* gt_report(uint64_t serial, uint32_t i) {
    return reinterpret_cast<GtReport*>(g_gtSink.cpu) + (serial % kGtRing) * kGtReports + i;
}

void gt_stamp(uint64_t serial, uint32_t i) {
    GtReport* r = gt_report(serial, i);
    r->timestamp = 0;  // still 0 at read-back: the GPU never wrote it (the record is dropped)
    dkCmdBufReportCounter(R.cmd, DkCounter_Timestamp, g_gtSink.gpu + ((serial % kGtRing) * kGtReports + i) * sizeof(GtReport));
}

double gt_ns(uint64_t a, uint64_t b) { return b > a ? double(dkTimestampToNs(b - a)) : 0.0; }

void gt_close_frame() {
    if (g_gtFrame == ~0ull) return;
    g_gtWin.frames++;
    g_gtWin.ns += g_gtFrameNs;
    g_gtWin.maxNs = std::max(g_gtWin.maxNs, g_gtFrameNs);
    g_gtFrameNs = 0;
}

void gt_fold(GtRec& rec) {
    const uint64_t s = rec.serial;
    const uint64_t t0 = gt_report(s, 0)->timestamp, t1 = gt_report(s, 1)->timestamp;
    if (rec.frame != g_gtFrame) {
        gt_close_frame();
        g_gtFrame = rec.frame;
    }
    if (rec.overflow) g_gtWin.passOverflow++;
    if (t0 == 0 || t1 == 0 || t1 < t0) {
        g_gtWin.invalid++;
        return;
    }
    g_gtWin.lists++;
    g_gtFrameNs += gt_ns(t0, t1);
    for (uint32_t i = 0; i < rec.passes; i++) {
        const GtPass& p = rec.pass[i];
        if (!p.ended) continue;
        const uint64_t a = gt_report(s, 2 + 2 * i)->timestamp, b = gt_report(s, 3 + 2 * i)->timestamp;
        if (a == 0 || b < a) continue;
        uint32_t k = 0;
        while (k < g_gtWin.nKinds && !(g_gtWin.kinds[k].w == p.w && g_gtWin.kinds[k].h == p.h &&
                                       g_gtWin.kinds[k].colors == p.colors && g_gtWin.kinds[k].depth == p.depth))
            k++;
        if (k == g_gtWin.nKinds) {
            if (k == kGtKinds) continue;  // (more than 32 kinds in a window: the rest go uncounted)
            g_gtWin.kinds[k] = GtPassKind{p.w, p.h, p.colors, p.depth, 0, 0};
            g_gtWin.nKinds++;
        }
        g_gtWin.kinds[k].count++;
        g_gtWin.kinds[k].ns += gt_ns(a, b);
    }
}

// Folds every closed record whose serial the GPU has finished, oldest first; stops at the first that
// has not (never waits).
void gt_drain(uint64_t completed) {
    for (;;) {
        GtRec* oldest = nullptr;
        for (GtRec& r : g_gtRing)
            if (r.serial && !r.open && r.serial <= completed && (!oldest || r.serial < oldest->serial)) oldest = &r;
        if (!oldest) return;
        gt_fold(*oldest);
        oldest->serial = 0;
    }
}

// At the start of a list (memory_init, then every flush): its record and start stamp.
void gt_open() {
    if (!g_gtSink) {
        g_gtSink = block_create(kGtRing * kGtReports * sizeof(GtReport), "GPU timestamps");
        std::memset(g_gtSink.cpu, 0, kGtRing * kGtReports * sizeof(GtReport));
    }
    GtRec& r = g_gtRing[g_recording % kGtRing];
    if (r.serial) {  // not drained yet (should not happen, see above): this list goes untimed
        g_gtWin.dropped++;
        g_gtCur = nullptr;
        return;
    }
    r.serial = g_recording;
    r.passes = 0;
    r.overflow = false;
    r.open = true;
    g_gtCur = &r;
    gt_stamp(r.serial, 0);
}

// In flush, before the list's fence: its end stamp.
void gt_close() {
    if (!g_gtCur) return;
    gt_stamp(g_gtCur->serial, 1);
    g_gtCur->frame = R.presents;
    g_gtCur->open = false;
    g_gtCur = nullptr;
}

}  // namespace

void gpu_time_pass_begin(uint32_t w, uint32_t h, uint32_t colors, bool depth) {
    if (!g_gtCur) return;
    if (g_gtCur->passes >= kGtMaxPasses) {
        g_gtCur->overflow = true;
        return;
    }
    const uint32_t i = g_gtCur->passes++;
    g_gtCur->pass[i] = GtPass{w, h, colors, depth, false};
    gt_stamp(g_gtCur->serial, 2 + 2 * i);
}

void gpu_time_pass_end() {
    if (!g_gtCur || g_gtCur->passes == 0) return;
    GtPass& p = g_gtCur->pass[g_gtCur->passes - 1];
    if (p.ended) return;  // the pass's begin overflowed: this end is not its
    p.ended = true;
    gt_stamp(g_gtCur->serial, 2 + 2 * (g_gtCur->passes - 1) + 1);
}

void gpu_time_summary(char* gpu, size_t gpuSize, char* passes, size_t passesSize) {
    GtWindow& w = g_gtWin;
    const double frames = w.frames ? double(w.frames) : 1.0;
    std::snprintf(gpu, gpuSize, "gpu avg %.2f ms max %.2f ms over %llu frames (%.1f lists/frame, %llu unread, %llu dropped)",
                  w.ns / frames / 1e6, w.maxNs / 1e6, (unsigned long long)w.frames, double(w.lists) / frames,
                  (unsigned long long)w.invalid, (unsigned long long)w.dropped);
    passes[0] = 0;
    // the five kinds with the most GPU time: ms per frame, passes per frame, target
    bool used[kGtKinds] = {};
    size_t at = 0;
    for (int n = 0; n < 5; n++) {
        int best = -1;
        for (uint32_t k = 0; k < w.nKinds; k++)
            if (!used[k] && (best < 0 || w.kinds[k].ns > w.kinds[best].ns)) best = int(k);
        if (best < 0 || at >= passesSize) break;
        used[best] = true;
        const GtPassKind& k = w.kinds[best];
        const int len = std::snprintf(passes + at, passesSize - at, "%s%ux%u c%u%s %.2f ms/f (%.1f/f)", n ? "; " : "",
                                      k.w, k.h, k.colors, k.depth ? "+d" : "", k.ns / frames / 1e6,
                                      double(k.count) / frames);
        if (len < 0) break;
        at += size_t(len);
    }
    if (w.passOverflow && at < passesSize)
        std::snprintf(passes + at, passesSize - at, "; %llu lists over %u passes (rest untimed)",
                      (unsigned long long)w.passOverflow, kGtMaxPasses);
    const uint64_t frame = g_gtFrame;
    const double frameNs = g_gtFrameNs;
    w = GtWindow{};
    g_gtFrame = frame;  // the frame in progress carries over
    g_gtFrameNs = frameNs;
}

std::mutex& queue_mutex() { return g_queueMutex; }

void memory_init() {
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, R.device);
    cm.cbAddMem = add_cmd_memory;
    dklog("creating the command buffer");
    log_flush();
    R.cmd = dkCmdBufCreate(&cm);
    g_code = make_block_logged(kCodeSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                               "shader code");
    char what[96];
    snprintf(what, sizeof what, "descriptors (%u images, %u samplers)", kImageDescriptors, kSamplerDescriptors);
    g_descriptors = make_block_logged(align_up((kImageDescriptors + kSamplerDescriptors) * 32, DK_MEMBLOCK_ALIGNMENT),
                                      DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, what);
    {
        std::lock_guard<std::mutex> lock(g_heapMutex);
        add_chunk(kImageChunkSize);  // the first image heap chunk now: its size shows in the set-up cost
    }
    std::lock_guard<std::mutex> lock(g_queueMutex);
    add_cmd_memory(nullptr, R.cmd, kCmdChunkSize);
    descriptors_bind();
    gt_open();  // GPU time: the first list's start stamp
}

uint64_t recording_serial() { return g_recording; }

void flush() {
    check_queue("a submit");
    const uint64_t serial = g_recording;
    FenceSlot& slot = g_fences[serial % kFenceRing];
    if (slot.serial != 0 && slot.serial > g_completed.load(std::memory_order_acquire)) {
        // the ring is full of unfinished work: the oldest submit has to finish first
        DkFence copy;
        {
            std::lock_guard<std::mutex> lock(g_fenceMutex);
            copy = slot.fence;
        }
        if (dkFenceWait(&copy, 10'000'000'000ll) != DkResult_Success)
            dklog("submit %llu: the GPU has not finished submit %llu after 10 s", (unsigned long long)serial,
                  (unsigned long long)slot.serial);
        note_completed(slot.serial);
    }
    gt_close();  // GPU time: the list's end stamp, before its fence
    dkCmdBufSignalFence(R.cmd, &slot.fence, true);
    const DkCmdList list = dkCmdBufFinishList(R.cmd);
    {
        std::lock_guard<std::mutex> lock(g_fenceMutex);
        dkQueueSubmitCommands(R.queue, list);
        slot.serial = serial;
    }
    dkQueueFlush(R.queue);
    g_submitted.store(serial, std::memory_order_release);
    g_stats.submits++;
    for (const Chunk& c : g_cmdInUse) g_cmdPool.give_back(c, serial);
    g_cmdInUse.clear();
    if (g_upload.block) g_upload.lastSerial = serial;
    if (!g_keepNow.empty()) {
        g_keepLater.emplace_back(serial, std::move(g_keepNow));
        g_keepNow = {};
    }
    g_recording = serial + 1;
    // dkCmdBufClear rewinds to the start of the memory fed last (deko3d 0.5.0 keeps it), which the
    // submitted list still uses: feed a fresh chunk explicitly (as deko_examples' CCmdMemRing)
    dkCmdBufClear(R.cmd);
    add_cmd_memory(nullptr, R.cmd, kCmdChunkSize);
    const uint64_t done = poll_completed();
    release_done(done);
    descriptors_reclaim();
    descriptors_bind();
    gt_drain(done);  // GPU time: the finished lists' stamps (never waits), then the next list's start
    gt_open();
}

uint64_t completed_serial() { return poll_completed(); }

bool wait_serial(uint64_t serial) {
    if (serial == 0 || serial <= g_completed.load(std::memory_order_acquire)) return true;
    if (serial > g_submitted.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (serial >= g_recording) flush();
    }
    for (int waited = 0; waited < 5; waited++) {
        if (poll_completed() >= serial) return true;
        DkFence copy;
        uint64_t slotSerial;
        {
            std::lock_guard<std::mutex> lock(g_fenceMutex);
            const FenceSlot& slot = g_fences[serial % kFenceRing];
            copy = slot.fence;
            slotSerial = slot.serial;
        }
        if (slotSerial != serial) return true;  // overwritten: only after it was finished
        if (dkFenceWait(&copy, 2'000'000'000ll) == DkResult_Success) {
            note_completed(serial);
            return true;
        }
        dklog("waiting for submit %llu: not finished after %d s%s", (unsigned long long)serial, (waited + 1) * 2,
              dkQueueIsInErrorState(R.queue) ? "; the queue is in an error state" : "");
    }
    return false;
}

void keep_until_done(std::shared_ptr<void> obj) {
    if (obj) g_keepNow.push_back(std::move(obj));
}

void keep_until_done(std::vector<std::shared_ptr<void>>&& objs) {
    if (g_keepNow.empty()) {
        g_keepNow = std::move(objs);
        return;
    }
    g_keepNow.insert(g_keepNow.end(), std::make_move_iterator(objs.begin()), std::make_move_iterator(objs.end()));
    objs.clear();
}

UploadAlloc upload_alloc(uint32_t size, uint32_t alignment) {
    alignment = std::max<uint32_t>(alignment, 16);
    uint32_t at = align_up(g_uploadUsed, alignment);
    if (!g_upload.block || at + size > g_upload.size) {
        if (g_upload.block) g_uploadPool.give_back(g_upload, g_recording);
        g_upload = g_uploadPool.take(size);
        g_uploadUsed = 0;
        at = 0;
    }
    g_upload.lastSerial = g_recording;
    g_uploadUsed = at + size;
    return {g_upload.cpu + at, g_upload.gpu + at};
}

void image_tile_size_fix(DkImageLayoutMaker& m, uint32_t rows) {
    if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) return;
    if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D ||
        m.type == DkImageType_Buffer)
        return;  // (3D images: deko3d's depth tile never shrinks at level 0)
    if (m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) {
        if (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x) rows *= 2;  // the multisampled height
    }
    // deko3d's choice (dkImageLayoutInitialize: pickTileSize of 1.5 x the height in GOBs) ...
    const uint32_t gobs = (rows + rows / 2 + 7) / 8;
    const uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
    // ... and its level-0 shrink (calcLevelOffset: adjustTileSize(m_tileH, 8, height))
    uint32_t shrunk = tile;
    while (shrunk && (8u << (shrunk - 1)) >= rows) shrunk--;
    if (shrunk == tile) return;  // deko3d's own layout is consistent
    m.flags |= DkImageFlags_CustomTileSize;
    m.tileSize = DkTileSize(shrunk);
}

ImageAlloc image_alloc(uint32_t size, uint32_t alignment) {
    size = align_up(size, 256);
    alignment = std::max<uint32_t>(alignment, 256);
    std::lock_guard<std::mutex> lock(g_heapMutex);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t ci = 0; ci < g_chunks.size(); ci++) {
            HeapChunk& c = g_chunks[ci];
            for (size_t i = 0; i < c.free.size(); i++) {
                auto [off, len] = c.free[i];
                const uint32_t at = align_up(off, alignment);
                if (at - off + size > len) continue;
                c.free.erase(c.free.begin() + long(i));
                if (at > off) chunk_free(c, off, at - off);
                if (at + size < off + len) chunk_free(c, at + size, off + len - at - size);
                g_stats.imageBytes += size;
                return {c.block, at, size, int(ci)};
            }
        }
        add_chunk(std::max(kImageChunkSize, align_up(size + alignment, DK_MEMBLOCK_ALIGNMENT)));
    }
    fatal("image heap: cannot place %u bytes", size);
}

void image_free(const ImageAlloc& a) {
    if (a.chunk < 0) return;
    std::lock_guard<std::mutex> lock(g_heapMutex);
    chunk_free(g_chunks[size_t(a.chunk)], a.offset, a.size);
    g_stats.imageBytes -= a.size;
}

Block block_create(uint32_t size, const char* what, bool cpuCached) {
    size = align_up(std::max<uint32_t>(size, 1), DK_MEMBLOCK_ALIGNMENT);
    Block b;
    (void)what;
    b.cpuCached = cpuCached;
    b.block = make_block(size, (cpuCached ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached) |
                                   DkMemBlockFlags_GpuCached);
    b.cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(b.block));
    b.gpu = dkMemBlockGetGpuAddr(b.block);
    b.size = size;
    std::lock_guard<std::mutex> lock(g_heapMutex);
    g_stats.blockBytes += size;
    g_stats.blocks++;
    return b;
}

void block_destroy(Block& b) {
    if (!b.block) return;
    dkMemBlockDestroy(b.block);
    {
        std::lock_guard<std::mutex> lock(g_heapMutex);
        g_stats.blockBytes -= b.size;
        g_stats.blocks--;
    }
    b = Block{};
}

bool code_take_written() { return g_codeWritten.exchange(false, std::memory_order_acq_rel); }

bool code_load(DkShader& shader, const void* dksh, uint32_t size, const char* name) {
    std::lock_guard<std::mutex> lock(g_codeMutex);
    const uint32_t at = align_up(g_codeUsed, DK_SHADER_CODE_ALIGNMENT);
    if (at + size > kCodeSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
        static int logged = 0;
        if (logged++ < 10) dklog("shader code memory full: %s (%u bytes) not loaded", name, size);
        return false;
    }
    uint8_t* const dst = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_code)) + at;
    memcpy(dst, dksh, size);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, g_code, at);
    dkShaderInitialize(&shader, &m);
    if (!dkShaderIsValid(&shader)) {
        dklog("shader %s (%u bytes of DKSH) is not valid", name, size);
        return false;
    }
    g_codeUsed = at + size;
    g_codeWritten.store(true, std::memory_order_release);
    return true;
}

uint32_t code_used() {
    std::lock_guard<std::mutex> lock(g_codeMutex);
    return g_codeUsed;
}

DkGpuAddr image_descriptors() { return dkMemBlockGetGpuAddr(g_descriptors); }
DkGpuAddr sampler_descriptors() { return dkMemBlockGetGpuAddr(g_descriptors) + kImageDescriptors * 32; }

MemoryStats memory_stats() {
    MemoryStats s;
    {
        std::lock_guard<std::mutex> lock(g_heapMutex);
        s = g_stats;
    }
    s.uploadChunks = g_uploadPool.created;
    s.cmdChunks = g_cmdPool.created;
    return s;
}

}  // namespace swdk

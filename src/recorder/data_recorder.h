#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "queue/spsc_queue.h"
#include "recorder/journal_types.h"
#include "recorder/record_types.h"

namespace lt {

// Configuration for the data recorder (24/7 bulletproof)
struct DataRecorderConfig {
    std::string output_dir = "data";         // base output directory
    int64_t flush_interval_ms = 1000;        // how often to flush writes to disk
    int64_t min_disk_space_mb = 500;         // minimum free disk space before pausing writes
    bool enabled = false;
};

// DataRecorder: background thread that drains per-source SPSC queues and writes
// timestamped records to hourly-rotated binary files.
//
// File layout per source:
//   {output_dir}/rtds/YYYYMMDD_HH.bin    — fixed 64B records
//   {output_dir}/market/YYYYMMDD_HH.bin  — variable-length raw JSON
//   {output_dir}/user/YYYYMMDD_HH.bin    — variable-length raw JSON
//
// Fixed files: [RecordFileHeader 16B] [Record...Record...]
// Raw files:   [RecordFileHeader 16B] [RawRecordHeader 24B][JSON]...[RawRecordHeader 24B][JSON]...
//
// Bulletproof features:
//   - Hourly file rotation (bounded file sizes)
//   - Disk space monitoring (pauses writes when low, resumes on recovery)
//   - Append mode (crash-safe, restarts within same hour append to file)
//   - 64KB stdio write buffer (batched I/O, minimal syscalls)
//   - Non-blocking queue drain (try_push on producer side)
//   - Graceful degradation (write errors close file, retry on next rotation)
//
// Thread ownership: T_rec (dedicated recorder thread).
class DataRecorder {
public:
    // Any queue pointer may be nullptr to skip that source.
    // market_queue/user_queue carry raw WS JSON payloads (variable-length on disk).
    DataRecorder(const DataRecorderConfig& config,
                 SpscQueue<RtdsRecord>* rtds_queue = nullptr,
                 SpscQueue<RawWsMessage>* market_queue = nullptr,
                 SpscQueue<RawWsMessage>* user_queue = nullptr,
                 SpscQueue<JournalRecord>* journal_queue = nullptr);
    ~DataRecorder();

    DataRecorder(const DataRecorder&) = delete;
    DataRecorder& operator=(const DataRecorder&) = delete;

    void run();
    void request_shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt

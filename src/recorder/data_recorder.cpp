#include "recorder/data_recorder.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace lt {

struct DataRecorder::Impl {
    DataRecorderConfig config;
    SpscQueue<RtdsRecord>* rtds_q;
    SpscQueue<RawWsMessage>* market_q;
    SpscQueue<RawWsMessage>* user_q;
    SpscQueue<JournalRecord>* journal_q;
    std::atomic<bool> stop_requested{false};

    FILE* rtds_file = nullptr;
    FILE* market_file = nullptr;
    FILE* user_file = nullptr;
    FILE* journal_file = nullptr;

    int64_t current_rotation_hour = -1;
    bool disk_low = false;
    int64_t last_flush_ms = 0;
    int records_since_flush = 0;

    Impl(const DataRecorderConfig& cfg,
         SpscQueue<RtdsRecord>* rq,
         SpscQueue<RawWsMessage>* mq,
         SpscQueue<RawWsMessage>* uq,
         SpscQueue<JournalRecord>* jq)
        : config(cfg), rtds_q(rq), market_q(mq), user_q(uq), journal_q(jq) {}

    ~Impl() { close_all(); }

    // ---- File management ----

    void close_file(FILE*& f) {
        if (f) {
            std::fflush(f);
            std::fclose(f);
            f = nullptr;
        }
    }

    void close_all() {
        close_file(rtds_file);
        close_file(market_file);
        close_file(user_file);
        close_file(journal_file);
    }

    FILE* open_record_file(const char* subdir, RecordSource src, uint32_t rec_size) {
        namespace fs = std::filesystem;

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &tt);
#else
        localtime_r(&tt, &tm_buf);
#endif
        char ts[20];
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H", &tm_buf);

        std::string dir = config.output_dir + "/" + subdir;
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return nullptr;

        std::string path = dir + "/" + ts + ".bin";
        bool needs_header = !fs::exists(path, ec) || fs::file_size(path, ec) == 0;

        FILE* f = std::fopen(path.c_str(), "ab");
        if (!f) return nullptr;

        // 64KB write buffer for batched I/O
        std::setvbuf(f, nullptr, _IOFBF, 65536);

        if (needs_header) {
            RecordFileHeader hdr{};
            hdr.source = static_cast<uint8_t>(src);
            hdr.record_size = rec_size;
            if (std::fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
                std::fclose(f);
                return nullptr;
            }
            std::fflush(f);
        }

        return f;
    }

    // ---- Rotation & disk space ----

    void check_rotation() {
        auto now = std::chrono::system_clock::now();
        auto hours_since_epoch = std::chrono::duration_cast<std::chrono::hours>(
            now.time_since_epoch()).count();

        if (hours_since_epoch == current_rotation_hour) return;
        current_rotation_hour = hours_since_epoch;

        close_all();

        // Check disk space
        std::error_code ec;
        auto si = std::filesystem::space(config.output_dir, ec);
        if (!ec && si.available != static_cast<std::uintmax_t>(-1)) {
            auto min_bytes = static_cast<std::uintmax_t>(config.min_disk_space_mb) * 1024 * 1024;
            if (si.available < min_bytes) {
                disk_low = true;
                return;
            }
        }
        disk_low = false;

        // Open new hourly files (record_size=0 for variable-length raw sources)
        if (rtds_q) {
            rtds_file = open_record_file("rtds", RecordSource::RTDS, sizeof(RtdsRecord));
        }
        if (market_q) {
            market_file = open_record_file("market", RecordSource::MARKET_RAW, 0);
        }
        if (user_q) {
            user_file = open_record_file("user", RecordSource::USER_RAW, 0);
        }
        if (journal_q) {
            journal_file = open_record_file("journal", RecordSource::JOURNAL, sizeof(JournalRecord));
        }
    }

    // ---- Queue drain: fixed-size records (RTDS) ----

    template <typename T>
    int drain_queue(SpscQueue<T>* q, FILE*& f) {
        if (!q) return 0;
        int count = 0;
        while (auto* rec = q->front()) {
            if (f && !disk_low) {
                if (std::fwrite(rec, sizeof(T), 1, f) != 1) {
                    close_file(f);
                }
            }
            q->pop();
            ++count;
        }
        return count;
    }

    // ---- Queue drain: variable-length raw JSON (market/user WS) ----

    int drain_raw_queue(SpscQueue<RawWsMessage>* q, FILE*& f) {
        if (!q) return 0;
        int count = 0;
        while (auto* rec = q->front()) {
            if (f && !disk_low) {
                // Write 24-byte header then payload
                RawRecordHeader hdr;
                hdr.recv_ts = rec->recv_ts;
                hdr.wall_clock_ms = rec->wall_clock_ms;
                hdr.payload_len = rec->payload_len;
                hdr.flags = rec->flags;
                bool ok = std::fwrite(&hdr, sizeof(hdr), 1, f) == 1;
                if (ok && rec->payload_len > 0) {
                    ok = std::fwrite(rec->payload, rec->payload_len, 1, f) == 1;
                }
                if (!ok) close_file(f);
            }
            q->pop();
            ++count;
        }
        return count;
    }

    // ---- Main loop ----

    void run() {
        std::error_code ec;
        std::filesystem::create_directories(config.output_dir, ec);

        while (!stop_requested.load(std::memory_order_relaxed)) {
            check_rotation();

            int total = 0;
            total += drain_queue(rtds_q, rtds_file);
            total += drain_raw_queue(market_q, market_file);
            total += drain_raw_queue(user_q, user_file);
            total += drain_queue(journal_q, journal_file);

            records_since_flush += total;

            // Periodic flush
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (records_since_flush > 0 &&
                (now_ms - last_flush_ms) >= config.flush_interval_ms) {
                if (rtds_file) std::fflush(rtds_file);
                if (market_file) std::fflush(market_file);
                if (user_file) std::fflush(user_file);
                if (journal_file) std::fflush(journal_file);
                last_flush_ms = now_ms;
                records_since_flush = 0;
            }

            if (total == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // Final drain — all producers stopped before T_rec in shutdown cascade
        drain_queue(rtds_q, rtds_file);
        drain_raw_queue(market_q, market_file);
        drain_raw_queue(user_q, user_file);
        drain_queue(journal_q, journal_file);
        close_all();
    }
};

// ---- Public API ----

DataRecorder::DataRecorder(const DataRecorderConfig& config,
                           SpscQueue<RtdsRecord>* rtds_queue,
                           SpscQueue<RawWsMessage>* market_queue,
                           SpscQueue<RawWsMessage>* user_queue,
                           SpscQueue<JournalRecord>* journal_queue)
    : impl_(std::make_unique<Impl>(config, rtds_queue, market_queue, user_queue, journal_queue)) {}

DataRecorder::~DataRecorder() {
    assert(impl_->stop_requested.load(std::memory_order_relaxed) &&
           "DataRecorder destroyed without request_shutdown() + join()");
    impl_.reset();
}

void DataRecorder::run() {
    impl_->run();
}

void DataRecorder::request_shutdown() {
    impl_->stop_requested.store(true, std::memory_order_relaxed);
}

}  // namespace lt

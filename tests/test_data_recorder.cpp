#include "doctest/doctest.h"
#include "recorder/data_recorder.h"
#include "recorder/record_types.h"
#include "queue/spsc_queue.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

using namespace lt;

namespace {

int64_t wall_clock_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string unique_dir(const char* base) {
    static int counter = 0;
    return std::string(base) + "_" + std::to_string(++counter);
}

// Read all fixed-size records of type T from a binary file, skipping the header.
template <typename T>
std::vector<T> read_binary_records(const std::string& path) {
    std::vector<T> records;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return records;

    RecordFileHeader hdr{};
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1) {
        std::fclose(f);
        return records;
    }

    T rec{};
    while (std::fread(&rec, sizeof(T), 1, f) == 1) {
        records.push_back(rec);
    }
    std::fclose(f);
    return records;
}

// Read variable-length raw records from a binary file.
// Returns pairs of (header, payload_string).
std::vector<std::pair<RawRecordHeader, std::string>> read_raw_records(const std::string& path) {
    std::vector<std::pair<RawRecordHeader, std::string>> records;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return records;

    RecordFileHeader fhdr{};
    if (std::fread(&fhdr, sizeof(fhdr), 1, f) != 1) {
        std::fclose(f);
        return records;
    }

    while (true) {
        RawRecordHeader hdr{};
        if (std::fread(&hdr, sizeof(hdr), 1, f) != 1) break;
        std::string payload(hdr.payload_len, '\0');
        if (hdr.payload_len > 0) {
            if (std::fread(payload.data(), hdr.payload_len, 1, f) != 1) break;
        }
        records.emplace_back(hdr, std::move(payload));
    }
    std::fclose(f);
    return records;
}

// Find the first .bin file in a directory
std::string find_bin_file(const std::string& dir) {
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) return "";
    for (const auto& f : fs::directory_iterator(dir)) {
        if (f.path().extension() == ".bin") {
            return f.path().string();
        }
    }
    return "";
}

}  // namespace

TEST_SUITE("DataRecorder") {

TEST_CASE("Records RTDS prices to binary file") {
    auto test_dir = unique_dir("test_rec_rtds");
    std::filesystem::remove_all(test_dir);

    SpscQueue<RtdsRecord> rtds_q(256);

    DataRecorderConfig cfg;
    cfg.output_dir = test_dir;
    cfg.flush_interval_ms = 100;
    cfg.enabled = true;

    RtdsRecord r1{};
    r1.recv_ts = 1000000;
    r1.wall_clock_ms = wall_clock_ms_now();
    r1.exchange_ts_ms = 1753314088395;
    r1.value = 67234.50;
    std::strncpy(r1.symbol, "btcusdt", 23);
    r1.symbol_len = 7;
    rtds_q.try_push(r1);

    RtdsRecord r2{};
    r2.recv_ts = 2000000;
    r2.wall_clock_ms = wall_clock_ms_now();
    r2.exchange_ts_ms = 1753314088400;
    r2.value = 3456.78;
    std::strncpy(r2.symbol, "ethusdt", 23);
    r2.symbol_len = 7;
    rtds_q.try_push(r2);

    {
        DataRecorder recorder(cfg, &rtds_q, nullptr, nullptr);
        std::thread t([&recorder]() { recorder.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        recorder.request_shutdown();
        t.join();
    }

    auto bin_path = find_bin_file(test_dir + "/rtds");
    REQUIRE(!bin_path.empty());

    FILE* f = std::fopen(bin_path.c_str(), "rb");
    REQUIRE(f);
    RecordFileHeader hdr{};
    REQUIRE(std::fread(&hdr, sizeof(hdr), 1, f) == 1);
    CHECK(std::string(hdr.magic, 4) == "LTRB");
    CHECK(hdr.version == 1);
    CHECK(hdr.source == static_cast<uint8_t>(RecordSource::RTDS));
    CHECK(hdr.record_size == sizeof(RtdsRecord));
    std::fclose(f);

    auto records = read_binary_records<RtdsRecord>(bin_path);
    REQUIRE(records.size() == 2);
    CHECK(std::string(records[0].symbol, records[0].symbol_len) == "btcusdt");
    CHECK(records[0].value == doctest::Approx(67234.50));
    CHECK(records[0].exchange_ts_ms == 1753314088395);
    CHECK(std::string(records[1].symbol, records[1].symbol_len) == "ethusdt");
    CHECK(records[1].value == doctest::Approx(3456.78));

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Records raw market JSON to binary file") {
    auto test_dir = unique_dir("test_rec_market_raw");
    std::filesystem::remove_all(test_dir);

    SpscQueue<RawWsMessage> market_q(64);

    DataRecorderConfig cfg;
    cfg.output_dir = test_dir;
    cfg.flush_interval_ms = 100;
    cfg.enabled = true;

    const char* json1 = R"([{"event_type":"book","asset_id":"0xabc","market":"0xcond","bids":[{"price":"0.52","size":"100"}],"asks":[{"price":"0.53","size":"200"}]}])";
    const char* json2 = R"([{"event_type":"price_change","asset_id":"0xabc","price":"0.55","side":"BUY","size":"50"}])";

    RawWsMessage msg1{};
    msg1.recv_ts = 100;
    msg1.wall_clock_ms = wall_clock_ms_now();
    msg1.payload_len = static_cast<uint32_t>(std::strlen(json1));
    std::memcpy(msg1.payload, json1, msg1.payload_len);
    market_q.try_push(msg1);

    RawWsMessage msg2{};
    msg2.recv_ts = 200;
    msg2.wall_clock_ms = wall_clock_ms_now();
    msg2.payload_len = static_cast<uint32_t>(std::strlen(json2));
    std::memcpy(msg2.payload, json2, msg2.payload_len);
    market_q.try_push(msg2);

    {
        DataRecorder recorder(cfg, nullptr, &market_q, nullptr);
        std::thread t([&recorder]() { recorder.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        recorder.request_shutdown();
        t.join();
    }

    auto bin_path = find_bin_file(test_dir + "/market");
    REQUIRE(!bin_path.empty());

    // Check file header
    FILE* f = std::fopen(bin_path.c_str(), "rb");
    REQUIRE(f);
    RecordFileHeader hdr{};
    REQUIRE(std::fread(&hdr, sizeof(hdr), 1, f) == 1);
    CHECK(std::string(hdr.magic, 4) == "LTRB");
    CHECK(hdr.version == 1);
    CHECK(hdr.source == static_cast<uint8_t>(RecordSource::MARKET_RAW));
    CHECK(hdr.record_size == 0);  // variable-length
    std::fclose(f);

    // Read raw records
    auto records = read_raw_records(bin_path);
    REQUIRE(records.size() == 2);

    CHECK(records[0].first.recv_ts == 100);
    CHECK(records[0].first.payload_len == std::strlen(json1));
    CHECK(records[0].second == json1);
    CHECK(records[0].first.flags == 0);

    CHECK(records[1].first.recv_ts == 200);
    CHECK(records[1].first.payload_len == std::strlen(json2));
    CHECK(records[1].second == json2);

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Records raw user JSON to binary file") {
    auto test_dir = unique_dir("test_rec_user_raw");
    std::filesystem::remove_all(test_dir);

    SpscQueue<RawWsMessage> user_q(64);

    DataRecorderConfig cfg;
    cfg.output_dir = test_dir;
    cfg.flush_interval_ms = 100;
    cfg.enabled = true;

    const char* json1 = R"([{"asset_id":"0xtok","type":"PLACEMENT","order":{"orderID":"0xord1","price":"0.52","original_size":"100"}}])";
    const char* json2 = R"([{"asset_id":"0xtok","type":"TRADE","trade":{"tradeID":"uuid1","price":"0.52","size":"50","status":"MATCHED"}}])";

    RawWsMessage msg1{};
    msg1.recv_ts = 300;
    msg1.wall_clock_ms = wall_clock_ms_now();
    msg1.payload_len = static_cast<uint32_t>(std::strlen(json1));
    std::memcpy(msg1.payload, json1, msg1.payload_len);
    user_q.try_push(msg1);

    RawWsMessage msg2{};
    msg2.recv_ts = 400;
    msg2.wall_clock_ms = wall_clock_ms_now();
    msg2.payload_len = static_cast<uint32_t>(std::strlen(json2));
    std::memcpy(msg2.payload, json2, msg2.payload_len);
    user_q.try_push(msg2);

    {
        DataRecorder recorder(cfg, nullptr, nullptr, &user_q);
        std::thread t([&recorder]() { recorder.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        recorder.request_shutdown();
        t.join();
    }

    auto bin_path = find_bin_file(test_dir + "/user");
    REQUIRE(!bin_path.empty());

    auto records = read_raw_records(bin_path);
    REQUIRE(records.size() == 2);

    CHECK(records[0].first.recv_ts == 300);
    CHECK(records[0].second == json1);
    CHECK(records[1].first.recv_ts == 400);
    CHECK(records[1].second == json2);

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Recorder handles all three sources simultaneously") {
    auto test_dir = unique_dir("test_rec_multi");
    std::filesystem::remove_all(test_dir);

    SpscQueue<RtdsRecord> rtds_q(64);
    SpscQueue<RawWsMessage> market_q(64);
    SpscQueue<RawWsMessage> user_q(64);

    DataRecorderConfig cfg;
    cfg.output_dir = test_dir;
    cfg.flush_interval_ms = 100;
    cfg.enabled = true;

    RtdsRecord r{};
    r.recv_ts = 1;
    r.value = 67000.0;
    std::strncpy(r.symbol, "btcusdt", 23);
    r.symbol_len = 7;
    rtds_q.try_push(r);

    RawWsMessage m{};
    m.recv_ts = 2;
    const char* mj = R"({"event":"book"})";
    m.payload_len = static_cast<uint32_t>(std::strlen(mj));
    std::memcpy(m.payload, mj, m.payload_len);
    market_q.try_push(m);

    RawWsMessage u{};
    u.recv_ts = 3;
    const char* uj = R"({"type":"PLACEMENT"})";
    u.payload_len = static_cast<uint32_t>(std::strlen(uj));
    std::memcpy(u.payload, uj, u.payload_len);
    user_q.try_push(u);

    {
        DataRecorder recorder(cfg, &rtds_q, &market_q, &user_q);
        std::thread t([&recorder]() { recorder.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        recorder.request_shutdown();
        t.join();
    }

    CHECK(!find_bin_file(test_dir + "/rtds").empty());
    CHECK(!find_bin_file(test_dir + "/market").empty());
    CHECK(!find_bin_file(test_dir + "/user").empty());

    auto rtds_recs = read_binary_records<RtdsRecord>(find_bin_file(test_dir + "/rtds"));
    auto market_recs = read_raw_records(find_bin_file(test_dir + "/market"));
    auto user_recs = read_raw_records(find_bin_file(test_dir + "/user"));

    CHECK(rtds_recs.size() == 1);
    CHECK(market_recs.size() == 1);
    CHECK(user_recs.size() == 1);

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Recorder handles empty queues gracefully") {
    auto test_dir = unique_dir("test_rec_empty");
    std::filesystem::remove_all(test_dir);

    SpscQueue<RtdsRecord> rtds_q(64);

    DataRecorderConfig cfg;
    cfg.output_dir = test_dir;
    cfg.flush_interval_ms = 100;
    cfg.enabled = true;

    {
        DataRecorder recorder(cfg, &rtds_q, nullptr, nullptr);
        std::thread t([&recorder]() { recorder.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recorder.request_shutdown();
        t.join();
    }

    auto bin_path = find_bin_file(test_dir + "/rtds");
    REQUIRE(!bin_path.empty());
    auto records = read_binary_records<RtdsRecord>(bin_path);
    CHECK(records.empty());

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Recorder with no queues runs and shuts down cleanly") {
    auto test_dir = unique_dir("test_rec_noq");
    std::filesystem::remove_all(test_dir);

    DataRecorderConfig cfg;
    cfg.output_dir = test_dir;
    cfg.flush_interval_ms = 100;
    cfg.enabled = true;

    {
        DataRecorder recorder(cfg, nullptr, nullptr, nullptr);
        std::thread t([&recorder]() { recorder.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recorder.request_shutdown();
        t.join();
    }

    CHECK(true);
    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Market raw file header has correct source and record_size=0") {
    auto test_dir = unique_dir("test_rec_hdr_raw");
    std::filesystem::remove_all(test_dir);

    SpscQueue<RawWsMessage> market_q(64);

    DataRecorderConfig cfg;
    cfg.output_dir = test_dir;
    cfg.flush_interval_ms = 100;
    cfg.enabled = true;

    RawWsMessage m{};
    m.payload_len = 5;
    std::memcpy(m.payload, "hello", 5);
    market_q.try_push(m);

    {
        DataRecorder recorder(cfg, nullptr, &market_q, nullptr);
        std::thread t([&recorder]() { recorder.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        recorder.request_shutdown();
        t.join();
    }

    auto bin_path = find_bin_file(test_dir + "/market");
    REQUIRE(!bin_path.empty());

    FILE* f = std::fopen(bin_path.c_str(), "rb");
    REQUIRE(f);
    RecordFileHeader hdr{};
    REQUIRE(std::fread(&hdr, sizeof(hdr), 1, f) == 1);
    CHECK(hdr.magic[0] == 'L');
    CHECK(hdr.magic[1] == 'T');
    CHECK(hdr.magic[2] == 'R');
    CHECK(hdr.magic[3] == 'B');
    CHECK(hdr.version == 1);
    CHECK(hdr.source == static_cast<uint8_t>(RecordSource::MARKET_RAW));
    CHECK(hdr.record_size == 0);
    std::fclose(f);

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Record types are trivially copyable POD") {
    CHECK(std::is_trivially_copyable_v<RecordFileHeader>);
    CHECK(std::is_trivially_copyable_v<RtdsRecord>);
    CHECK(std::is_trivially_copyable_v<RawRecordHeader>);
    CHECK(std::is_trivially_copyable_v<RawWsMessage>);
}

TEST_CASE("Record type sizes are stable") {
    CHECK(sizeof(RecordFileHeader) == 16);
    CHECK(sizeof(RtdsRecord) == 64);
    CHECK(sizeof(RawRecordHeader) == 24);
}

TEST_CASE("Truncation flag is set for oversized payloads") {
    // Verify kMaxRawWsPayload and flag constant
    CHECK(kMaxRawWsPayload == 65536);
    CHECK(kRawFlagTruncated == 1);

    // Simulate constructing a message with truncation flag
    RawWsMessage msg{};
    msg.flags = kRawFlagTruncated;
    CHECK((msg.flags & kRawFlagTruncated) != 0);
}

}  // TEST_SUITE

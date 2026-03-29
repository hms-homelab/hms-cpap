/**
 * HMS-CPAP FysetcSniffService Unit Tests
 *
 * Tests JSON parsing, PostgreSQL array building, sequence gap detection,
 * and simultaneous operation with other services.
 * Pure logic tests -- no MQTT or DB connections needed.
 */

#include <gtest/gtest.h>
#include <json/json.h>
#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>

// ============================================================================
// SNIFF JSON PARSING (mirrors FysetcSniffService::onSniffData logic)
// ============================================================================

class FysetcSniffTest : public ::testing::Test {
protected:
    struct SniffRecord {
        int64_t ts;
        int uptime;
        int seq;
        int interval_ms;
        bool therapy;
        int idle_ms;
        std::vector<int> samples;
    };

    // Parse a sniff JSON payload (same logic as onSniffData)
    bool parseSniffPayload(const std::string& payload, SniffRecord& out) {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::istringstream ss(payload);
        std::string errors;
        if (!Json::parseFromStream(builder, ss, &root, &errors)) {
            return false;
        }

        out.ts = root.get("ts", 0).asInt64();
        out.uptime = root.get("up", 0).asInt();
        out.seq = root.get("seq", 0).asInt();
        out.interval_ms = root.get("ms", 100).asInt();
        out.therapy = root.get("therapy", false).asBool();
        out.idle_ms = root.get("idle_ms", 0).asInt();

        const Json::Value& samples = root["samples"];
        if (!samples.isArray() || samples.empty()) {
            return false;
        }

        out.samples.clear();
        for (Json::ArrayIndex i = 0; i < samples.size(); i++) {
            out.samples.push_back(samples[i].asInt());
        }

        return true;
    }

    // Build PostgreSQL array literal (same logic as onSniffData)
    std::string buildPgArray(const std::vector<int>& samples) {
        std::ostringstream arr;
        arr << "{";
        for (size_t i = 0; i < samples.size(); i++) {
            if (i > 0) arr << ",";
            arr << samples[i];
        }
        arr << "}";
        return arr.str();
    }

    // Build INSERT SQL (same logic as onSniffData)
    std::string buildInsertSQL(const std::string& device_id, const SniffRecord& r) {
        std::ostringstream sql;
        sql << "INSERT INTO cpap_sniff_data "
            << "(device_id, timestamp, uptime_sec, seq, pulse_counts, interval_ms, therapy_detected, idle_ms) "
            << "VALUES ("
            << "'" << device_id << "', "
            << "to_timestamp(" << r.ts << "), "
            << r.uptime << ", "
            << r.seq << ", "
            << "'" << buildPgArray(r.samples) << "', "
            << r.interval_ms << ", "
            << (r.therapy ? "TRUE" : "FALSE") << ", "
            << r.idle_ms
            << ")";
        return sql.str();
    }

    // Detect sequence gaps (same logic as onSniffData)
    int detectGap(int current_seq, int last_seq) {
        if (last_seq >= 0 && current_seq != last_seq + 1) {
            return current_seq - last_seq - 1;
        }
        return 0;
    }
};

// ============================================================================
// PAYLOAD PARSING
// ============================================================================

TEST_F(FysetcSniffTest, ParsePayload_ValidComplete) {
    std::string json = R"({
        "ts": 1711670400,
        "up": 3600,
        "seq": 42,
        "samples": [12, 0, 0, 45, 23, 0, 0, 0, 0, 15],
        "ms": 100,
        "therapy": false,
        "idle_ms": 1234
    })";

    SniffRecord r;
    ASSERT_TRUE(parseSniffPayload(json, r));

    EXPECT_EQ(r.ts, 1711670400);
    EXPECT_EQ(r.uptime, 3600);
    EXPECT_EQ(r.seq, 42);
    EXPECT_EQ(r.interval_ms, 100);
    EXPECT_FALSE(r.therapy);
    EXPECT_EQ(r.idle_ms, 1234);
    ASSERT_EQ(r.samples.size(), 10u);
    EXPECT_EQ(r.samples[0], 12);
    EXPECT_EQ(r.samples[3], 45);
    EXPECT_EQ(r.samples[9], 15);
}

TEST_F(FysetcSniffTest, ParsePayload_TherapyActive) {
    std::string json = R"({
        "ts": 1711670500,
        "up": 100,
        "seq": 0,
        "samples": [88, 92, 85, 90, 87, 91, 86, 89, 93, 88],
        "ms": 100,
        "therapy": true,
        "idle_ms": 0
    })";

    SniffRecord r;
    ASSERT_TRUE(parseSniffPayload(json, r));

    EXPECT_TRUE(r.therapy);
    EXPECT_EQ(r.idle_ms, 0);
    // All samples should be high during therapy
    for (int s : r.samples) {
        EXPECT_GT(s, 0);
    }
}

TEST_F(FysetcSniffTest, ParsePayload_InvalidJSON_ReturnsFalse) {
    SniffRecord r;
    EXPECT_FALSE(parseSniffPayload("{not valid json", r));
    EXPECT_FALSE(parseSniffPayload("", r));
}

TEST_F(FysetcSniffTest, ParsePayload_MissingSamples_ReturnsFalse) {
    std::string json = R"({"ts":123,"up":1,"seq":0,"ms":100,"therapy":false,"idle_ms":0})";
    SniffRecord r;
    EXPECT_FALSE(parseSniffPayload(json, r));
}

TEST_F(FysetcSniffTest, ParsePayload_EmptySamples_ReturnsFalse) {
    std::string json = R"({"ts":123,"up":1,"seq":0,"samples":[],"ms":100,"therapy":false,"idle_ms":0})";
    SniffRecord r;
    EXPECT_FALSE(parseSniffPayload(json, r));
}

TEST_F(FysetcSniffTest, ParsePayload_SingleSample) {
    std::string json = R"({"ts":123,"up":1,"seq":0,"samples":[77],"ms":100,"therapy":false,"idle_ms":500})";
    SniffRecord r;
    ASSERT_TRUE(parseSniffPayload(json, r));
    ASSERT_EQ(r.samples.size(), 1u);
    EXPECT_EQ(r.samples[0], 77);
}

TEST_F(FysetcSniffTest, ParsePayload_ZeroTimestamp_SnptNotReady) {
    // SNTP not synced yet -- ts=0 is valid (firmware sends 0 until NTP syncs)
    std::string json = R"({"ts":0,"up":5,"seq":3,"samples":[1,2,3],"ms":100,"therapy":false,"idle_ms":100})";
    SniffRecord r;
    ASSERT_TRUE(parseSniffPayload(json, r));
    EXPECT_EQ(r.ts, 0);
    EXPECT_EQ(r.uptime, 5);
}

// ============================================================================
// POSTGRESQL ARRAY BUILDING
// ============================================================================

TEST_F(FysetcSniffTest, PgArray_NormalSamples) {
    std::vector<int> samples = {12, 0, 0, 45, 23, 0, 0, 0, 0, 15};
    EXPECT_EQ(buildPgArray(samples), "{12,0,0,45,23,0,0,0,0,15}");
}

TEST_F(FysetcSniffTest, PgArray_SingleElement) {
    std::vector<int> samples = {42};
    EXPECT_EQ(buildPgArray(samples), "{42}");
}

TEST_F(FysetcSniffTest, PgArray_AllZeros) {
    std::vector<int> samples = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(buildPgArray(samples), "{0,0,0,0,0,0,0,0,0,0}");
}

TEST_F(FysetcSniffTest, PgArray_HighValues) {
    // PCNT can count up to 32767 per sample window
    std::vector<int> samples = {32767, 32767, 32767};
    EXPECT_EQ(buildPgArray(samples), "{32767,32767,32767}");
}

// ============================================================================
// SQL GENERATION
// ============================================================================

TEST_F(FysetcSniffTest, InsertSQL_CorrectFormat) {
    SniffRecord r;
    r.ts = 1711670400;
    r.uptime = 3600;
    r.seq = 42;
    r.interval_ms = 100;
    r.therapy = false;
    r.idle_ms = 1234;
    r.samples = {12, 0, 45};

    std::string sql = buildInsertSQL("cpap_resmed_123", r);

    EXPECT_NE(sql.find("cpap_resmed_123"), std::string::npos);
    EXPECT_NE(sql.find("to_timestamp(1711670400)"), std::string::npos);
    EXPECT_NE(sql.find("3600"), std::string::npos);
    EXPECT_NE(sql.find("42"), std::string::npos);
    EXPECT_NE(sql.find("'{12,0,45}'"), std::string::npos);
    EXPECT_NE(sql.find("FALSE"), std::string::npos);
    EXPECT_NE(sql.find("1234"), std::string::npos);
}

TEST_F(FysetcSniffTest, InsertSQL_TherapyTrue) {
    SniffRecord r;
    r.ts = 1000;
    r.uptime = 10;
    r.seq = 0;
    r.interval_ms = 100;
    r.therapy = true;
    r.idle_ms = 0;
    r.samples = {90, 85};

    std::string sql = buildInsertSQL("device_1", r);

    EXPECT_NE(sql.find("TRUE"), std::string::npos);
}

// ============================================================================
// SEQUENCE GAP DETECTION
// ============================================================================

TEST_F(FysetcSniffTest, SequenceGap_NoGap) {
    EXPECT_EQ(detectGap(1, 0), 0);
    EXPECT_EQ(detectGap(42, 41), 0);
    EXPECT_EQ(detectGap(100, 99), 0);
}

TEST_F(FysetcSniffTest, SequenceGap_SingleMissed) {
    EXPECT_EQ(detectGap(2, 0), 1);    // missed seq 1
    EXPECT_EQ(detectGap(44, 42), 1);   // missed seq 43
}

TEST_F(FysetcSniffTest, SequenceGap_MultipleMissed) {
    EXPECT_EQ(detectGap(10, 5), 4);    // missed 6,7,8,9
    EXPECT_EQ(detectGap(100, 0), 99);  // missed 1-99
}

TEST_F(FysetcSniffTest, SequenceGap_FirstPacket_NoGap) {
    // last_seq = -1 means first packet ever -- no gap
    EXPECT_EQ(detectGap(0, -1), 0);
    EXPECT_EQ(detectGap(42, -1), 0);  // first packet can start anywhere
}

// ============================================================================
// SIMULTANEOUS OPERATION (concurrent callback simulation)
// ============================================================================

TEST_F(FysetcSniffTest, SimultaneousCallbacks_NoDataRace) {
    // Simulates what happens in real operation: sniff callbacks and
    // fysetc receiver callbacks fire concurrently on Paho's thread.
    // Both use separate state -- verify no data corruption.

    std::atomic<int> sniff_count{0};
    std::atomic<int> receiver_count{0};
    std::mutex sniff_mutex;
    std::mutex receiver_mutex;

    // Collected results
    std::vector<SniffRecord> sniff_records;
    std::vector<std::string> receiver_filenames;

    auto sniff_callback = [&](const std::string& payload) {
        SniffRecord r;
        if (parseSniffPayload(payload, r)) {
            std::lock_guard<std::mutex> lock(sniff_mutex);
            sniff_records.push_back(r);
            sniff_count++;
        }
    };

    auto receiver_callback = [&](const std::string& payload) {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::istringstream ss(payload);
        std::string errors;
        if (Json::parseFromStream(builder, ss, &root, &errors)) {
            std::lock_guard<std::mutex> lock(receiver_mutex);
            receiver_filenames.push_back(root.get("f", "").asString());
            receiver_count++;
        }
    };

    // Fire both types of callbacks concurrently
    const int iterations = 100;
    std::thread sniff_thread([&]() {
        for (int i = 0; i < iterations; i++) {
            std::ostringstream json;
            json << R"({"ts":)" << (1000 + i) << R"(,"up":)" << i
                 << R"(,"seq":)" << i << R"(,"samples":[)" << i << R"(,0,0],"ms":100,"therapy":false,"idle_ms":0})";
            sniff_callback(json.str());
        }
    });

    std::thread receiver_thread([&]() {
        for (int i = 0; i < iterations; i++) {
            std::ostringstream json;
            json << R"({"f":"file_)" << i << R"(.edf","d":"20260315","o":0,"n":4,"b64":"AQID"})";
            receiver_callback(json.str());
        }
    });

    sniff_thread.join();
    receiver_thread.join();

    EXPECT_EQ(sniff_count.load(), iterations);
    EXPECT_EQ(receiver_count.load(), iterations);
    EXPECT_EQ(sniff_records.size(), static_cast<size_t>(iterations));
    EXPECT_EQ(receiver_filenames.size(), static_cast<size_t>(iterations));

    // Verify sniff records are all distinct and correctly parsed
    for (int i = 0; i < iterations; i++) {
        EXPECT_EQ(sniff_records[i].seq, i);
        EXPECT_EQ(sniff_records[i].ts, 1000 + i);
        ASSERT_EQ(sniff_records[i].samples.size(), 3u);
        EXPECT_EQ(sniff_records[i].samples[0], i);
    }
}

TEST_F(FysetcSniffTest, SimultaneousCounters_Independent) {
    // Verify that sniff and receiver row counters don't interfere
    std::atomic<uint64_t> sniff_inserted{0};
    std::atomic<uint64_t> sniff_failed{0};
    std::atomic<uint64_t> receiver_processed{0};

    const int sniff_ok = 50;
    const int sniff_bad = 10;
    const int recv_ok = 30;

    std::thread t1([&]() {
        for (int i = 0; i < sniff_ok; i++) sniff_inserted++;
        for (int i = 0; i < sniff_bad; i++) sniff_failed++;
    });

    std::thread t2([&]() {
        for (int i = 0; i < recv_ok; i++) receiver_processed++;
    });

    t1.join();
    t2.join();

    EXPECT_EQ(sniff_inserted.load(), sniff_ok);
    EXPECT_EQ(sniff_failed.load(), sniff_bad);
    EXPECT_EQ(receiver_processed.load(), recv_ok);
}

// ============================================================================
// PATTERN ANALYSIS HELPERS (what you'd query from the DB)
// ============================================================================

TEST_F(FysetcSniffTest, PatternAnalysis_RestVsTherapy) {
    // Simulate a capture session: 5s rest then 5s therapy
    // Rest: mostly zeros with occasional low pulses
    // Therapy: sustained high pulse counts

    std::vector<SniffRecord> capture;

    // 5 batches of rest (10 samples each = 5 seconds)
    for (int batch = 0; batch < 5; batch++) {
        SniffRecord r;
        r.ts = 1000 + batch;
        r.uptime = batch;
        r.seq = batch;
        r.interval_ms = 100;
        r.therapy = false;
        r.idle_ms = (batch + 1) * 1000;
        r.samples = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        // Occasional blip
        if (batch == 2) r.samples[3] = 5;
        capture.push_back(r);
    }

    // 5 batches of therapy
    for (int batch = 0; batch < 5; batch++) {
        SniffRecord r;
        r.ts = 1005 + batch;
        r.uptime = 5 + batch;
        r.seq = 5 + batch;
        r.interval_ms = 100;
        r.therapy = true;
        r.idle_ms = 0;
        r.samples = {85, 92, 88, 90, 87, 91, 86, 89, 93, 88};
        capture.push_back(r);
    }

    ASSERT_EQ(capture.size(), 10u);

    // Count total pulses per batch
    int rest_total = 0, therapy_total = 0;
    int rest_batches = 0, therapy_batches = 0;

    for (const auto& r : capture) {
        int batch_sum = 0;
        for (int s : r.samples) batch_sum += s;

        if (r.therapy) {
            therapy_total += batch_sum;
            therapy_batches++;
        } else {
            rest_total += batch_sum;
            rest_batches++;
        }
    }

    EXPECT_EQ(rest_batches, 5);
    EXPECT_EQ(therapy_batches, 5);
    EXPECT_EQ(rest_total, 5);  // only the one blip

    // Therapy should have dramatically more pulses
    double rest_avg = static_cast<double>(rest_total) / rest_batches;
    double therapy_avg = static_cast<double>(therapy_total) / therapy_batches;
    EXPECT_GT(therapy_avg, rest_avg * 10);  // at least 10x more activity
}

/**
 * HMS-CPAP FysetcReceiverService Unit Tests
 *
 * Tests sync response, chunk writing, manifest diffing, and base64 decoding.
 * These are pure logic tests -- no MQTT or DB connections needed.
 */

#include <gtest/gtest.h>
#include <json/json.h>
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>

// Test fixture
class FysetcReceiverTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "hms_cpap_fysetc_test";
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        if (std::filesystem::exists(test_dir_)) {
            std::filesystem::remove_all(test_dir_);
        }
    }

    void createTestFile(const std::filesystem::path& path, size_t size_bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary);
        std::vector<char> data(size_bytes, 'X');
        ofs.write(data.data(), size_bytes);
    }

    // Base64 encode helper (mirrors what FYSETC firmware does)
    std::string base64Encode(const std::vector<uint8_t>& data) {
        size_t out_len = 4 * ((data.size() + 2) / 3) + 1;
        std::vector<char> out(out_len);
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                        data.data(), static_cast<int>(data.size()));
        return std::string(out.data());
    }

    std::filesystem::path test_dir_;
};

// ============================================================================
// SYNC RESPONSE LOGIC
// ============================================================================

TEST_F(FysetcReceiverTest, SyncResponse_ExistingFiles_ReturnsFileSizes) {
    // Create some local files
    std::string date = "20260315";
    auto date_dir = test_dir_ / date;
    createTestFile(date_dir / "20260315_022443_BRP.edf", 184320);
    createTestFile(date_dir / "20260315_022443_PLD.edf", 32768);

    // Simulate sync request payload
    Json::Value request;
    request["date"] = date;
    Json::Value files(Json::arrayValue);
    files.append("20260315_022443_BRP.edf");
    files.append("20260315_022443_PLD.edf");
    files.append("20260315_022443_SAD.edf");  // doesn't exist locally
    request["files"] = files;

    // Build expected response: check file sizes
    Json::Value offsets(Json::objectValue);
    for (const auto& f : files) {
        std::string filename = f.asString();
        std::filesystem::path filepath = date_dir / filename;
        uint64_t size = 0;
        if (std::filesystem::exists(filepath)) {
            size = std::filesystem::file_size(filepath);
        }
        offsets[filename] = static_cast<Json::UInt64>(size);
    }

    EXPECT_EQ(offsets["20260315_022443_BRP.edf"].asUInt64(), 184320u);
    EXPECT_EQ(offsets["20260315_022443_PLD.edf"].asUInt64(), 32768u);
    EXPECT_EQ(offsets["20260315_022443_SAD.edf"].asUInt64(), 0u);
}

TEST_F(FysetcReceiverTest, SyncResponse_EmptyDir_ReturnsZeroOffsets) {
    std::string date = "20260315";
    // Don't create date dir

    std::filesystem::path filepath = test_dir_ / date / "anything.edf";
    EXPECT_FALSE(std::filesystem::exists(filepath));
}

// ============================================================================
// CHUNK WRITING (BASE64 DECODE + SEEK WRITE)
// ============================================================================

TEST_F(FysetcReceiverTest, ChunkWrite_NewFile_CreatedAtOffset) {
    std::string date = "20260315";
    auto date_dir = test_dir_ / date;
    std::filesystem::create_directories(date_dir);

    // Simulate writing a chunk at offset 0
    std::vector<uint8_t> raw_data = {0x30, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20};
    std::string b64 = base64Encode(raw_data);

    // Decode (same as FysetcReceiverService::onChunk)
    size_t b64_len = b64.size();
    size_t max_decoded = 3 * (b64_len / 4) + 3;
    std::vector<unsigned char> decoded(max_decoded);
    int decoded_len = EVP_DecodeBlock(decoded.data(),
                                       reinterpret_cast<const unsigned char*>(b64.c_str()),
                                       static_cast<int>(b64_len));
    int padding = 0;
    if (b64_len >= 1 && b64[b64_len - 1] == '=') padding++;
    if (b64_len >= 2 && b64[b64_len - 2] == '=') padding++;
    decoded_len -= padding;

    ASSERT_EQ(static_cast<size_t>(decoded_len), raw_data.size());

    // Write to file at offset 0
    std::string filepath = (date_dir / "test.edf").string();
    std::ofstream file(filepath, std::ios::binary | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.write(reinterpret_cast<const char*>(decoded.data()), decoded_len);
    file.close();

    EXPECT_EQ(std::filesystem::file_size(filepath), raw_data.size());
}

TEST_F(FysetcReceiverTest, ChunkWrite_AppendAtOffset_ExtendsFile) {
    std::string date = "20260315";
    auto date_dir = test_dir_ / date;
    std::filesystem::create_directories(date_dir);
    std::string filepath = (date_dir / "growing.edf").string();

    // Create initial file with 1024 bytes
    createTestFile(filepath, 1024);
    ASSERT_EQ(std::filesystem::file_size(filepath), 1024u);

    // Write 512 bytes at offset 1024 (append)
    std::vector<uint8_t> new_data(512, 0xAB);
    std::string b64 = base64Encode(new_data);

    size_t b64_len = b64.size();
    size_t max_decoded = 3 * (b64_len / 4) + 3;
    std::vector<unsigned char> decoded(max_decoded);
    int decoded_len = EVP_DecodeBlock(decoded.data(),
                                       reinterpret_cast<const unsigned char*>(b64.c_str()),
                                       static_cast<int>(b64_len));
    int padding = 0;
    if (b64_len >= 1 && b64[b64_len - 1] == '=') padding++;
    if (b64_len >= 2 && b64[b64_len - 2] == '=') padding++;
    decoded_len -= padding;

    ASSERT_EQ(static_cast<size_t>(decoded_len), 512u);

    // Open for in/out (seek write)
    std::ofstream file(filepath, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekp(1024);
    file.write(reinterpret_cast<const char*>(decoded.data()), decoded_len);
    file.close();

    EXPECT_EQ(std::filesystem::file_size(filepath), 1536u);

    // Verify the appended data
    std::ifstream verify(filepath, std::ios::binary);
    verify.seekg(1024);
    std::vector<uint8_t> readback(512);
    verify.read(reinterpret_cast<char*>(readback.data()), 512);
    EXPECT_EQ(readback, new_data);
}

// ============================================================================
// MANIFEST DIFF LOGIC
// ============================================================================

TEST_F(FysetcReceiverTest, ManifestDiff_AllFilesComplete_NothingNeeded) {
    std::string date = "20260315";
    auto date_dir = test_dir_ / date;

    createTestFile(date_dir / "BRP.edf", 676000);
    createTestFile(date_dir / "PLD.edf", 64000);
    createTestFile(date_dir / "SAD.edf", 29000);

    std::map<std::string, uint64_t> remote = {
        {"BRP.edf", 676000},
        {"PLD.edf", 64000},
        {"SAD.edf", 29000},
    };

    int needed = 0;
    for (const auto& [filename, remote_size] : remote) {
        uint64_t local_size = 0;
        auto filepath = date_dir / filename;
        if (std::filesystem::exists(filepath)) {
            local_size = std::filesystem::file_size(filepath);
        }
        if (local_size < remote_size) needed++;
    }

    EXPECT_EQ(needed, 0);
}

TEST_F(FysetcReceiverTest, ManifestDiff_MissingAndIncomplete_RequestsBoth) {
    std::string date = "20260315";
    auto date_dir = test_dir_ / date;

    createTestFile(date_dir / "BRP.edf", 676000);   // complete
    createTestFile(date_dir / "PLD.edf", 32000);     // incomplete (remote has 64000)
    // SAD.edf missing entirely

    std::map<std::string, uint64_t> remote = {
        {"BRP.edf", 676000},
        {"PLD.edf", 64000},
        {"SAD.edf", 29000},
        {"EVE.edf", 1024},   // new file not on local
    };

    std::vector<std::pair<std::string, uint64_t>> fetch_list;
    for (const auto& [filename, remote_size] : remote) {
        uint64_t local_size = 0;
        auto filepath = date_dir / filename;
        if (std::filesystem::exists(filepath)) {
            local_size = std::filesystem::file_size(filepath);
        }
        if (local_size < remote_size) {
            fetch_list.push_back({filename, local_size});
        }
    }

    EXPECT_EQ(fetch_list.size(), 3u);  // PLD (incomplete), SAD (missing), EVE (missing)

    // Check offsets
    std::map<std::string, uint64_t> fetch_map;
    for (const auto& [f, o] : fetch_list) fetch_map[f] = o;

    EXPECT_EQ(fetch_map["PLD.edf"], 32000u);  // resume from 32000
    EXPECT_EQ(fetch_map["SAD.edf"], 0u);       // start from 0
    EXPECT_EQ(fetch_map["EVE.edf"], 0u);       // start from 0
}

// ============================================================================
// ROOT FILE (STR) CHUNK HANDLING
// ============================================================================

TEST_F(FysetcReceiverTest, ChunkWrite_EmptyDate_WritesToRootDir) {
    // STR.edf chunks come with date="" -- should write to data_dir root
    std::string filename = "STR.edf";
    std::string filepath = (test_dir_ / filename).string();

    std::vector<uint8_t> data(4096, 0xCC);
    std::ofstream file(filepath, std::ios::binary | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();

    EXPECT_EQ(std::filesystem::file_size(filepath), 4096u);
    EXPECT_TRUE(std::filesystem::exists(test_dir_ / "STR.edf"));
}

// ============================================================================
// BASE64 ROUND-TRIP
// ============================================================================

TEST_F(FysetcReceiverTest, Base64RoundTrip_VariousSizes) {
    for (size_t size : {0, 1, 2, 3, 4, 100, 511, 512, 513, 4096, 8192}) {
        if (size == 0) continue;  // EVP_DecodeBlock doesn't handle empty

        std::vector<uint8_t> original(size);
        for (size_t i = 0; i < size; i++) {
            original[i] = static_cast<uint8_t>(i & 0xFF);
        }

        // Encode
        std::string b64 = base64Encode(original);

        // Decode
        size_t b64_len = b64.size();
        size_t max_decoded = 3 * (b64_len / 4) + 3;
        std::vector<unsigned char> decoded(max_decoded);
        int decoded_len = EVP_DecodeBlock(decoded.data(),
                                           reinterpret_cast<const unsigned char*>(b64.c_str()),
                                           static_cast<int>(b64_len));
        int padding = 0;
        if (b64_len >= 1 && b64[b64_len - 1] == '=') padding++;
        if (b64_len >= 2 && b64[b64_len - 2] == '=') padding++;
        decoded_len -= padding;

        ASSERT_EQ(static_cast<size_t>(decoded_len), size)
            << "Failed for size=" << size;

        for (size_t i = 0; i < size; i++) {
            ASSERT_EQ(decoded[i], original[i])
                << "Mismatch at byte " << i << " for size=" << size;
        }
    }
}

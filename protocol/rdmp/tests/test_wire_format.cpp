// test_wire_format.cpp – Unit tests for the RDMP UDP wire-format
//
// These tests validate the binary serialisation/deserialisation logic by
// exercising the same byte layout that RDMPClient::sendMulticast() produces
// and RDMPServer::receiveAndProcess() consumes.

#include <gtest/gtest.h>

#include "rdmp_common.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Wire-format helpers (mirrors rdmp_client.cpp logic)
// ---------------------------------------------------------------------------

std::vector<uint8_t> buildDatagram(const std::string& uuid,
                                   rdmp::MsgType       type,
                                   const std::string&  payload) {
    const uint32_t magic   = htonl(rdmp::RDMP_MAGIC);
    const uint32_t pay_len = htonl(static_cast<uint32_t>(payload.size()));

    std::vector<uint8_t> buf;
    buf.reserve(rdmp::RDMP_HEADER_SIZE + payload.size());

    // Magic (4 B)
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&magic),
               reinterpret_cast<const uint8_t*>(&magic) + 4);
    // Version (1 B)
    buf.push_back(rdmp::RDMP_VERSION);
    // Message type (1 B)
    buf.push_back(static_cast<uint8_t>(type));
    // UUID (36 B)
    buf.insert(buf.end(), uuid.begin(), uuid.end());
    // Payload length (4 B)
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&pay_len),
               reinterpret_cast<const uint8_t*>(&pay_len) + 4);
    // Payload (N B)
    buf.insert(buf.end(), payload.begin(), payload.end());

    return buf;
}

// Parse the datagram back into its fields (mirrors receiveAndProcess).
struct ParsedDatagram {
    bool        valid       = false;
    uint8_t     version     = 0;
    rdmp::MsgType msg_type  = rdmp::MsgType::HEARTBEAT;
    std::string uuid;
    std::string payload;
};

ParsedDatagram parseDatagram(const std::vector<uint8_t>& buf) {
    ParsedDatagram d;
    if (buf.size() < rdmp::RDMP_HEADER_SIZE) return d;

    uint32_t magic = 0;
    memcpy(&magic, buf.data(), 4);
    if (ntohl(magic) != rdmp::RDMP_MAGIC) return d;

    d.version  = buf[4];
    d.msg_type = static_cast<rdmp::MsgType>(buf[5]);

    char uuid_buf[37] = {};
    memcpy(uuid_buf, buf.data() + 6, 36);
    d.uuid = uuid_buf;

    uint32_t pay_len = 0;
    memcpy(&pay_len, buf.data() + 42, 4);
    pay_len = ntohl(pay_len);

    if (buf.size() < rdmp::RDMP_HEADER_SIZE + pay_len) return d;
    d.payload.assign(reinterpret_cast<const char*>(buf.data() + rdmp::RDMP_HEADER_SIZE),
                     pay_len);
    d.valid = true;
    return d;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(WireFormatTest, HeaderSizeConstant) {
    // Documented: 4+1+1+36+4 = 46
    EXPECT_EQ(rdmp::RDMP_HEADER_SIZE, 46u);
}

TEST(WireFormatTest, MagicConstant) {
    // 'R','D','M','P'
    EXPECT_EQ(rdmp::RDMP_MAGIC, 0x52444D50u);
}

TEST(WireFormatTest, TaskAnnounceRoundTrip) {
    const std::string uuid    = rdmp::generateUUID();
    const std::string payload = "scale-out node-7";

    auto buf = buildDatagram(uuid, rdmp::MsgType::TASK_ANNOUNCE, payload);
    ASSERT_EQ(buf.size(), rdmp::RDMP_HEADER_SIZE + payload.size());

    auto d = parseDatagram(buf);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.version,  rdmp::RDMP_VERSION);
    EXPECT_EQ(d.msg_type, rdmp::MsgType::TASK_ANNOUNCE);
    EXPECT_EQ(d.uuid,     uuid);
    EXPECT_EQ(d.payload,  payload);
}

TEST(WireFormatTest, HeartbeatRoundTrip) {
    const std::string uuid    = rdmp::generateUUID();
    const std::string payload = "";

    auto buf = buildDatagram(uuid, rdmp::MsgType::HEARTBEAT, payload);
    auto d   = parseDatagram(buf);

    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.msg_type, rdmp::MsgType::HEARTBEAT);
    EXPECT_EQ(d.payload,  "");
}

TEST(WireFormatTest, EmptyPayload) {
    const std::string uuid = rdmp::generateUUID();
    auto buf = buildDatagram(uuid, rdmp::MsgType::TASK_ANNOUNCE, "");
    EXPECT_EQ(buf.size(), rdmp::RDMP_HEADER_SIZE);

    auto d = parseDatagram(buf);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.payload, "");
}

TEST(WireFormatTest, LargePayload) {
    const std::string uuid    = rdmp::generateUUID();
    const std::string payload(4096, 'z');

    auto buf = buildDatagram(uuid, rdmp::MsgType::TASK_ANNOUNCE, payload);
    auto d   = parseDatagram(buf);

    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.payload, payload);
}

TEST(WireFormatTest, TruncatedDatagram) {
    // Shorter than RDMP_HEADER_SIZE
    std::vector<uint8_t> short_buf(20, 0);
    auto d = parseDatagram(short_buf);
    EXPECT_FALSE(d.valid);
}

TEST(WireFormatTest, WrongMagic) {
    const std::string uuid = rdmp::generateUUID();
    auto buf = buildDatagram(uuid, rdmp::MsgType::TASK_ANNOUNCE, "test");
    // Corrupt the magic number
    buf[0] = 0xFF;
    auto d = parseDatagram(buf);
    EXPECT_FALSE(d.valid);
}

TEST(WireFormatTest, PayloadLengthOverflow) {
    const std::string uuid = rdmp::generateUUID();
    auto buf = buildDatagram(uuid, rdmp::MsgType::TASK_ANNOUNCE, "x");
    // Set payload_len to something larger than the buffer
    uint32_t big = htonl(99999);
    memcpy(buf.data() + 42, &big, 4);
    auto d = parseDatagram(buf);
    EXPECT_FALSE(d.valid);
}

} // namespace

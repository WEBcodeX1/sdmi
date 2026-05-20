// test_wire_format.cpp – Unit tests for the RMDP UDP wire-format
//
// These tests validate the binary serialisation/deserialisation logic by
// exercising the same byte layout that RMDPClient::sendMulticast() produces
// and RMDPServer::receiveAndProcess() consumes.

#include <boost/test/unit_test.hpp>

#include "rmdp_common.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Wire-format helpers (mirrors rmdp_client.cpp logic)
// ---------------------------------------------------------------------------

std::vector<uint8_t> buildDatagram(const std::string& uuid,
                                   rmdp::MsgType       type,
                                   const std::string&  payload) {
    const uint32_t magic   = htonl(rmdp::RMDP_MAGIC);
    const uint32_t pay_len = htonl(static_cast<uint32_t>(payload.size()));

    std::vector<uint8_t> buf;
    buf.reserve(rmdp::RMDP_HEADER_SIZE + payload.size() + rmdp::RMDP_FOOTER_SIZE);

    // Magic (4 B)
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&magic),
               reinterpret_cast<const uint8_t*>(&magic) + 4);
    // Frame start marker (1 B)
    buf.push_back(rmdp::RMDP_FRAME_START);
    // Version (1 B)
    buf.push_back(rmdp::RMDP_VERSION);
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
    // End-of-frame footer: magic (4 B) + frame end marker (1 B)
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t*>(&magic),
               reinterpret_cast<const uint8_t*>(&magic) + 4);
    buf.push_back(rmdp::RMDP_FRAME_END);

    return buf;
}

// Parse the datagram back into its fields (mirrors receiveAndProcess).
struct ParsedDatagram {
    bool        valid       = false;
    uint8_t     version     = 0;
    rmdp::MsgType msg_type  = rmdp::MsgType::TASK_ANNOUNCE;
    std::string uuid;
    std::string payload;
};

ParsedDatagram parseDatagram(const std::vector<uint8_t>& buf) {
    ParsedDatagram d;
    if (buf.size() < rmdp::RMDP_HEADER_SIZE + rmdp::RMDP_FOOTER_SIZE) return d;

    uint32_t magic = 0;
    memcpy(&magic, buf.data(), 4);
    if (ntohl(magic) != rmdp::RMDP_MAGIC) return d;

    // Frame start marker
    if (buf[4] != rmdp::RMDP_FRAME_START) return d;

    d.version  = buf[5];
    d.msg_type = static_cast<rmdp::MsgType>(buf[6]);

    char uuid_buf[37] = {};
    memcpy(uuid_buf, buf.data() + 7, 36);
    d.uuid = uuid_buf;

    uint32_t pay_len = 0;
    memcpy(&pay_len, buf.data() + 43, 4);
    pay_len = ntohl(pay_len);

    if (buf.size() < rmdp::RMDP_HEADER_SIZE + pay_len + rmdp::RMDP_FOOTER_SIZE) return d;

    // Frame end marker: magic + RMDP_FRAME_END
    const size_t footer_offset = rmdp::RMDP_HEADER_SIZE + pay_len;
    uint32_t end_magic = 0;
    memcpy(&end_magic, buf.data() + footer_offset, 4);
    if (ntohl(end_magic) != rmdp::RMDP_MAGIC) return d;
    if (buf[footer_offset + 4] != rmdp::RMDP_FRAME_END) return d;

    d.payload.assign(reinterpret_cast<const char*>(buf.data() + rmdp::RMDP_HEADER_SIZE),
                     pay_len);
    d.valid = true;
    return d;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(WireFormatTest)

BOOST_AUTO_TEST_CASE(HeaderSizeConstant) {
    // Documented: 4+1+1+1+36+4 = 47
    BOOST_CHECK_EQUAL(rmdp::RMDP_HEADER_SIZE, 47u);
}

BOOST_AUTO_TEST_CASE(FooterSizeConstant) {
    // Documented: 4 (magic) + 1 (frame_end) = 5
    BOOST_CHECK_EQUAL(rmdp::RMDP_FOOTER_SIZE, 5u);
}

BOOST_AUTO_TEST_CASE(MagicConstant) {
    // 'R','M','D','P'
    BOOST_CHECK_EQUAL(rmdp::RMDP_MAGIC, 0x524D4450u);
}

BOOST_AUTO_TEST_CASE(FrameMarkerConstants) {
    BOOST_CHECK_EQUAL(rmdp::RMDP_FRAME_START, 0x01u);
    BOOST_CHECK_EQUAL(rmdp::RMDP_FRAME_END,   0x02u);
}

BOOST_AUTO_TEST_CASE(TaskAnnounceRoundTrip) {
    const std::string uuid    = rmdp::generateUUID();
    const std::string payload = "scale-out node-7";

    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, payload);
    BOOST_REQUIRE_EQUAL(buf.size(),
                        rmdp::RMDP_HEADER_SIZE + payload.size() + rmdp::RMDP_FOOTER_SIZE);

    auto d = parseDatagram(buf);
    BOOST_CHECK(d.valid);
    BOOST_CHECK_EQUAL(d.version,  rmdp::RMDP_VERSION);
    BOOST_CHECK_EQUAL(d.msg_type, rmdp::MsgType::TASK_ANNOUNCE);
    BOOST_CHECK_EQUAL(d.uuid,     uuid);
    BOOST_CHECK_EQUAL(d.payload,  payload);
}

BOOST_AUTO_TEST_CASE(EmptyPayload) {
    const std::string uuid = rmdp::generateUUID();
    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, "");
    BOOST_CHECK_EQUAL(buf.size(), rmdp::RMDP_HEADER_SIZE + rmdp::RMDP_FOOTER_SIZE);

    auto d = parseDatagram(buf);
    BOOST_CHECK(d.valid);
    BOOST_CHECK_EQUAL(d.payload, "");
}

BOOST_AUTO_TEST_CASE(LargePayload) {
    const std::string uuid    = rmdp::generateUUID();
    const std::string payload(4096, 'z');

    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, payload);
    auto d   = parseDatagram(buf);

    BOOST_CHECK(d.valid);
    BOOST_CHECK_EQUAL(d.payload, payload);
}

BOOST_AUTO_TEST_CASE(TruncatedDatagram) {
    // Shorter than RMDP_HEADER_SIZE
    std::vector<uint8_t> short_buf(20, 0);
    auto d = parseDatagram(short_buf);
    BOOST_CHECK(!d.valid);
}

BOOST_AUTO_TEST_CASE(WrongMagic) {
    const std::string uuid = rmdp::generateUUID();
    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, "test");
    // Corrupt the magic number
    buf[0] = 0xFF;
    auto d = parseDatagram(buf);
    BOOST_CHECK(!d.valid);
}

BOOST_AUTO_TEST_CASE(PayloadLengthOverflow) {
    const std::string uuid = rmdp::generateUUID();
    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, "x");
    // Set payload_len to something larger than the buffer
    uint32_t big = htonl(99999);
    memcpy(buf.data() + 43, &big, 4);
    auto d = parseDatagram(buf);
    BOOST_CHECK(!d.valid);
}

BOOST_AUTO_TEST_CASE(MissingEndMarker) {
    // A datagram without the end-of-frame footer must be rejected.
    const std::string uuid = rmdp::generateUUID();
    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, "hello");
    // Strip the 5-byte footer
    buf.resize(buf.size() - rmdp::RMDP_FOOTER_SIZE);
    auto d = parseDatagram(buf);
    BOOST_CHECK(!d.valid);
}

BOOST_AUTO_TEST_CASE(WrongEndMarkerByte) {
    // A datagram with a corrupted end-of-frame byte (not 0x02) must be rejected.
    const std::string uuid = rmdp::generateUUID();
    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, "hello");
    // Overwrite the last byte (frame_end marker)
    buf.back() = 0xFF;
    auto d = parseDatagram(buf);
    BOOST_CHECK(!d.valid);
}

BOOST_AUTO_TEST_CASE(WrongStartMarkerByte) {
    // A datagram with a corrupted start-of-frame byte (not 0x01) must be rejected.
    const std::string uuid = rmdp::generateUUID();
    auto buf = buildDatagram(uuid, rmdp::MsgType::TASK_ANNOUNCE, "hello");
    // Byte 4 is the frame start marker
    buf[4] = 0xFF;
    auto d = parseDatagram(buf);
    BOOST_CHECK(!d.valid);
}

BOOST_AUTO_TEST_SUITE_END()


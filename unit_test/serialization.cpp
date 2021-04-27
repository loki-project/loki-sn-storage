#include "serialization.h"
#include "service_node.h"

#include <boost/test/unit_test.hpp>

#include <string>

using namespace oxen;

BOOST_AUTO_TEST_SUITE(serialization)

BOOST_AUTO_TEST_CASE(it_serializes_and_deserializes) {

    const auto pub_key =
        "054368520005786b249bcd461d28f75e560ea794014eeb17fcf6003f37d876783e"s;
    const auto data = "data";
    const auto hash = "hash";
    const uint64_t timestamp = 12345678;
    const uint64_t ttl = 3456000;
    message_t msg{pub_key, data, hash, ttl, timestamp};
    std::string msg_serialized;
    serialize_message(msg_serialized, msg);
    const auto expected_serialized = oxenmq::to_hex(pub_key) +
        "040000000000000068617368" // size+hash
        "040000000000000064617461" // size+data
        "00bc340000000000" // ttl
        "4e61bc0000000000" // timestamp
        "0000000000000000"s; // nonce
    BOOST_CHECK_EQUAL(oxenmq::to_hex(msg_serialized), expected_serialized);
    const std::vector<message_t> inputs{msg, msg};
    const std::vector<std::string> batches = serialize_messages(inputs);
    BOOST_CHECK_EQUAL(batches.size(), 1);
    BOOST_CHECK_EQUAL(oxenmq::to_hex(batches[0]), expected_serialized + expected_serialized);

    const auto messages = deserialize_messages(batches[0]);
    BOOST_CHECK_EQUAL(messages.size(), 2);
    for (int i = 0; i < messages.size(); ++i) {
        BOOST_CHECK_EQUAL(messages[i].pub_key, pub_key);
        BOOST_CHECK_EQUAL(messages[i].data, data);
        BOOST_CHECK_EQUAL(messages[i].hash, hash);
        BOOST_CHECK_EQUAL(messages[i].timestamp, timestamp);
        BOOST_CHECK_EQUAL(messages[i].ttl, ttl);
    }
}

BOOST_AUTO_TEST_CASE(it_serialises_in_batches) {
    const auto pub_key =
        "054368520005786b249bcd461d28f75e560ea794014eeb17fcf6003f37d876783e";
    const auto data = "data";
    const auto hash = "hash";
    const uint64_t timestamp = 12345678;
    const uint64_t ttl = 3456000;
    message_t msg{pub_key, data, hash, ttl, timestamp};
    std::string buffer;
    serialize_message(buffer, msg);
    const size_t num_messages = (500000 / buffer.size()) + 10;
    std::vector<message_t> inputs;
    for (int i = 0; i < num_messages; ++i)
        inputs.push_back(msg);
    const std::vector<std::string> batches = serialize_messages(inputs);
    BOOST_CHECK_EQUAL(batches.size(), 2);
}
BOOST_AUTO_TEST_SUITE_END()

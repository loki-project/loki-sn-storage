#include "pow.hpp"
#include "utils.hpp"

#include <boost/test/unit_test.hpp>
#include <chrono>

using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(pow_unit_test)

namespace valid_pow {
constexpr auto pubkey =
    "054368520005786b249bcd461d28f75e560ea794014eeb17fcf6003f37d876783e";
constexpr auto ttl = "345600";
constexpr auto nonce = "AAAAAAAaFio=";
constexpr auto timestamp = "1554859211";
constexpr auto data =
    "CAESvgMKA1BVVBIPL2FwaS92MS9tZXNzYWdlGqIDCGUSQjA1NDM2ODUyMDAwNTc4NmIyNDliY2"
    "Q0NjFkMjhmNzVlNTYwZWE3OTQwMTRlZWIxN2ZjZjYwMDNmMzdkODc2NzgzNDgBKID00qagLULQ"
    "AksmCNIm0urN9g/"
    "z2jBn0iLuaDPrrZnsLeRj9MD4eVDG0qTOD2Hg84LWswV3YAzb2Xku7LyTaitU3lhRAyDZRByjS"
    "Z2nWphSnzrkFaKhfKrHuzzDNDSGZsK9B1xM+lZJ/cR05Nn8fshfbW1CdINfNIiSK1qW2d/"
    "mfe3DI/"
    "CqShvA8D0r7LdbSq0tGH7JUIESu1OqJNePc46kLS0sMQ24YOitPtXSxUxbHrdBalydOqpYNGWG"
    "l4dtzOMr35Z0vCz/u25ROajbcj1D7hf40dyIv9RT/"
    "wwo52CiwtbrFdXc84K0yDKTnPu6QoTKoiVt+hftgKv0o/"
    "ruRN40fLSGl0KD+FqLf4Oqe5u1MaDDoUlLW1tdw6gsPSUxgQPWkJ6qda3t+"
    "IioolNQWidLcKg4WXvSwWRO6bDLNXfyDLd9UwcpHRus5UYnmc9BTjiXHQP8pWc4ciCMAQ==";
const std::vector<pow_difficulty_t> history{
    pow_difficulty_t{std::chrono::milliseconds(1554559211), 10}};
} // namespace valid_pow

BOOST_AUTO_TEST_CASE(util_parses_a_valid_ttl) {
    uint64_t ttl;
    BOOST_CHECK_EQUAL(util::parseTTL("0", ttl), false);

    BOOST_CHECK_EQUAL(util::parseTTL("10000", ttl), true);
    BOOST_CHECK_EQUAL(ttl, 10000);

    BOOST_CHECK_EQUAL(util::parseTTL("1000000", ttl), true);
    BOOST_CHECK_EQUAL(ttl, 1000000);
    // Maximum time to live of 4 days
    BOOST_CHECK_EQUAL(util::parseTTL("345600000", ttl), true);
    BOOST_CHECK_EQUAL(ttl, 345600000);

    BOOST_CHECK_EQUAL(util::parseTTL("3456010000", ttl), false);
    BOOST_CHECK_EQUAL(util::parseTTL("-1000", ttl), false);
    BOOST_CHECK_EQUAL(util::parseTTL("abcvs", ttl), false);
    BOOST_CHECK_EQUAL(util::parseTTL("", ttl), false);
}

BOOST_AUTO_TEST_CASE(it_checks_a_valid_pow) {
    using namespace valid_pow;
    std::string messageHash;
    BOOST_CHECK_EQUAL(
        checkPoW(nonce, timestamp, ttl, pubkey, data, messageHash, history),
        true);
}

BOOST_AUTO_TEST_CASE(it_checks_an_invalid_nonce) {
    using namespace valid_pow;
    std::string messageHash;
    BOOST_CHECK_EQUAL(checkPoW("AAAAAAABBCF=", timestamp, ttl, pubkey, data,
                                messageHash, history),
                      false);
}

BOOST_AUTO_TEST_CASE(it_checks_an_invalid_timestamp) {
    using namespace valid_pow;
    std::string messageHash;
    BOOST_CHECK_EQUAL(
        checkPoW(nonce, "1549252653", ttl, pubkey, data, messageHash, history),
        false);
}

BOOST_AUTO_TEST_CASE(it_checks_an_invalid_ttl) {
    using namespace valid_pow;
    std::string messageHash;
    BOOST_CHECK_EQUAL(checkPoW(nonce, timestamp, "345601", pubkey, data,
                                messageHash, history),
                      false);
}

BOOST_AUTO_TEST_CASE(it_checks_an_invalid_pubkey) {
    using namespace valid_pow;
    std::string messageHash;
    BOOST_CHECK_EQUAL(
        checkPoW(nonce, timestamp, ttl,
                  "05d5970e75efb8e8daccd4d07f5f59e744c3aea25cec8bf"
                  "a3e43674c4a55875f4c",
                  data, messageHash, history),
        false);
}

BOOST_AUTO_TEST_CASE(it_checks_an_invalid_data) {
    using namespace valid_pow;
    const auto wrong_data =
        "CAESvgMKA1BVVBIPL2FwaS92MS9tZXNzYWdlGqIDCGUSQjA1ZDU5NzBlNzVlZmI4ZThkYW"
        "NjZDRkMDdmNWY1OWU3NDRjM2FlYTI1Y2VjOGJmYTNlNDM2NzRjNGE1NTg3NWY0ZDgBKNPR"
        "nbWLLULQAvG1sbxpwQY7xXBBWLvYDDtHNBpHtxAMim7+"
        "iSqYYfWvwrobXbUSMP55nAiIUr6iJtvM4OzQoZSV/"
        "zCz5tN9T5tKAvkiVkTyAXva7Re8BO8HX3ra+zPWDsXYw12w9XA4cxY95Y/"
        "6agyMjNDAhj2bhUCMiZNd8dpl5VvKxwFKdxP4zVruKRdAaJy1/xB1gCfZ/hkh2xX90n/"
        "4p4SlPj/XGmjSQ73h6PWm5/"
        "qb2tPNmpkb6uuPD3GpZzxxf4pyMETwhhyruJ6KLxV0eUYh5haHxgjJbs+"
        "OcwjoNpGOEhDCgThcQait2Iyb4ahMWVSjt9vxrfL/7I3HbFtMT+En1J7RutRkWX6YvGHN/"
        "gJApKpfGatKZAwdnUeZy+EUTuZnRzvSLOwOL6HsOFvuq4k3gQ5v5+"
        "ZPfNxSO9T6JvrGzRxofo7edadxn/hqi6dkHU7koHNAjSD2AP==";
    std::string messageHash;
    BOOST_CHECK_EQUAL(checkPoW(nonce, timestamp, ttl, pubkey, wrong_data,
                                messageHash, history),
                      false);
}

BOOST_AUTO_TEST_CASE(it_checks_most_recent_difficulty) {
    using namespace valid_pow;
    std::string messageHash;
    const auto t = std::chrono::milliseconds(1554859211);
    const auto t1 = t - 30min;
    const auto t2 = t - 20min;

    const std::vector<pow_difficulty_t> valid_history{
        pow_difficulty_t{t1, 1000}, pow_difficulty_t{t2, 10}};
    BOOST_CHECK_EQUAL(checkPoW(nonce, timestamp, ttl, pubkey, data,
                                messageHash, valid_history),
                      true);

    const std::vector<pow_difficulty_t> invalid_history{
        pow_difficulty_t{t2, 100000}, pow_difficulty_t{t1, 10}};
    BOOST_CHECK_EQUAL(checkPoW(nonce, timestamp, ttl, pubkey, data,
                                messageHash, invalid_history),
                      false);
}

BOOST_AUTO_TEST_CASE(it_checks_difficulty_in_valid_range) {
    using namespace valid_pow;
    std::string messageHash;
    const auto t = std::chrono::milliseconds(1554859211);
    const auto t1 = t - 30min;
    const auto t2 = t + 5min;

    const std::vector<pow_difficulty_t> history1{pow_difficulty_t{t1, 1000},
                                                 pow_difficulty_t{t2, 10}};
    BOOST_CHECK_EQUAL(
        checkPoW(nonce, timestamp, ttl, pubkey, data, messageHash, history1),
        true);

    const auto t3 = t - 5min;
    const auto t4 = t - 8min;
    const std::vector<pow_difficulty_t> history2{pow_difficulty_t{t3, 1000},
                                                 pow_difficulty_t{t4, 10}};
    BOOST_CHECK_EQUAL(
        checkPoW(nonce, timestamp, ttl, pubkey, data, messageHash, history2),
        true);
}

BOOST_AUTO_TEST_SUITE_END()

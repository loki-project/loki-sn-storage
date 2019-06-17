#include "../external/json.hpp"
#include "stats.h"
#include <algorithm>
#include <iostream>
#include <chrono>

namespace loki {

void to_json(nlohmann::json& j, const test_result_t& val) {
    j["timestamp"] = time(nullptr);
    j["success"] = val.success;
}

std::string all_stats_t::to_json(bool pretty) const {

    nlohmann::json json;

    json["client_store_requests"] = client_store_requests;
    json["client_retrieve_requests"] = client_retrieve_requests;
    json["reset_time"] = reset_time;

    nlohmann::json peers;

    for (const auto& kv : peer_report_) {
        const auto& pubkey = kv.first.address;

        peers[pubkey]["requests_failed"] = kv.second.requests_failed;
        peers[pubkey]["pushes_failed"] = kv.second.requests_failed;
        peers[pubkey]["storage_tests"] = kv.second.storage_tests;
        peers[pubkey]["blockchain_tests"] = kv.second.blockchain_tests;
    }

    json["peers"] = peers;
    const int indent = pretty ? 4 : 0;
    return json.dump(indent);
}

static void cleanup_old(std::deque<test_result_t>& tests, time_t cutoff_time) {

    const auto it = std::find_if(tests.begin(), tests.end(),
                                 [cutoff_time](const test_result_t& res) {
                                     return res.timestamp > cutoff_time;
                                 });

    tests.erase(tests.begin(), it);
}

static constexpr auto ROLLING_WINDOW_SIZE = std::chrono::minutes(60);

void all_stats_t::cleanup() {

    using std::chrono::duration_cast;
    using std::chrono::seconds;

    const auto cutoff =
        reset_time - duration_cast<seconds>(ROLLING_WINDOW_SIZE).count();

    for (auto& kv : peer_report_) {

        const sn_record_t& sn = kv.first;

        cleanup_old(peer_report_[sn].storage_tests, cutoff);
        cleanup_old(peer_report_[sn].blockchain_tests, cutoff);
    }
}

} // namespace loki

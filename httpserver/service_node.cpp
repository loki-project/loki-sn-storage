#include "service_node.h"

#include "Database.hpp"
#include "Item.hpp"
#include "http_connection.h"
#include "https_client.h"
#include "loki_logger.h"
#include "lokid_key.h"
#include "serialization.h"
#include "signature.h"
#include "utils.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <resolv.h>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/bind.hpp>

using json = nlohmann::json;
using loki::storage::Item;
using namespace std::chrono_literals;

namespace loki {
using http_server::connection_t;

constexpr std::array<std::chrono::seconds, 8> RETRY_INTERVALS = {
    std::chrono::seconds(1),   std::chrono::seconds(5),
    std::chrono::seconds(10),  std::chrono::seconds(20),
    std::chrono::seconds(40),  std::chrono::seconds(80),
    std::chrono::seconds(160), std::chrono::seconds(320)};

static void make_sn_request(boost::asio::io_context& ioc, const sn_record_t& sn,
                            const std::shared_ptr<request_t>& req,
                            http_callback_t&& cb) {
    // TODO: Return to using snode address instead of ip
    return make_https_request(ioc, sn.ip(), sn.port(), sn.pub_key(), req,
                              std::move(cb));
}

std::vector<pow_difficulty_t> query_pow_difficulty(std::error_code& ec) {
    std::vector<pow_difficulty_t> new_history;
    int response;
    unsigned char query_buffer[1024] = {};
    response = res_query(POW_DIFFICULTY_URL, ns_c_in, ns_t_txt, query_buffer,
                         sizeof(query_buffer));
    int pow_difficulty;
    ns_msg nsMsg;
    if (ns_initparse(query_buffer, response, &nsMsg) == -1) {
        ec = std::make_error_code(std::errc::bad_message);
        return new_history;
    }
    ns_rr rr;
    if (ns_parserr(&nsMsg, ns_s_an, 0, &rr) == -1) {
        ec = std::make_error_code(std::errc::bad_message);
        return new_history;
    }

    try {
        const json history = json::parse(ns_rr_rdata(rr) + 1, nullptr, true);
        new_history.reserve(history.size());
        for (const auto& el : history.items()) {
            const std::chrono::milliseconds timestamp(std::stoul(el.key()));
            const int difficulty = el.value().get<int>();
            new_history.push_back(pow_difficulty_t{timestamp, difficulty});
        }
        return new_history;
    } catch (...) {
        ec = std::make_error_code(std::errc::bad_message);
        return new_history;
    }
}

FailedRequestHandler::FailedRequestHandler(
    boost::asio::io_context& ioc, const sn_record_t& sn,
    std::shared_ptr<request_t> req,
    boost::optional<std::function<void()>>&& give_up_cb)
    : ioc_(ioc), retry_timer_(ioc), sn_(sn), request_(std::move(req)),
      give_up_callback_(std::move(give_up_cb)) {}

void FailedRequestHandler::retry(std::shared_ptr<FailedRequestHandler>&& self) {

    attempt_count_ += 1;
    if (attempt_count_ > RETRY_INTERVALS.size()) {
        LOKI_LOG(debug, "Gave up after {} attempts", attempt_count_);
        if (give_up_callback_)
            (*give_up_callback_)();
        return;
    }

    retry_timer_.expires_after(RETRY_INTERVALS[attempt_count_ - 1]);
    LOKI_LOG(debug, "Will retry in {} secs",
             RETRY_INTERVALS[attempt_count_ - 1].count());

    retry_timer_.async_wait(
        [self = std::move(self)](const boost::system::error_code& ec) mutable {
            /// Save some references before possibly moved out of `self`
            const auto& sn = self->sn_;
            auto& ioc = self->ioc_;
            /// TODO: investigate whether we can get rid of the extra ptr copy
            /// here?
            const std::shared_ptr<request_t> req = self->request_;

            /// Request will be copied here
            make_sn_request(
                ioc, sn, req,
                [self = std::move(self)](sn_response_t&& res) mutable {
                    if (res.error_code != SNodeError::NO_ERROR) {
                        LOKI_LOG(error, "Could not relay one: {} (attempt #{})",
                                 self->sn_, self->attempt_count_);
                        self->retry(std::move(self));
                    }
                });
        });
}

FailedRequestHandler::~FailedRequestHandler() {
    LOKI_LOG(trace, "~FailedRequestHandler()");
}

void FailedRequestHandler::init_timer() { retry(shared_from_this()); }

/// TODO: there should be config.h to store constants like these
#ifdef INTEGRATION_TEST
constexpr std::chrono::milliseconds SWARM_UPDATE_INTERVAL = 200ms;
#else
constexpr std::chrono::milliseconds SWARM_UPDATE_INTERVAL = 1000ms;
#endif
constexpr std::chrono::seconds CLEANUP_TIMER = 60min;
constexpr std::chrono::minutes LOKID_PING_INTERVAL = 5min;
constexpr std::chrono::minutes POW_DIFFICULTY_UPDATE_INTERVAL = 10min;
constexpr int CLIENT_RETRIEVE_MESSAGE_LIMIT = 10;

static std::shared_ptr<request_t> make_post_request(const char* target,
                                                    std::string&& data) {
    auto req = std::make_shared<request_t>();
    req->body() = std::move(data);
    req->method(http::verb::post);
    req->set(http::field::host, "service node");
    req->target(target);
    req->prepare_payload();
    return req;
}

static std::shared_ptr<request_t> make_push_all_request(std::string&& data) {
    return make_post_request("/swarms/push_batch/v1", std::move(data));
}

static std::shared_ptr<request_t> make_push_request(std::string&& data) {
    return make_post_request("/swarms/push/v1", std::move(data));
}

static bool verify_message(const message_t& msg,
                           const std::vector<pow_difficulty_t> history,
                           const char** error_message = nullptr) {
    if (!util::validateTTL(msg.ttl)) {
        if (error_message)
            *error_message = "Provided TTL is not valid";
        return false;
    }
    if (!util::validateTimestamp(msg.timestamp, msg.ttl)) {
        if (error_message)
            *error_message = "Provided timestamp is not valid";
        return false;
    }
    std::string hash;
#ifndef DISABLE_POW
    const int difficulty =
        get_valid_difficulty(std::to_string(msg.timestamp), history);
    if (!checkPoW(msg.nonce, std::to_string(msg.timestamp),
                  std::to_string(msg.ttl), msg.pub_key, msg.data, hash,
                  difficulty)) {
        if (error_message)
            *error_message = "Provided PoW nonce is not valid";
        return false;
    }
#endif
    if (hash != msg.hash) {
        if (error_message)
            *error_message = "Incorrect hash provided";
        return false;
    }
    return true;
}

ServiceNode::ServiceNode(boost::asio::io_context& ioc,
                         boost::asio::io_context& worker_ioc, uint16_t port,
                         const loki::lokid_key_pair_t& lokid_key_pair,
                         const std::string& db_location,
                         LokidClient& lokid_client, const bool force_start)
    : ioc_(ioc), worker_ioc_(worker_ioc),
      db_(std::make_unique<Database>(ioc, db_location)),
      swarm_update_timer_(ioc), lokid_ping_timer_(ioc),
      stats_cleanup_timer_(ioc), pow_update_timer_(worker_ioc),
      lokid_key_pair_(lokid_key_pair), lokid_client_(lokid_client),
      force_start_(force_start) {

    char buf[64] = {0};
    if (char const* dest =
            util::base32z_encode(lokid_key_pair_.public_key, buf)) {

        std::string addr = dest;
        our_address_.set_address(addr);
    } else {
        throw std::runtime_error("Could not encode our public key");
    }
    // TODO: fail hard if we can't encode our public key
    LOKI_LOG(info, "Read our snode address: {}", our_address_);
    our_address_.set_port(port);

    bootstrap_ips();

    LOKI_LOG(info, "Requesting initial swarm state");
    swarm_timer_tick();
    lokid_ping_timer_tick();
    cleanup_timer_tick();

    worker_thread_ = boost::thread([this]() { worker_ioc_.run(); });
    boost::asio::post(worker_ioc_, [this]() {
        pow_difficulty_timer_tick(std::bind(
            &ServiceNode::set_difficulty_history, this, std::placeholders::_1));
    });
}

static block_update_t
parse_swarm_update(const std::shared_ptr<std::string>& response_body) {

    if (!response_body) {
        LOKI_LOG(error, "Bad lokid rpc response: no response body");
        throw std::runtime_error("Failed to parse swarm update");
    }
    const json body = json::parse(*response_body, nullptr, false);
    if (body.is_discarded()) {
        LOKI_LOG(trace, "Response body: {}", *response_body);
        LOKI_LOG(error, "Bad lokid rpc response: invalid json");
        throw std::runtime_error("Failed to parse swarm update");
    }
    std::map<swarm_id_t, std::vector<sn_record_t>> swarm_map;
    block_update_t bu;

    try {
        const json service_node_states =
            body.at("result").at("service_node_states");

        for (const auto& sn_json : service_node_states) {
            const std::string pubkey =
                sn_json.at("service_node_pubkey").get<std::string>();

            const swarm_id_t swarm_id =
                sn_json.at("swarm_id").get<swarm_id_t>();
            std::string snode_address = util::hex64_to_base32z(pubkey);

            const uint16_t port = sn_json.at("storage_port").get<uint16_t>();
            const std::string snode_ip =
                sn_json.at("public_ip").get<std::string>();
            const sn_record_t sn{port, std::move(snode_address),
                                 std::move(snode_ip)};

            swarm_map[swarm_id].push_back(sn);
        }

        bu.height = body.at("result").at("height").get<uint64_t>();
        bu.target_height =
            body.at("result").at("target_height").get<uint64_t>();
        bu.block_hash = body.at("result").at("block_hash").get<std::string>();
        bu.hardfork = body.at("result").at("hardfork").get<int>();

    } catch (...) {
        LOKI_LOG(trace, "swarm repsonse: {}", body.dump(2));
        LOKI_LOG(error, "Bad lokid rpc response: invalid json fields");
        throw std::runtime_error("Failed to parse swarm update");
    }

    for (auto const& swarm : swarm_map) {
        bu.swarms.emplace_back(SwarmInfo{swarm.first, swarm.second});
    }

    return bu;
}

void ServiceNode::bootstrap_ips() {
    LOKI_LOG(trace, "Bootstrapping peer ips");

    json params;
    json fields;

    fields["service_node_pubkey"] = true;
    fields["swarm_id"] = true;
    fields["storage_port"] = true;
    fields["public_ip"] = true;
    fields["height"] = true;
    fields["target_height"] = true;
    fields["block_hash"] = true;
    fields["hardfork"] = true;

    params["fields"] = fields;

    std::vector<std::pair<std::string, uint16_t>> seed_nodes{
        std::pair<std::string, uint16_t>{"3.104.19.14", 22023},
        std::pair<std::string, uint16_t>{"13.238.53.205", 38157},
        std::pair<std::string, uint16_t>{"149.56.148.124", 38157}
    };

    for (auto seed_node : seed_nodes) {
        lokid_client_.make_lokid_request(
            seed_node.first, seed_node.second, "get_n_service_nodes", params,
            [this](const sn_response_t&& res) {
                if (res.error_code == SNodeError::NO_ERROR) {
                    try {
                        const block_update_t bu = parse_swarm_update(res.body);
                        on_swarm_update(bu);
                    } catch (const std::exception& e) {
                        LOKI_LOG(error, "Exception caught on swarm update: {}",
                                e.what());
                    }
                }
            });
    }
}

bool ServiceNode::snode_ready() {
    bool ready = true;
    ready = ready && hardfork_ >= STORAGE_SERVER_HARDFORK;
    ready = ready && swarm_ && swarm_->is_valid();
    ready = ready && !syncing_;
    return ready || force_start_;
}

ServiceNode::~ServiceNode() {
    worker_ioc_.stop();
    worker_thread_.join();
};

void ServiceNode::send_sn_request(const std::shared_ptr<request_t>& req,
                                  const sn_record_t& sn) const {

    LOKI_LOG(debug, "Relaying data to: {}", sn);

    // Note: often one of the reason for failure here is that the node has just
    // deregistered but our SN hasn't updated its swarm list yet.
    make_sn_request(ioc_, sn, req, [this, sn, req](sn_response_t&& res) {
        if (res.error_code != SNodeError::NO_ERROR) {
            all_stats_.record_request_failed(sn);

            if (res.error_code == SNodeError::NO_REACH) {
                LOKI_LOG(error, "Could not relay data to: {} (Unreachable)",
                         sn);
            } else if (res.error_code == SNodeError::ERROR_OTHER) {
                LOKI_LOG(error, "Could not relay data to: {} (Generic error)",
                         sn);
            }

            std::function<void()> give_up_cb = [this, sn]() {
                this->all_stats_.record_push_failed(sn);
            };

            boost::optional<std::function<void()>> cb = give_up_cb;

            std::make_shared<FailedRequestHandler>(ioc_, sn, req, std::move(cb))
                ->init_timer();
        }
    });
}

void ServiceNode::register_listener(const std::string& pk,
                                    const std::shared_ptr<connection_t>& c) {
    pk_to_listeners[pk].push_back(c);
    LOKI_LOG(debug, "register pubkey: {}, total pubkeys: {}", pk,
             pk_to_listeners.size());
}

void ServiceNode::notify_listeners(const std::string& pk,
                                   const message_t& msg) {

    auto it = pk_to_listeners.find(pk);

    if (it != pk_to_listeners.end()) {

        auto& listeners = it->second;

        LOKI_LOG(debug, "number of notified listeners: {}", listeners.size());

        for (auto& c : listeners) {
            c->notify(msg);
        }
        pk_to_listeners.erase(it);
    }
}

void ServiceNode::reset_listeners() {

    /// It is probably not worth it to try to
    /// determine which connections needn't
    /// be reset (most of them will need to be),
    /// so we just reset all connections for
    /// simplicity
    for (auto& entry : pk_to_listeners) {
        for (auto& c : entry.second) {
            c->reset();
        }
    }

    pk_to_listeners.clear();
}

/// initiate a /swarms/push request
void ServiceNode::push_message(const message_t& msg) {

    if (!swarm_)
        return;

    const auto& others = swarm_->other_nodes();

    LOKI_LOG(debug, "push_message to {} other nodes", others.size());

    std::string body;
    serialize_message(body, msg);

#ifndef DISABLE_SNODE_SIGNATURE
    const auto hash = hash_data(body);
    const auto signature = generate_signature(hash, lokid_key_pair_);
#endif

    auto req = make_push_request(std::move(body));

#ifndef DISABLE_SNODE_SIGNATURE
    attach_signature(req, signature);
#endif

    for (const auto& address : others) {
        /// send a request asynchronously
        send_sn_request(req, address);
    }
}

/// do this asynchronously on a different thread? (on the same thread?)
bool ServiceNode::process_store(const message_t& msg) {

    /// only accept a message if we are in a swarm
    if (!swarm_) {
        LOKI_LOG(error, "error: my swarm in not initialized");
        return false;
    }

    all_stats_.client_store_requests++;

    /// store to the database
    save_if_new(msg);

    /// initiate a /swarms/push request;
    /// (done asynchronously)
    this->push_message(msg);

    return true;
}

void ServiceNode::process_push(const message_t& msg) {
#ifndef DISABLE_POW
    const char* error_msg;
    if (!verify_message(msg, pow_history_, &error_msg))
        throw std::runtime_error(error_msg);
#endif
    save_if_new(msg);
}

void ServiceNode::save_if_new(const message_t& msg) {

    if (db_->store(msg.hash, msg.pub_key, msg.data, msg.ttl, msg.timestamp,
                   msg.nonce)) {
        notify_listeners(msg.pub_key, msg);
        LOKI_LOG(debug, "saved message: {}", msg.data);
    }
}

void ServiceNode::save_bulk(const std::vector<Item>& items) {

    if (!db_->bulk_store(items)) {
        LOKI_LOG(error, "failed to save batch to the database");
        return;
    }

    LOKI_LOG(trace, "saved messages count: {}", items.size());

    // For batches, it is not trivial to get the list of saved (new)
    // messages, so we are only going to "notify" clients with no data
    // effectively resetting the connection.
    reset_listeners();
}

void ServiceNode::on_swarm_update(const block_update_t& bu) {

    hardfork_ = bu.hardfork;

    if (syncing_ && bu.target_height != 0) {
        syncing_ = bu.height < bu.target_height - 1;
    }

    /// We don't have anything to do until we have synced
    if (syncing_) {
        LOKI_LOG(debug, "Still syncing: {}/{}", bu.height, bu.target_height);
        return;
    }

    if (bu.block_hash != block_hash_) {

        LOKI_LOG(debug, "new block, height: {}, hash: {}", bu.height,
                 bu.block_hash);

        if (bu.height > block_height_ + 1 && block_height_ != 0) {
            LOKI_LOG(warn, "Skipped some block(s), old: {} new: {}",
                     block_height_, bu.height);
            /// TODO: if we skipped a block, should we try to run peer tests for
            /// them as well?
        } else if (bu.height <= block_height_) {
            // TODO: investigate how testing will be affected under reorg
            LOKI_LOG(warn,
                     "new block height is not higher than the current height");
        }

        block_height_ = bu.height;
        block_hash_ = bu.block_hash;

        block_hashes_cache_.push_back(std::make_pair(bu.height, bu.block_hash));

    } else {
        LOKI_LOG(trace, "already seen this block");
        return;
    }

    if (!swarm_) {
        LOKI_LOG(info, "Initialized our swarm");
        swarm_ = std::make_unique<Swarm>(our_address_);
    }

    const SwarmEvents events = swarm_->derive_swarm_events(bu.swarms);

    swarm_->set_swarm_id(events.our_swarm_id);

    if (!snode_ready()) {
        LOKI_LOG(warn, "Service Node is still not ready");
        return;
    }

    swarm_->update_state(bu.swarms, events);

    if (!events.new_snodes.empty()) {
        bootstrap_peers(events.new_snodes);
    }

    if (!events.new_swarms.empty()) {
        bootstrap_swarms(events.new_swarms);
    }

    if (events.decommissioned) {
        /// Go through all our PK and push them accordingly
        salvage_data();
    }

    initiate_peer_test();
}

void ServiceNode::pow_difficulty_timer_tick(const pow_dns_callback_t cb) {
    std::error_code ec;
    std::vector<pow_difficulty_t> new_history = query_pow_difficulty(ec);
    if (!ec) {
        boost::asio::post(ioc_, std::bind(cb, new_history));
    }
    pow_update_timer_.expires_after(POW_DIFFICULTY_UPDATE_INTERVAL);
    pow_update_timer_.async_wait(
        boost::bind(&ServiceNode::pow_difficulty_timer_tick, this, cb));
}

void ServiceNode::swarm_timer_tick() {
    LOKI_LOG(trace, "Swarm timer tick");

    json params;
    json fields;

    fields["service_node_pubkey"] = true;
    fields["swarm_id"] = true;
    fields["storage_port"] = true;
    fields["public_ip"] = true;
    fields["height"] = true;
    fields["target_height"] = true;
    fields["block_hash"] = true;
    fields["hardfork"] = true;

    params["fields"] = fields;

    lokid_client_.make_lokid_request(
        "get_n_service_nodes", params, [this](const sn_response_t&& res) {
            if (res.error_code == SNodeError::NO_ERROR) {
                try {
                    const block_update_t bu = parse_swarm_update(res.body);
                    on_swarm_update(bu);
                } catch (const std::exception& e) {
                    LOKI_LOG(error, "Exception caught on swarm update: {}",
                             e.what());
                }
            }
        });

    swarm_update_timer_.expires_after(SWARM_UPDATE_INTERVAL);
    swarm_update_timer_.async_wait(
        boost::bind(&ServiceNode::swarm_timer_tick, this));
}

void ServiceNode::cleanup_timer_tick() {

    all_stats_.cleanup();

    stats_cleanup_timer_.expires_after(CLEANUP_TIMER);
    stats_cleanup_timer_.async_wait(
        boost::bind(&ServiceNode::cleanup_timer_tick, this));
}

void ServiceNode::lokid_ping_timer_tick() {

    auto cb = [](const sn_response_t&& res) {
        if (res.error_code == SNodeError::NO_ERROR) {

            if (!res.body) {
                LOKI_LOG(error, "Empty body on Lokid ping");
                return;
            }

            try {
                json res_json = json::parse(*res.body);

                if (res_json.at("result").at("status").get<std::string>() ==
                    "OK") {
                    LOKI_LOG(info, "Successfully pinged Lokid");
                } else {
                    LOKI_LOG(error, "Could not ping Lokid: status is NOT OK");
                }
            } catch (...) {
                LOKI_LOG(error, "Could not ping Lokid: bad json in response");
            }

        } else {
            LOKI_LOG(warn, "Could not ping Lokid");
        }
    };

    json params;
    lokid_client_.make_lokid_request("storage_server_ping", params,
                                     std::move(cb));

    lokid_ping_timer_.expires_after(LOKID_PING_INTERVAL);
    lokid_ping_timer_.async_wait(
        boost::bind(&ServiceNode::lokid_ping_timer_tick, this));
}

static std::vector<std::shared_ptr<request_t>>
make_batch_requests(std::vector<std::string>&& data) {

    std::vector<std::shared_ptr<request_t>> result;
    result.reserve(data.size());

    std::transform(std::make_move_iterator(data.begin()),
                   std::make_move_iterator(data.end()),
                   std::back_inserter(result), make_push_all_request);
    return result;
}

void ServiceNode::perform_blockchain_test(
    bc_test_params_t test_params,
    std::function<void(blockchain_test_answer_t)>&& cb) const {

    LOKI_LOG(debug, "Delegating blockchain test to Lokid");

    nlohmann::json params;

    params["max_height"] = test_params.max_height;
    params["seed"] = test_params.seed;

    auto on_resp = [cb = std::move(cb)](const sn_response_t& resp) {
        if (resp.error_code != SNodeError::NO_ERROR || !resp.body) {
            LOKI_LOG(error, "Could not send blockchain request to Lokid");
            return;
        }

        const json body = json::parse(*resp.body, nullptr, false);

        if (body.is_discarded()) {
            LOKI_LOG(error, "Bad Lokid rpc response: invalid json");
            return;
        }

        try {
            auto result = body.at("result");
            uint64_t height = result.at("res_height").get<uint64_t>();

            cb(blockchain_test_answer_t{height});

        } catch (...) {
        }
    };

    lokid_client_.make_lokid_request("perform_blockchain_test", params,
                                     std::move(on_resp));
}

void ServiceNode::attach_signature(std::shared_ptr<request_t>& request,
                                   const signature& sig) const {

    std::string raw_sig;
    raw_sig.reserve(sig.c.size() + sig.r.size());
    raw_sig.insert(raw_sig.begin(), sig.c.begin(), sig.c.end());
    raw_sig.insert(raw_sig.end(), sig.r.begin(), sig.r.end());

    const std::string sig_b64 = boost::beast::detail::base64_encode(raw_sig);
    request->set(LOKI_SNODE_SIGNATURE_HEADER, sig_b64);

    attach_pubkey(request);
}

void ServiceNode::attach_pubkey(std::shared_ptr<request_t>& request) const {
    request->set(LOKI_SENDER_SNODE_PUBKEY_HEADER, our_address_.pub_key());
}

void abort_if_integration_test() {
#ifdef INTEGRATION_TEST
    LOKI_LOG(error, "ABORT in integration test");
    abort();
#endif
}

void ServiceNode::send_storage_test_req(const sn_record_t& testee,
                                        const Item& item) {

    auto callback = [testee, item, height = this->block_height_,
                     this](sn_response_t&& res) {
        bool success = false;

        if (res.error_code == SNodeError::NO_ERROR && res.body) {
            if (*res.body == item.data) {
                LOKI_LOG(debug,
                         "Storage test is successful for: {} at height: {}",
                         testee, height);
                success = true;
            } else {

                LOKI_LOG(warn, "Test answer doesn't match for: {} at height {}",
                         testee, height);

#ifdef INTEGRATION_TEST
                LOKI_LOG(warn, "got: {} expected: {}", *res.body, item.data);
#endif
                abort_if_integration_test();
            }
        } else {
            LOKI_LOG(error,
                     "Failed to send a storage test request to snode: {}",
                     testee);

            /// TODO: retry here, otherwise tests sometimes fail (when SN not
            /// running yet)
            // abort_if_integration_test();
        }

        this->all_stats_.record_storage_test_result(testee, success);
    };

    nlohmann::json json_body;

    json_body["height"] = block_height_;
    json_body["hash"] = item.hash;

    auto req = make_post_request("/swarms/storage_test/v1", json_body.dump());

#ifndef DISABLE_SNODE_SIGNATURE
    const auto hash = hash_data(req->body());
    const auto signature = generate_signature(hash, lokid_key_pair_);
    attach_signature(req, signature);
#else
    attach_pubkey(req);
#endif

    make_sn_request(ioc_, testee, req, callback);
}

void ServiceNode::send_blockchain_test_req(const sn_record_t& testee,
                                           bc_test_params_t params,
                                           blockchain_test_answer_t answer) {

    nlohmann::json json_body;

    json_body["max_height"] = params.max_height;
    json_body["seed"] = params.seed;

    auto req =
        make_post_request("/swarms/blockchain_test/v1", json_body.dump());

#ifndef DISABLE_SNODE_SIGNATURE
    const auto hash = hash_data(req->body());
    const auto signature = generate_signature(hash, lokid_key_pair_);
    attach_signature(req, signature);
#else
    attach_pubkey(req);
#endif

    make_sn_request(ioc_, testee, req,
                    std::bind(&ServiceNode::process_blockchain_test_response,
                              this, std::placeholders::_1, answer, testee,
                              this->block_height_));
}

void ServiceNode::process_blockchain_test_response(
    sn_response_t&& res, blockchain_test_answer_t our_answer,
    sn_record_t testee, uint64_t bc_height) {

    LOKI_LOG(debug,
             "Processing blockchain test response from: {} at height: {}",
             testee, bc_height);

    bool success = false;

    if (res.error_code == SNodeError::NO_ERROR && res.body) {

        try {

            const json body = json::parse(*res.body, nullptr, true);
            uint64_t their_height = body.at("res_height").get<uint64_t>();

            if (our_answer.res_height == their_height) {
                success = true;
                LOKI_LOG(debug, "Success.");
            } else {
                LOKI_LOG(debug, "Failed: incorrect answer.");
            }

        } catch (...) {
            LOKI_LOG(debug, "Failed: could not find answer in json.");
        }

    } else {
        LOKI_LOG(debug, "Failed to send a blockchain test request to snode: {}",
                 testee);
    }

    this->all_stats_.record_blockchain_test_result(testee, success);
}

// Deterministically selects two random swarm members; returns true on success
bool ServiceNode::derive_tester_testee(uint64_t blk_height, sn_record_t& tester,
                                       sn_record_t& testee) {

    std::vector<sn_record_t> members = swarm_->other_nodes();
    members.push_back(our_address_);

    if (members.size() < 2) {
        LOKI_LOG(error, "Could not initiate peer test: swarm too small");
        return false;
    }

    std::sort(members.begin(), members.end());

    std::string block_hash;
    if (blk_height == block_height_) {
        block_hash = block_hash_;
    } else if (blk_height < block_height_) {

        LOKI_LOG(debug, "got storage test request for an older block");

        const auto it =
            std::find_if(block_hashes_cache_.begin(), block_hashes_cache_.end(),
                         [=](const std::pair<uint64_t, std::string>& val) {
                             return val.first == blk_height;
                         });

        if (it != block_hashes_cache_.end()) {
            block_hash = it->second;
        } else {
            LOKI_LOG(warn, "Could not find hash for a given block height");
            // TODO: request from lokid?
            return false;
        }
    } else {
        assert(false);
        LOKI_LOG(error, "Could not find hash: block height is in the future");
        return false;
    }

    uint64_t seed;
    if (block_hash.size() < sizeof(seed)) {
        LOKI_LOG(error, "Could not initiate peer test: invalid block hash");
        return false;
    }

    std::memcpy(&seed, block_hash.data(), sizeof(seed));
    std::mt19937_64 mt(seed);
    const auto tester_idx =
        util::uniform_distribution_portable(mt, members.size());
    tester = members[tester_idx];

    uint64_t testee_idx;
    do {
        testee_idx = util::uniform_distribution_portable(mt, members.size());
    } while (testee_idx == tester_idx);

    testee = members[testee_idx];

    return true;
}

MessageTestStatus ServiceNode::process_storage_test_req(
    uint64_t blk_height, const std::string& tester_addr,
    const std::string& msg_hash, std::string& answer) {

    // 1. Check height, retry if we are behind
    std::string block_hash;

    if (blk_height > block_height_) {
        LOKI_LOG(warn, "Our blockchain is behind, height: {}, requested: {}",
                 block_height_, blk_height);
        return MessageTestStatus::RETRY;
    }

    // 2. Check tester/testee pair
    {
        sn_record_t tester;
        sn_record_t testee;
        derive_tester_testee(blk_height, tester, testee);

        if (testee != our_address_) {
            LOKI_LOG(warn, "We are NOT the testee for height: {}", blk_height);
            return MessageTestStatus::ERROR;
        }

        if (tester.sn_address() != tester_addr) {
            LOKI_LOG(warn, "Wrong tester: {}, expected: {}", tester_addr,
                     tester.sn_address());
            abort_if_integration_test();
            return MessageTestStatus::ERROR;
        } else {
            LOKI_LOG(trace, "Tester is valid: {}", tester_addr);
        }
    }

    // 3. If for a current/past block, try to respond right away
    Item item;
    if (!db_->retrieve_by_hash(msg_hash, item)) {
        return MessageTestStatus::RETRY;
    }

    answer = item.data;
    return MessageTestStatus::SUCCESS;
}

bool ServiceNode::select_random_message(Item& item) {

    uint64_t message_count;
    if (!db_->get_message_count(message_count)) {
        LOKI_LOG(error, "Could not count messages in the database");
        return false;
    }

    LOKI_LOG(debug, "total messages: {}", message_count);

    if (message_count == 0) {
        LOKI_LOG(warn, "No messages in the database to initiate a peer test");
        return false;
    }

    // SNodes don't have to agree on this, rather they should use different
    // messages
    const uint64_t seed =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 mt(seed);
    const auto msg_idx = util::uniform_distribution_portable(mt, message_count);

    if (!db_->retrieve_by_index(msg_idx, item)) {
        LOKI_LOG(error, "Could not retrieve message by index: {}", msg_idx);
        return false;
    }

    return true;
}

void ServiceNode::initiate_peer_test() {

    // 1. Select the tester/testee pair
    sn_record_t tester, testee;
    if (!derive_tester_testee(block_height_, tester, testee)) {
        return;
    }

    LOKI_LOG(trace, "For height {}; tester: {} testee: {}", block_height_,
             tester, testee);

    if (tester != our_address_) {
        /// Not our turn to initiate a test
        return;
    }

    /// 2. Storage Testing
    {
        // 2.1. Select a message
        Item item;
        if (!this->select_random_message(item)) {
            LOKI_LOG(warn, "Could not select a message for testing");
        } else {
            LOKI_LOG(trace, "Selected random message: {}, {}", item.hash,
                     item.data);

            // 2.2. Initiate testing request
            send_storage_test_req(testee, item);
        }
    }

    // Note: might consider choosing a different tester/testee pair for
    // different types of tests as to spread out the computations

    /// 3. Blockchain Testing
    {

        // Distance between two consecutive checkpoints,
        // should be in sync with lokid
        constexpr uint64_t CHECKPOINT_DISTANCE = 4;
        // We can be confident that blockchain data won't
        // change if we go this many blocks back
        constexpr uint64_t SAFETY_BUFFER_BLOCKS = CHECKPOINT_DISTANCE * 2;

        if (block_height_ <= SAFETY_BUFFER_BLOCKS) {
            LOKI_LOG(debug,
                     "Blockchain too short, skipping blockchain testing.");
            return;
        }

        bc_test_params_t params;
        params.max_height = block_height_ - SAFETY_BUFFER_BLOCKS;

        const uint64_t rng_seed = std::chrono::high_resolution_clock::now()
                                      .time_since_epoch()
                                      .count();
        std::mt19937_64 mt(rng_seed);
        params.seed = mt();

        auto callback = std::bind(&ServiceNode::send_blockchain_test_req, this,
                                  testee, params, std::placeholders::_1);

        /// Compute your own answer, then initiate a test request
        perform_blockchain_test(params, callback);
    }
}

void ServiceNode::bootstrap_peers(const std::vector<sn_record_t>& peers) const {

    std::vector<Item> all_entries;
    get_all_messages(all_entries);

    relay_messages(all_entries, peers);
}

template <typename T>
std::string vec_to_string(const std::vector<T>& vec) {

    std::stringstream ss;

    ss << "[";

    for (auto i = 0u; i < vec.size(); ++i) {
        ss << vec[i];

        if (i < vec.size() - 1) {
            ss << " ";
        }
    }

    ss << "]";

    return ss.str();
}

void ServiceNode::bootstrap_swarms(
    const std::vector<swarm_id_t>& swarms) const {

    if (swarms.empty()) {
        LOKI_LOG(info, "Bootstrapping all swarms");
    } else {
        LOKI_LOG(info, "Bootstrapping swarms: {}", vec_to_string(swarms));
    }

    const auto& all_swarms = swarm_->all_swarms();

    std::vector<Item> all_entries;
    if (!get_all_messages(all_entries)) {
        LOKI_LOG(error, "Could not retrieve entries from the database");
        return;
    }

    std::unordered_map<swarm_id_t, size_t> swarm_id_to_idx;
    for (auto i = 0u; i < all_swarms.size(); ++i) {
        swarm_id_to_idx.insert({all_swarms[i].swarm_id, i});
    }

    /// See what pubkeys we have
    std::unordered_map<std::string, swarm_id_t> cache;

    LOKI_LOG(debug, "We have {} messages", all_entries.size());

    std::unordered_map<swarm_id_t, std::vector<Item>> to_relay;

    for (auto& entry : all_entries) {

        swarm_id_t swarm_id;
        const auto it = cache.find(entry.pub_key);
        if (it == cache.end()) {
            swarm_id = get_swarm_by_pk(all_swarms, entry.pub_key);
            cache.insert({entry.pub_key, swarm_id});
        } else {
            swarm_id = it->second;
        }

        bool relevant = false;
        for (const auto swarm : swarms) {

            if (swarm == swarm_id) {
                relevant = true;
            }
        }

        if (relevant || swarms.empty()) {

            to_relay[swarm_id].emplace_back(std::move(entry));
        }
    }

    LOKI_LOG(trace, "Bootstrapping {} swarms", to_relay.size());

    for (const auto& kv : to_relay) {
        const uint64_t swarm_id = kv.first;
        /// what if not found?
        const size_t idx = swarm_id_to_idx[swarm_id];

        relay_messages(kv.second, all_swarms[idx].snodes);
    }
}

void ServiceNode::relay_messages(const std::vector<storage::Item>& messages,
                                 const std::vector<sn_record_t>& snodes) const {
    std::vector<std::string> data = serialize_messages(messages);

#ifndef DISABLE_SNODE_SIGNATURE
    std::vector<signature> signatures;
    signatures.reserve(data.size());
    for (const auto& d : data) {
        const auto hash = hash_data(d);
        signatures.push_back(generate_signature(hash, lokid_key_pair_));
    }
#endif

    std::vector<std::shared_ptr<request_t>> batches =
        make_batch_requests(std::move(data));

#ifndef DISABLE_SNODE_SIGNATURE
    assert(batches.size() == signatures.size());
    for (size_t i = 0; i < batches.size(); ++i) {
        attach_signature(batches[i], signatures[i]);
    }
#endif

    LOKI_LOG(debug, "Serialised batches: {}", data.size());
    for (const sn_record_t& sn : snodes) {
        for (const std::shared_ptr<request_t>& batch : batches) {
            send_sn_request(batch, sn);
        }
    }
}

void ServiceNode::salvage_data() const {

    /// This is very similar to ServiceNode::bootstrap_swarms, so just reuse it
    bootstrap_swarms({});
}

bool ServiceNode::retrieve(const std::string& pubKey,
                           const std::string& last_hash,
                           std::vector<Item>& items) {
    return db_->retrieve(pubKey, items, last_hash,
                         CLIENT_RETRIEVE_MESSAGE_LIMIT);
}

void ServiceNode::set_difficulty_history(
    const std::vector<pow_difficulty_t>& new_history) {
    pow_history_ = new_history;
    for (const auto& difficulty : pow_history_) {
        if (curr_pow_difficulty_.timestamp < difficulty.timestamp) {
            curr_pow_difficulty_ = difficulty;
        }
    }
    LOKI_LOG(info, "Read PoW difficulty: {}", curr_pow_difficulty_.difficulty);
}

int ServiceNode::get_curr_pow_difficulty() const {
    return curr_pow_difficulty_.difficulty;
}

bool ServiceNode::get_all_messages(std::vector<Item>& all_entries) const {

    LOKI_LOG(trace, "Get all messages");

    return db_->retrieve("", all_entries, "");
}

void ServiceNode::process_push_batch(const std::string& blob) {
    // Note: we only receive batches on bootstrap (new swarm/new snode)

    if (blob.empty())
        return;

    std::vector<message_t> messages = deserialize_messages(blob);

    LOKI_LOG(trace, "Saving all: begin");

    LOKI_LOG(debug, "Got {} messages from peers, size: {}", messages.size(),
             blob.size());

#ifndef DISABLE_POW
    const auto it = std::remove_if(
        messages.begin(), messages.end(), [this](const message_t& message) {
            return verify_message(message, pow_history_) == false;
        });
    messages.erase(it, messages.end());
    if (it != messages.end()) {
        LOKI_LOG(
            warn,
            "Some of the batch messages were removed due to incorrect PoW");
    }
#endif

    std::vector<Item> items;
    items.reserve(messages.size());

    // TODO: avoid copying m.data
    // Promoting message_t to Item:
    std::transform(messages.begin(), messages.end(), std::back_inserter(items),
                   [](const message_t& m) {
                       return Item{m.hash, m.pub_key,           m.timestamp,
                                   m.ttl,  m.timestamp + m.ttl, m.nonce,
                                   m.data};
                   });

    save_bulk(items);

    LOKI_LOG(trace, "Saving all: end");
}

bool ServiceNode::is_pubkey_for_us(const std::string& pk) const {
    if (!swarm_) {
        LOKI_LOG(error, "Swarm data missing");
        return false;
    }
    return swarm_->is_pubkey_for_us(pk);
}

std::vector<sn_record_t> ServiceNode::get_snodes_by_pk(const std::string& pk) {

    if (!swarm_) {
        LOKI_LOG(error, "Swarm data missing");
        return {};
    }

    const auto& all_swarms = swarm_->all_swarms();

    swarm_id_t swarm_id = get_swarm_by_pk(all_swarms, pk);

    // TODO: have get_swarm_by_pk return idx into all_swarms instead,
    // so we don't have to find it again

    for (const auto& si : all_swarms) {
        if (si.swarm_id == swarm_id)
            return si.snodes;
    }

    LOKI_LOG(error, "Something went wrong in get_snodes_by_pk");

    return {};
}

bool ServiceNode::is_snode_address_known(const std::string& sn_address) {

    // TODO: need more robust handling of uninitialized swarm_
    if (!swarm_) {
        LOKI_LOG(error, "Swarm data missing");
        return {};
    }

    const auto& all_swarms = swarm_->all_swarms();

    return std::any_of(all_swarms.begin(), all_swarms.end(),
                       [&sn_address](const SwarmInfo& swarm_info) {
                           return std::any_of(
                               swarm_info.snodes.begin(),
                               swarm_info.snodes.end(),
                               [&sn_address](const sn_record_t& sn_record) {
                                   return sn_record.sn_address() == sn_address;
                               });
                       });
}

} // namespace loki

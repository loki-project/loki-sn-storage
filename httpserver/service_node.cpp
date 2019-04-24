#include "service_node.h"

#include "Database.hpp"
#include "lokid_key.h"
#include "utils.hpp"

#include "Item.hpp"
#include "http_connection.h"

#include <chrono>
#include <fstream>
#include <iomanip>

#include <boost/bind.hpp>

#include <boost/log/trivial.hpp>

/// move this out
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "serialization.h"

using service_node::storage::Item;

namespace loki {
using http_server::connection_t;

constexpr std::array<std::chrono::seconds, 5> RETRY_INTERVALS = {
    std::chrono::seconds(5), std::chrono::seconds(10), std::chrono::seconds(20),
    std::chrono::seconds(40), std::chrono::seconds(80)};

FailedRequestHandler::FailedRequestHandler(boost::asio::io_context& ioc,
                                           const sn_record_t& sn,
                                           std::shared_ptr<request_t> req)
    : ioc_(ioc), retry_timer_(ioc), sn_(sn), request_(std::move(req)) {}

void FailedRequestHandler::retry(std::shared_ptr<FailedRequestHandler>&& self) {

    attempt_count_ += 1;
    if (attempt_count_ > RETRY_INTERVALS.size()) {
        BOOST_LOG_TRIVIAL(debug)
            << "Gave up after " << attempt_count_ << " attempts";
        return;
    }

    retry_timer_.expires_after(RETRY_INTERVALS[attempt_count_ - 1]);
    BOOST_LOG_TRIVIAL(debug)
        << "Will retry in " << RETRY_INTERVALS[attempt_count_ - 1].count()
        << " secs";

    retry_timer_.async_wait(
        [self = std::move(self)](const boost::system::error_code& ec) mutable {
            /// Save some references before possibly moved out of `self`
            const auto& sn = self->sn_;
            auto& ioc = self->ioc_;
            /// TODO: investigate whether we can get rid of the extra ptr copy here?
            const std::shared_ptr<request_t> req = self->request_;

            /// Request will be copied here
            make_http_request(
                ioc, sn.address, sn.port, req,
                [self = std::move(self)](sn_response_t&& res) mutable {
                    if (res.error_code != SNodeError::NO_ERROR) {
                        BOOST_LOG_TRIVIAL(error)
                            << "Could not relay one: " << self->sn_
                            << " (attempt #" << self->attempt_count_ << ")";
                        /// TODO: record failure here as well?
                        self->retry(std::move(self));
                    }
                });
        });
}

FailedRequestHandler::~FailedRequestHandler() {
    BOOST_LOG_TRIVIAL(trace) << "~FailedRequestHandler()";
}

void FailedRequestHandler::init_timer() { retry(shared_from_this()); }

/// TODO: can we reuse context (reset it)?
std::string hash_data(std::string data) {

    unsigned char result[EVP_MAX_MD_SIZE];

    /// Allocate and init digest context
    EVP_MD_CTX* mdctx = EVP_MD_CTX_create();

    /// Set the method
    EVP_DigestInit_ex(mdctx, EVP_sha512(), NULL);

    /// Do the hashing, can be called multiple times (?)
    /// to hash
    EVP_DigestUpdate(mdctx, data.data(), data.size());

    unsigned int md_len;

    EVP_DigestFinal_ex(mdctx, result, &md_len);

    /// Clean up the context
    EVP_MD_CTX_destroy(mdctx);

    /// Not sure if this is needed
    EVP_cleanup();

    /// store into the string
    /// TODO: use binary instead?
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < EVP_MAX_MD_SIZE; i++) {
        ss << std::setw(2) << static_cast<unsigned>(result[i]);
    }

    return std::string(ss.str());
}

static std::shared_ptr<request_t> make_post_request(const char* target, std::string&& data) {
    auto req = std::make_shared<request_t>();
    req->body() = std::move(data);
    req->method(http::verb::post);
    req->set(http::field::host, "service node");
    req->target(target);
    req->prepare_payload();
    return req;
}

static std::shared_ptr<request_t> make_push_all_request(std::string&& data) {
    return make_post_request("/v1/swarms/push_batch", std::move(data));
}

static std::shared_ptr<request_t> make_push_request(std::string&& data) {
    return make_post_request("/v1/swarms/push", std::move(data));
}

ServiceNode::ServiceNode(boost::asio::io_context& ioc, uint16_t port,
                         const std::vector<uint8_t>& public_key,
                         const std::string& dbLocation)
    : ioc_(ioc), db_(std::make_unique<Database>(dbLocation)),
      update_timer_(ioc, std::chrono::milliseconds(100)) {

#ifndef INTEGRATION_TEST
    char buf[64] = {0};
    std::string our_address;
    if (char const* dest = util::base32z_encode(public_key, buf)) {
        our_address.append(dest);
        our_address.append(".snode");
    }
    BOOST_LOG_TRIVIAL(info) << "Read snode address " << our_address;
    our_address_.address = our_address;
#endif
    our_address_.port = port;

    swarm_timer_tick();
}

ServiceNode::~ServiceNode() = default;

void ServiceNode::relay_data(const std::shared_ptr<request_t>& req,
                            sn_record_t sn) const {

    BOOST_LOG_TRIVIAL(debug) << "Relaying data to: " << sn;

    make_http_request(
        ioc_, sn.address, sn.port, req, [this, sn, req](sn_response_t&& res) {
            if (res.error_code != SNodeError::NO_ERROR) {
                snode_report_[sn].relay_fails += 1;

                if (res.error_code == SNodeError::NO_REACH) {
                    BOOST_LOG_TRIVIAL(error)
                        << "Could not relay data to: " << sn << " (Unreachable)";
                } else if (res.error_code == SNodeError::ERROR_OTHER) {
                    BOOST_LOG_TRIVIAL(error) << "Could not relay data to: " << sn
                                             << " (Generic error)";
                }

                std::make_shared<FailedRequestHandler>(ioc_, sn, req)
                    ->init_timer();
            }
        });
}

void ServiceNode::register_listener(const std::string& pk,
                                    const std::shared_ptr<connection_t>& c) {
    pk_to_listeners[pk].push_back(c);
    BOOST_LOG_TRIVIAL(debug) << "register pubkey: " << pk
                             << ", total pubkeys: " << pk_to_listeners.size();
}

void ServiceNode::notify_listeners(const std::string& pk,
                                   const message_t& msg) {

    auto it = pk_to_listeners.find(pk);

    if (it != pk_to_listeners.end()) {

        auto& listeners = it->second;

        BOOST_LOG_TRIVIAL(debug)
            << "number of notified listeners: " << listeners.size();

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

    BOOST_LOG_TRIVIAL(debug)
        << "push_message to " << others.size() << " other nodes";

    std::string body;
    serialize_message(body, msg);
    auto req = make_push_request(std::move(body));

    for (const auto& address : others) {
        /// send a request asynchronously
        relay_data(req, address);
    }
}

/// do this asynchronously on a different thread? (on the same thread?)
bool ServiceNode::process_store(const message_t& msg) {

    /// TODO: accept messages if they are coming from other service nodes

    /// only accept a message if we are in a swarm
    if (!swarm_) {
        BOOST_LOG_TRIVIAL(error) << "error: my swarm in not initialized";
        return false;
    }

    /// store to the database
    save_if_new(msg);

    /// initiate a /swarms/push request;
    /// (done asynchronously)
    this->push_message(msg);

    return true;
}

void ServiceNode::process_push(const message_t& msg) { save_if_new(msg); }

void ServiceNode::save_if_new(const message_t& msg) {

    if (db_->store(msg.hash, msg.pub_key, msg.data, msg.ttl, msg.timestamp,
                   msg.nonce)) {
        notify_listeners(msg.pub_key, msg);
        BOOST_LOG_TRIVIAL(debug) << "saved message: " << msg.data;
    }
}

void ServiceNode::save_bulk(const std::vector<Item>& items) {

    if (!db_->bulk_store(items)) {
        BOOST_LOG_TRIVIAL(error) << "failed to save batch to the database";
        return;
    }

    BOOST_LOG_TRIVIAL(trace) << "saved messages count: " << items.size();

    // For batches, it is not trivial to get the list of saved (new)
    // messages, so we are only going to "notify" clients with no data
    // effectively resetting the connection.
    reset_listeners();
}

void ServiceNode::on_swarm_update(all_swarms_t all_swarms) {
    if (!swarm_) {
        BOOST_LOG_TRIVIAL(trace) << "initialized our swarm";
        swarm_ = std::make_unique<Swarm>(our_address_);
    }

    const SwarmEvents events = swarm_->update_swarms(all_swarms);

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

    this->purge_outdated();
}

void ServiceNode::swarm_timer_tick() {
    const swarm_callback_t cb =
        std::bind(&ServiceNode::on_swarm_update, this, std::placeholders::_1);
    request_swarm_update(ioc_, std::move(cb));
    update_timer_.expires_after(std::chrono::seconds(2));
    update_timer_.async_wait(boost::bind(&ServiceNode::swarm_timer_tick, this));
}

static std::vector<std::shared_ptr<request_t>>
to_requests(std::vector<std::string>&& data) {

    std::vector<std::shared_ptr<request_t>> result;
    result.reserve(data.size());

    std::transform(std::make_move_iterator(data.begin()),
                   std::make_move_iterator(data.end()),
                   std::back_inserter(result), make_push_all_request);
    return result;
}

void ServiceNode::bootstrap_peers(const std::vector<sn_record_t>& peers) const {

    std::vector<Item> all_entries;
    db_->retrieve("", all_entries, "");

    std::vector<std::string> data = serialize_messages(all_entries);
    std::vector<std::shared_ptr<request_t>> batches = to_requests(std::move(data));


    for (const sn_record_t& sn : peers) {
        for (const std::shared_ptr<request_t>& batch : batches) {
            relay_data(batch, sn);
        }
    }
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
        BOOST_LOG_TRIVIAL(info) << "bootstrapping all swarms\n";
    } else {
        BOOST_LOG_TRIVIAL(info)
            << "bootstrapping swarms: " << vec_to_string(swarms);
    }

    const auto& all_swarms = swarm_->all_swarms();

    std::vector<Item> all_entries;
    if (!db_->retrieve("", all_entries, "")) {
        BOOST_LOG_TRIVIAL(error)
            << "could not retrieve entries from the database\n";
        return;
    }

    std::unordered_map<swarm_id_t, size_t> swarm_id_to_idx;
    for (auto i = 0u; i < all_swarms.size(); ++i) {
        swarm_id_to_idx.insert({all_swarms[i].swarm_id, i});
    }

    /// See what pubkeys we have
    std::unordered_map<std::string, swarm_id_t> cache;

    BOOST_LOG_TRIVIAL(debug)
        << "we have " << all_entries.size() << " messages\n";

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

    BOOST_LOG_TRIVIAL(trace)
        << "Bootstrapping " << to_relay.size() << " swarms";

    for (const auto& kv : to_relay) {
        const uint64_t swarm_id = kv.first;
        /// what if not found?
        const size_t idx = swarm_id_to_idx[swarm_id];

        std::vector<std::string> data = serialize_messages(kv.second);
        std::vector<std::shared_ptr<request_t>> batches = to_requests(std::move(data));

        BOOST_LOG_TRIVIAL(info) << "serialized batches: " << data.size();

        for (const sn_record_t& sn : all_swarms[idx].snodes) {
            for (const std::shared_ptr<request_t>& batch : batches) {
                relay_data(batch, sn);
            }
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
    return db_->retrieve(pubKey, items, last_hash);
}

bool ServiceNode::get_all_messages(std::vector<Item>& all_entries) {

    BOOST_LOG_TRIVIAL(trace) << "get all messages";

    return db_->retrieve("", all_entries, "");
}

void ServiceNode::purge_outdated() {

    /// TODO: use database instead, for now it is a no-op
    return;
}

void ServiceNode::process_push_batch(const std::string& blob) {
    // Note: we only receive batches on bootstrap (new swarm/new snode)

    if (blob.empty())
        return;

    const std::vector<message_t> messages = deserialize_messages(blob);

    BOOST_LOG_TRIVIAL(trace) << "saving all: begin";

    BOOST_LOG_TRIVIAL(debug) << "got " << messages.size()
                             << " messages from peers, size: " << blob.size();

    std::vector<Item> items;
    items.reserve(messages.size());

    // Promoting message_t to Item:
    std::transform(messages.begin(), messages.end(), std::back_inserter(items),
                   [](const message_t& m) {
                       return Item{m.hash, m.pub_key,           m.timestamp,
                                   m.ttl,  m.timestamp + m.ttl, m.nonce,
                                   m.data};
                   });

    save_bulk(items);

    BOOST_LOG_TRIVIAL(trace) << "saving all: end";
}

bool ServiceNode::is_pubkey_for_us(const std::string& pk) const {
    const auto& all_swarms = swarm_->all_swarms();
    return swarm_->is_pubkey_for_us(all_swarms, pk);
}

std::vector<sn_record_t> ServiceNode::get_snodes_by_pk(const std::string& pk) {

    const auto& all_swarms = swarm_->all_swarms();

    swarm_id_t swarm_id = get_swarm_by_pk(all_swarms, pk);

    // TODO: have get_swarm_by_pk return idx into all_swarms instead,
    // so we don't have to find it again

    for (const auto& si : all_swarms) {
        if (si.swarm_id == swarm_id)
            return si.snodes;
    }

    BOOST_LOG_TRIVIAL(fatal) << "Something went wrong in get_snodes_by_pk";

    return {};
}

} // namespace loki

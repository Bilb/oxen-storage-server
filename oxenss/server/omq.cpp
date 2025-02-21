#include "omq.h"
#include "omq_logger.h"

#include <oxenss/crypto/channel_encryption.hpp>
#include <oxenmq/auth.h>
#include <oxenmq/connections.h>
#include <oxenss/crypto/keys.h>
#include <oxenss/logging/oxen_logger.h>
#include <oxenss/rpc/rate_limiter.h>
#include <oxenss/rpc/request_handler.h>
#include <oxenss/snode/service_node.h>
#include <oxenss/utils/string_utils.hpp>

#include <oxenc/base64.h>
#include <oxenc/bt_serialize.h>
#include <oxenc/hex.h>
#include <oxenc/bt_producer.h>
#include <fmt/std.h>
#include <sodium/crypto_sign.h>

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace oxenss::server {

static auto logcat = log::Cat("server");

std::string OMQ::peer_lookup(std::string_view pubkey_bin) const {
    log::trace(logcat, "[OMQ] Peer Lookup");

    if (pubkey_bin.size() != sizeof(crypto::x25519_pubkey))
        return "";
    crypto::x25519_pubkey pubkey;
    std::memcpy(pubkey.data(), pubkey_bin.data(), sizeof(crypto::x25519_pubkey));

    if (auto sn = service_node_->find_node(pubkey))
        return fmt::format("tcp://{}:{}", sn->ip, sn->omq_quic_port);

    log::debug(logcat, "[OMQ] peer node not found via x25519 pubkey {}!", pubkey);
    return "";
}

void OMQ::handle_sn_data(oxenmq::Message& message) {
    log::debug(logcat, "[OMQ] handle_sn_data");
    log::debug(logcat, "[OMQ]   thread id: {}", std::this_thread::get_id());
    log::debug(logcat, "[OMQ]   from: {}", oxenc::to_hex(message.conn.pubkey()));

    std::stringstream ss;

    // We are only expecting a single part message, so consider removing this
    for (auto& part : message.data) {
        ss << part;
    }

    // TODO: process push batch should move to "Request handler"
    service_node_->process_push_batch(ss.str());

    log::debug(logcat, "[OMQ] send reply");

    // TODO: Investigate if the above could fail and whether we should report
    // that to the sending SN
    message.send_reply();
};

void OMQ::handle_ping(oxenmq::Message& message) {
    log::debug(logcat, "Remote pinged me");
    service_node_->update_last_ping(snode::ReachType::OMQ);
    message.send_reply("pong");
}

void OMQ::handle_storage_test(oxenmq::Message& message) {
    if (message.conn.pubkey().size() != 32) {
        // This shouldn't happen as this endpoint should have remote-SN-only permissions, so be
        // noisy
        log::error(
                logcat,
                "bug: invalid sn.storage_test omq request from {} with no pubkey",
                message.remote);
        return message.send_reply("invalid parameters");
    } else if (message.data.size() < 2) {
        log::warning(
                logcat,
                "invalid sn.storage_test omq request from {}: not enough data parts; expected 2, "
                "received {}",
                message.remote,
                message.data.size());
        return message.send_reply("invalid parameters");
    }
    crypto::legacy_pubkey tester_pk;
    if (auto node = service_node_->find_node(
                crypto::x25519_pubkey::from_bytes(message.conn.pubkey()))) {
        tester_pk = node->pubkey_legacy;
        log::debug(
                logcat, "incoming sn.storage_test request from {}@{}", tester_pk, message.remote);
    } else {
        log::warning(logcat, "invalid sn.storage_test omq request: sender is not an active SN");
        return message.send_reply("invalid pubkey");
    }

    uint64_t height;
    if (!util::parse_int(message.data[0], height) || !height) {
        log::warning(
                logcat,
                "invalid sn.storage_test omq request from {}@{}: '{}' is not a valid height",
                tester_pk,
                message.remote,
                height);
        return message.send_reply("invalid height");
    }
    std::string msg_hash;
    if (message.data[1].size() == 64)
        msg_hash = oxenc::to_hex(message.data[1]);
    else if (message.data[1].size() == 32) {
        msg_hash = oxenc::to_base64(message.data[1]);
        assert(msg_hash.back() == '=');
        msg_hash.pop_back();
    } else {
        log::warning(
                logcat,
                "invalid sn.storage_test omq request from {}@{}: message hash is {} bytes, "
                "expected 64 or 32",
                tester_pk,
                message.remote,
                message.data[1].size());
        return message.send_reply("invalid msg hash");
    }

    request_handler_->process_storage_test_req(
            height,
            tester_pk,
            msg_hash,
            [reply = message.send_later()](
                    snode::MessageTestStatus status,
                    std::string answer,
                    std::chrono::steady_clock::duration elapsed) {
                switch (status) {
                    case snode::MessageTestStatus::SUCCESS:
                        log::debug(
                                logcat,
                                "Storage test success after {}",
                                util::friendly_duration(elapsed));
                        reply.reply("OK", answer);
                        return;
                    case snode::MessageTestStatus::WRONG_REQ: reply.reply("wrong request"); return;
                    case snode::MessageTestStatus::RETRY:
                        [[fallthrough]];  // If we're getting called then a retry ran out of time
                    case snode::MessageTestStatus::ERROR:
                        // Promote this to `error` once we enforce storage testing
                        log::debug(
                                logcat,
                                "Failed storage test, tried for {}",
                                util::friendly_duration(elapsed));
                        reply.reply("other");
                }
            });
}

void OMQ::handle_onion_request(
        std::string_view payload,
        rpc::OnionRequestMetadata&& data,
        oxenmq::Message::DeferredSend send) {
    data.cb = [send](rpc::Response res) {
#ifndef NDEBUG
        log::trace(logcat, "on response: {}...", to_string(res).substr(0, 100));
#endif

        if (auto* js = std::get_if<nlohmann::json>(&res.body))
            send.reply(std::to_string(res.status.first), js->dump());
        else
            send.reply(std::to_string(res.status.first), view_body(res));
    };

    if (data.hop_no > rpc::MAX_ONION_HOPS)
        return data.cb({http::BAD_REQUEST, "onion request max path length exceeded"sv});

    request_handler_->process_onion_req(payload, std::move(data));
}

void OMQ::handle_onion_request(oxenmq::Message& message) {
    std::pair<std::string_view, rpc::OnionRequestMetadata> data;
    try {
        if (message.data.size() != 1)
            throw std::runtime_error{"expected 1 part, got " + std::to_string(message.data.size())};

        data = decode_onion_data(message.data[0]);
    } catch (const std::exception& e) {
        auto msg = "Invalid internal onion request: "s + e.what();
        log::error(logcat, "{}", msg);
        message.send_reply(std::to_string(http::BAD_REQUEST.first), msg);
        return;
    }

    handle_onion_request(data.first, std::move(data.second), message.send_later());
}

void OMQ::handle_get_stats(oxenmq::Message& message) {

    log::debug(logcat, "Received get_stats request via OMQ");

    auto payload = service_node_->get_stats();

    message.send_reply(payload);
}

void OMQ::handle_client_request(std::string_view method, oxenmq::Message& message, bool forwarded) {
    log::debug(logcat, "Handling OMQ RPC request for {}", method);

    const size_t full_size = forwarded ? 2 : 1;
    const size_t empty_body = full_size - 1;
    if (message.data.size() != empty_body && message.data.size() != full_size) {
        log::warning(
                logcat,
                "Invalid {}OMQ RPC request for {}: incorrect number of message parts ({})",
                forwarded ? "forwarded " : "",
                method,
                message.data.size());
        message.send_reply(
                std::to_string(http::BAD_REQUEST.first),
                fmt::format(
                        "Invalid request: expected {} message parts, received {}",
                        full_size,
                        message.data.size()));
        return;
    }

    [[maybe_unused]] bool found = handle_client_rpc(
            method,
            message.data.size() == full_size ? message.data.back() : ""sv,
            message.remote,
            [send = message.send_later()](http::response_code status, std::string_view body) {
                if (status == http::OK)
                    send.reply(body);
                else
                    send.reply(std::to_string(status.first), body);
            },
            forwarded);

    // This endpoint shouldn't have been registered at all if it isn't found in here
    assert(found);
}

OMQ::OMQ(
        const snode::sn_record& me,
        const crypto::x25519_seckey& privkey,
        const std::vector<crypto::x25519_pubkey>& stats_access_keys) :
        omq_{std::string{me.pubkey_x25519.view()},
             std::string{privkey.view()},
             true,                                         // is service node
             [this](auto pk) { return peer_lookup(pk); },  // SN-by-key lookup func
             omq_logger,
             oxenmq::LogLevel::info} {
    for (const auto& key : stats_access_keys)
        stats_access_keys_.emplace(key.view());

    // clang-format off

    // Endpoints invoked by other SNs
    omq_.add_category("sn", oxenmq::Access{oxenmq::AuthLevel::none, true, false}, 2 /*reserved threads*/, 1000 /*max queue*/)
        .add_request_command("data", [this](auto& m) { handle_sn_data(m); })
        .add_request_command("ping", [this](auto& m) { handle_ping(m); })
        .add_request_command("storage_test", [this](auto& m) { handle_storage_test(m); }) // NB: requires a 60s request timeout
        .add_request_command("onion_request", [this](auto& m) { handle_onion_request(m); })
        .add_request_command("storage_cc", [this](auto& m) {
            if (m.data.size() >= 2) return handle_client_request(m.data[0], m, true);
            log::warning(logcat, "Invalid forwarded client request: incorrect number of message parts ({})",  m.data.size());
        })
        ;

    // storage.WHATEVER (e.g. storage.store, storage.retrieve, etc.) endpoints are invokable by
    // anyone (i.e. clients) and have the same WHATEVER endpoints as the "method" values for the
    // HTTPS /storage_rpc/v1 endpoint.
    auto st_cat = omq_.add_category("storage", oxenmq::AuthLevel::none, 1 /*reserved threads*/, 200 /*max queue*/);
    for (const auto& [name, _cb] : rpc::RequestHandler::client_rpc_endpoints)
        st_cat.add_request_command(std::string{name}, [this, name=name](auto& m) { handle_client_request(name, m); });

    // monitor.* endpoints are used to subscribe to events such as new messages arriving for an
    // account.
    omq_.add_category("monitor", oxenmq::AuthLevel::none, 1 /*reserved threads*/, 500 /*max queue*/)
        .add_request_command("messages", [this](auto& m) { handle_monitor_messages(m); })
        ;

    // Endpoints invokable by a local admin
    omq_.add_category("service", oxenmq::AuthLevel::admin)
        .add_request_command("get_stats", [this](auto& m) { handle_get_stats(m); })
        ;

    // We send a sub.block to oxend to tell it to push new block notifications to us via this
    // endpoint:
    omq_.add_category("notify", oxenmq::AuthLevel::admin)
        .add_request_command("block", [this](auto&&) {
            log::debug(logcat, "Received new block notification from oxend, updating swarms");
            if (service_node_) service_node_->update_swarms();
        });

    // clang-format on
    omq_.set_general_threads(1);

    omq_.MAX_MSG_SIZE =
            10 * 1024 * 1024;  // 10 MB (needed by the fileserver, and swarm msg serialization)

    // Be explicit about wanting per-SN unique connection IDs:
    omq_.EPHEMERAL_ROUTING_ID = false;
}

void OMQ::connect_oxend(const oxenmq::address& oxend_rpc) {
    // Establish our persistent connection to oxend.
    auto start = std::chrono::steady_clock::now();
    while (true) {
        std::promise<bool> prom;
        log::info(logcat, "Establishing connection to oxend...");
        omq_.connect_remote(
                oxend_rpc,
                [this, &prom](auto cid) {
                    oxend_conn_ = cid;
                    prom.set_value(true);
                },
                [&prom, &oxend_rpc](auto&&, std::string_view reason) {
                    log::warning(
                            logcat,
                            "failed to connect to local oxend @ {}: {}; retrying",
                            oxend_rpc.full_address(),
                            reason);
                    prom.set_value(false);
                },
                // Turn this off since we are using oxenmq's own key and don't want to replace some
                // existing connection to it that might also be using that pubkey:
                oxenmq::connect_option::ephemeral_routing_id{},
                oxenmq::AuthLevel::admin);

        if (prom.get_future().get()) {
            log::info(
                    logcat,
                    "Connected to oxend in {}",
                    util::short_duration(std::chrono::steady_clock::now() - start));
            break;
        }
        std::this_thread::sleep_for(500ms);
    }
}

void OMQ::init(
        snode::ServiceNode* sn,
        rpc::RequestHandler* rh,
        rpc::RateLimiter* rl,
        oxenmq::address oxend_rpc) {
    // Initialization happens in 3 steps:
    // - connect to oxend
    // - get initial block update from oxend
    // - start OMQ/QUIC/HTTPS listeners
    assert(!service_node_);
    service_node_ = sn;
    request_handler_ = rh;
    rate_limiter_ = rl;
    omq_.start();
    // Block until we are connected to oxend:
    connect_oxend(oxend_rpc);

    // Block until we get a block update from oxend:
    service_node_->on_oxend_connected();

    // start omq listener
    const auto& me = service_node_->own_address();
    log::info(logcat, "Starting listening for OxenMQ connections on port {}", me.omq_quic_port);
    auto omq_prom = std::make_shared<std::promise<void>>();
    auto omq_future = omq_prom->get_future();
    omq_.listen_curve(
            fmt::format("tcp://0.0.0.0:{}", me.omq_quic_port),
            [this](std::string_view /*addr*/, std::string_view pk, bool /*sn*/) {
                return stats_access_keys_.count(std::string{pk}) ? oxenmq::AuthLevel::admin
                                                                 : oxenmq::AuthLevel::none;
            },
            [prom = std::move(omq_prom)](bool listen_success) {
                if (listen_success)
                    prom->set_value();
                else {
                    try {
                        throw std::runtime_error{""};
                    } catch (...) {
                        prom->set_exception(std::current_exception());
                    }
                }
            });
    try {
        omq_future.get();
    } catch (const std::runtime_error&) {
        auto msg = fmt::format("OxenMQ server failed to bind to port {}", me.omq_quic_port);
        log::critical(logcat, "{}", msg);
        throw std::runtime_error{msg};
    }

    // The https server startup happens in main(), after we return
}

std::string OMQ::encode_onion_data(
        std::string_view payload, const rpc::OnionRequestMetadata& data) {
    return oxenc::bt_serialize<oxenc::bt_dict>({
            {"data", payload},
            {"enc_type", to_string(data.enc_type)},
            {"ephemeral_key", data.ephem_key.view()},
            {"hop_no", data.hop_no},
    });
}

std::pair<std::string_view, rpc::OnionRequestMetadata> OMQ::decode_onion_data(
        std::string_view data) {
    // NB: stream parsing here is alphabetical (that's also why these keys *aren't* constexprs:
    // that would potentially be error-prone if someone changed them without noticing the sort
    // order requirements).
    std::pair<std::string_view, rpc::OnionRequestMetadata> result;
    auto& [payload, meta] = result;
    oxenc::bt_dict_consumer d{data};
    if (!d.skip_until("data"))
        throw std::runtime_error{"required data payload not found"};
    payload = d.consume_string_view();

    if (d.skip_until("enc_type"))
        meta.enc_type = crypto::parse_enc_type(d.consume_string_view());
    else
        meta.enc_type = crypto::EncryptType::aes_gcm;

    if (!d.skip_until("ephemeral_key"))
        throw std::runtime_error{"ephemeral key not found"};
    meta.ephem_key = crypto::x25519_pubkey::from_bytes(d.consume_string_view());

    if (d.skip_until("hop_no"))
        meta.hop_no = d.consume_integer<int>();
    if (meta.hop_no < 1)
        meta.hop_no = 1;

    return result;
}

void OMQ::handle_monitor_messages(oxenmq::Message& message) {
    // If not a single part then send an empty string so that the base class fires back an error for
    // us:
    std::string_view request = message.data.size() != 1 ? ""sv : message.data[0];

    handle_monitor(
            request,
            [&message](std::string body) { message.send_reply(std::move(body)); },
            message.conn);
}

void OMQ::notify(std::vector<connection_id>& conns, std::string_view notification) {
    for (const auto& c : conns)
        if (auto* id = std::get_if<oxenmq::ConnectionID>(&c))
            omq_.send(*id, "notify.message", notification);
}

void OMQ::reachability_test(std::shared_ptr<snode::sn_test> test) {
    auto xpk = test->sn.pubkey_x25519.view();
    omq_.request(
            xpk,
            "sn.ping",
            [test = std::move(test)](bool success, const auto&) {
                log::debug(
                        logcat,
                        "{} response for OxenMQ ping test of {}",
                        success ? "Successful" : "FAILED",
                        test->sn.pubkey_legacy);

                test->add_result(success);
            },
            // Only use an existing (or new) outgoing connection:
            oxenmq::send_option::outgoing{},
            oxenmq::send_option::request_timeout{snode::SN_PING_TIMEOUT});
}

}  // namespace oxenss::server

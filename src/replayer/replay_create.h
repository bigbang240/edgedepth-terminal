#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// replay_create.h - creating a generic replay session in one message
//
// Task replay-session-create-and-join (2026-10-13). A generic replay used to be
// two requests: POST /replay/session for a session id, then join_replay with
// that id on the replay socket. The POST existed only to move the id, and from
// Auckland it cost 1.45 s before the socket could ask for anything (a CORS
// preflight that opened Cloudflare's connection to the origin, then the POST).
// A box with create-and-join takes the POST body inside join_replay and
// creates the row itself, verifying the same token the same way.
//
// An older box ignores the body and answers with one of the two errors its
// join path raises first; the client then posts and joins as before, so either
// deploy order works. Archive (event) replays keep their POST: they are served
// by hub over the live socket and are not part of this change.
//
// Pure and Emscripten-free so tests/native can exercise it.
// ═══════════════════════════════════════════════════════════════════════════════

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace replay_create {

enum class Path { OneMessage, PostThenJoin };

// The host page can force the old path with "post" (window global
// __EDGEDEPTH_REPLAY_CREATE__ or ?replay_create=post), which is the rollback
// that needs no rebuild. Anything else, including nothing, is one message.
inline Path path_from_choice(const std::string& choice) {
    return choice == "post" ? Path::PostThenJoin : Path::OneMessage;
}

// The join_replay frame that creates and joins. `create` is the exact POST
// /replay/session body, so the box validates the same document either way.
// A positive preload asks the box to send the chart's first history page
// (the get_historical_candles the replay context would otherwise send after
// replay_joined, a round trip later) right behind replay_joined.
inline std::string join_message(const nlohmann::json& create, const std::string& token,
                                int64_t preload_timeframe_s = 0, int64_t preload_count = 0) {
    nlohmann::json data = {{"create", create}};
    if (!token.empty()) data["entitlement_token"] = token;
    if (preload_timeframe_s > 0 && preload_count > 0)
        data["preload_candles"] = {{"timeframe", preload_timeframe_s}, {"count", preload_count}};
    return nlohmann::json{{"method", "join_replay"}, {"data", std::move(data)}}.dump();
}

// An error frame that means "this box does not know create-and-join": its join
// path verifies the token and then needs a session id, and says so in exactly
// these words. A box that does know it reports refusals as replay_create_error
// instead, so neither string can come from a refused create on a new box.
inline bool box_lacks_create_and_join(const std::string& error_text) {
    return error_text == "Missing session_id" || error_text == "Replay authorization failed";
}

// replay_create_error carries the code, message and HTTP status the POST would
// have answered with. on_session_error takes (status, body) in the POST's shape.
struct Refusal {
    int status = 0;
    std::string body;
};

inline Refusal refusal_from(const nlohmann::json& data) {
    Refusal r;
    if (!data.is_object()) return r;
    if (data.contains("status") && data["status"].is_number_integer()) r.status = data["status"].get<int>();
    nlohmann::json body = nlohmann::json::object();
    if (data.contains("error") && data["error"].is_string()) body["error"] = data["error"];
    if (data.contains("code") && data["code"].is_string()) body["code"] = data["code"];
    r.body = body.dump();
    return r;
}

// The POST waited up to two seconds for the host to publish the entitlement
// token, then sent anyway so the box answers with a coded error rather than a
// spinner that never resolves. The one-message path waits the same way.
constexpr int64_t TOKEN_WAIT_MAX_MS = 2000;

inline bool should_wait_for_token(bool have_token, int64_t waited_ms) {
    return !have_token && waited_ms < TOKEN_WAIT_MAX_MS;
}

}  // namespace replay_create

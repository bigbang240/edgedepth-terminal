// replay_create.h: the one-message create-and-join contract the replay manager
// sends, and the replies that decide between it and the old POST then join.
#include "replayer/replay_create.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using nlohmann::json;
using namespace replay_create;

int main() {
    // The host's rollback switch: only "post" selects the old path.
    CHECK(path_from_choice("") == Path::OneMessage);
    CHECK(path_from_choice("join") == Path::OneMessage);
    CHECK(path_from_choice("post") == Path::PostThenJoin);

    // The frame carries the POST body verbatim under "create", plus the token.
    const json create = {{"symbols", {"btcusdt"}}, {"exchange", "binancef"},
                         {"start_timestamp", 1000}, {"end_timestamp", 2000},
                         {"anchor_timestamp", 1500}, {"speed", 1.0}};
    const json frame = json::parse(join_message(create, "tok"));
    CHECK(frame["method"] == "join_replay");
    CHECK(frame["data"]["create"] == create);
    CHECK(frame["data"]["entitlement_token"] == "tok");
    CHECK(!frame["data"].contains("session_id"));
    CHECK(!frame["data"].contains("preload_candles"));
    // The first chart page rides along when asked for.
    const json preload = json::parse(join_message(create, "tok", 300, 720));
    CHECK(preload["data"]["preload_candles"]["timeframe"] == 300);
    CHECK(preload["data"]["preload_candles"]["count"] == 720);
    CHECK(!json::parse(join_message(create, "tok", 0, 720))["data"].contains("preload_candles"));
    // No token yet: the box must see none at all, and answer AUTH_REQUIRED.
    CHECK(!json::parse(join_message(create, ""))["data"].contains("entitlement_token"));

    // An old box's join path: these two answers, and only these, mean fall back.
    CHECK(box_lacks_create_and_join("Missing session_id"));
    CHECK(box_lacks_create_and_join("Replay authorization failed"));
    CHECK(!box_lacks_create_and_join("Replay already active on this connection"));
    CHECK(!box_lacks_create_and_join("Session already active"));
    CHECK(!box_lacks_create_and_join(""));

    // A refusal becomes the (status, body) the POST error path already maps.
    const Refusal tier = refusal_from(json{{"code", "TIER_DAILY"}, {"error", "daily replay limit reached"}, {"status", 429}});
    CHECK(tier.status == 429);
    const json body = json::parse(tier.body);
    CHECK(body["code"] == "TIER_DAILY");
    CHECK(body["error"] == "daily replay limit reached");
    const Refusal plain = refusal_from(json{{"error", "invalid create payload"}, {"status", 400}});
    CHECK(plain.status == 400 && !json::parse(plain.body).contains("code"));
    const Refusal junk = refusal_from(json("nope"));
    CHECK(junk.status == 0 && junk.body.empty());

    // The token wait is the POST's: up to two seconds, then send regardless.
    CHECK(should_wait_for_token(false, 0));
    CHECK(should_wait_for_token(false, TOKEN_WAIT_MAX_MS - 1));
    CHECK(!should_wait_for_token(false, TOKEN_WAIT_MAX_MS));
    CHECK(!should_wait_for_token(true, 0));

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("replay_create_test: ok");
    return EXIT_SUCCESS;
}

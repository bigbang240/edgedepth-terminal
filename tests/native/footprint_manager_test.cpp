#include "core/footprint_manager.cpp"
#include <cstdio>
#include <initializer_list>

using Manager = FootprintManager;
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
struct Row { double price, buy, sell; };
Manager::CandleFootprint candle(int64_t start, std::initializer_list<Row> rows) {
    Manager::CandleFootprint fp;
    fp.start_time = start;
    fp.end_time = start + 60000;
    for (auto r : rows) {
        fp.levels.push_back({r.price, r.buy, r.sell, r.buy+r.sell, 1, r.buy-r.sell});
        fp.total_buy += r.buy;
        fp.total_sell += r.sell;
        fp.high_price = std::max(fp.high_price, r.price);
        if (fp.low_price == 0 || r.price < fp.low_price) fp.low_price = r.price;
    }
    fp.total_volume = fp.total_buy + fp.total_sell;
    fp.delta = fp.total_buy - fp.total_sell;
    return fp;
}
int main() {
    Manager m;
    auto fp = candle(60000, {{100, 1, 10}, {101, 30, 100}, {102, 1, 1}});
    auto rows = m.group_levels(fp, 1);
    expect(rows[1].sell_imbalance && !rows[1].buy_imbalance, "default same-price compares 100 sells with 30 buys");
    m.comparison = Manager::Comparison::Diagonal;
    rows = m.group_levels(fp, 1);
    expect(rows[1].buy_imbalance && rows[1].sell_imbalance, "diagonal row can qualify both sides: 30/10 and 100/1");
    expect(!rows.front().buy_imbalance && !rows.back().sell_imbalance, "edges do not invent absent opponents");
    m.imbalance_ratio = 3.01f;
    rows = m.group_levels(fp, 1);
    expect(!rows[1].buy_imbalance, "ratio just above equality excludes buy");
    m.imbalance_ratio = 3;
    m.imbalance_min_volume = 30;
    expect(m.group_levels(fp, 1)[1].buy_imbalance, "minimum numerator volume inclusive");
    m.imbalance_min_volume = 30.01;
    expect(!m.group_levels(fp, 1)[1].buy_imbalance, "minimum numerator volume excludes dust");
    m.imbalance_min_volume = 0;
    fp = candle(60000, {{100, 0, 0}, {101, 100, 0}, {103, 100, 1}});
    rows = m.group_levels(fp, 1);
    expect(rows.size() == 2 && !rows[0].buy_imbalance && !rows[1].buy_imbalance,
        "zero-volume and absent rows cannot supply a diagonal denominator");
    fp = candle(60000, {{100, 1, 0}, {101, 100, 1}});
    expect(!m.group_levels(fp, 1)[1].buy_imbalance, "explicit zero denominator never qualifies");

    m.stacked_levels = 3;
    fp = candle(60000, {{100, 10, 1}, {101, 10, 1}, {102, 10, 1},
        {103, 10, 1}, {105, 10, 1}, {106, 10, 1}, {107, 10, 1}});
    rows = m.group_levels(fp, 1);
    expect(!rows[0].buy_stack && rows[1].buy_stack && rows[2].buy_stack && rows[3].buy_stack,
        "three adjacent diagonal buys mark the entire run");
    expect(!rows[4].buy_stack && !rows[5].buy_stack && !rows[6].buy_stack,
        "gap breaks stack and following run is too short");
    for (auto& r : fp.levels) { std::swap(r.buy_volume, r.sell_volume); r.delta = -r.delta; }
    rows = m.group_levels(fp, 1);
    expect(rows[0].sell_stack && rows[1].sell_stack && rows[2].sell_stack && !rows[3].sell_stack,
        "mirrored sells compare upward and stack down to bottom edge");
    m.comparison = Manager::Comparison::SamePrice;
    rows = m.group_levels(fp, 1);
    expect(rows[4].sell_stack && rows[6].sell_stack, "same-price stacks also work independently after gap");
    fp = candle(60000, {{0.2, 10, 1}, {0.3, 10, 1}, {0.4, 10, 1}});
    rows = m.group_levels(fp, 0.1);
    expect(rows.size() == 3 && rows[0].buy_stack && rows[2].buy_stack,
        "decimal tick boundaries remain three contiguous prices");
    rows = m.group_levels(fp, 0.2);
    expect(rows.size() == 2 && !rows[0].buy_stack && rows[0].buy_volume == 20,
        "regrouping aggregates before detecting stacks");

    m = Manager{};
    m.store_footprint("A", candle(60000, {{100, 30, 10}}));
    m.store_footprint("A", candle(120000, {{101, 3000, 1}}));
    auto read = [&](int64_t tf, double tick, int64_t now) {
        return m.get_merged_grouped("A", 60000, tf, tick, now);
    };
    expect(!read(60, 1, 119999), "future completed-minute history withheld before end");
    const auto* cache = read(60, 1, 120000);
    expect(cache && cache->total_buy == 30 && !cache->provisional, "minute becomes available exactly at end");
    const auto* level_storage = cache->levels.data();
    const auto* stable = read(60, 1, 120001);
    expect(stable == cache && stable->levels.data() == level_storage, "unchanged data reuses cache");
    cache = read(120, 1, 150000);
    expect(cache && cache->total_buy == 30 && cache->provisional, "forming higher timeframe only uses closed minutes");
    cache = read(120, 1, 180000);
    expect(cache && cache->total_buy == 3030 && !cache->provisional, "clock crossing includes newly eligible minute");
    cache = read(60, 1, 180000);
    expect(cache && cache->total_buy == 30, "timeframe change invalidates merged cache");
    m.imbalance_ratio = 4;
    expect(!read(60, 1, 180000)->levels[0].buy_imbalance, "ratio changes invalidate cache without data update");
    m.imbalance_ratio = 3;
    expect(read(60, 1, 180000)->levels[0].buy_imbalance, "ratio change restores threshold equality");
    m.imbalance_min_volume = 31;
    expect(!read(60, 1, 180000)->levels[0].buy_imbalance, "minimum volume invalidates cache");
    m.imbalance_min_volume = 0;
    m.comparison = Manager::Comparison::Diagonal;
    expect(!read(60, 1, 180000)->levels[0].buy_imbalance, "comparison mode invalidates cache");
    m.comparison = Manager::Comparison::SamePrice;
    m.stacked_levels = 2;
    expect(read(120, 1, 180000)->levels[0].buy_stack, "stack setting invalidates cache");
    m.stacked_levels = 3;
    expect(!read(120, 1, 180000)->levels[0].buy_stack, "longer stack setting clears old flags");
    expect(read(120, 2, 180000)->levels.size() == 1, "tick grouping invalidates cache");
    cache = read(120, 1, 150000);
    expect(cache && cache->total_buy == 30, "rewind removes future volume already stored and cached");
    expect(!read(120, 1, 119999), "rewind before first close removes all footprint evidence");
    expect(!read(15, 1, 180000), "one minute snapshot cannot masquerade as subminute volume");
    m.store_footprint("B", candle(60000, {{100, 7, 1}}));
    expect(m.get_merged_grouped("B", 60000, 60, 1, 120000)->total_buy == 7,
        "symbols keep separate footprints");
    m.store_footprint("A", candle(60000, {{100, 4, 10}}));
    expect(read(60, 1, 120000)->total_buy == 4, "updated snapshot invalidates old analysis");
    auto invalid = candle(60000, {{100, 9000, 1}}); invalid.end_time += 60000;
    m.store_footprint("A", invalid);
    expect(read(60, 1, 120000)->total_buy == 4, "other resolution cannot overwrite minute source");
    const auto version = m.data_version();
    m.clear("A");
    expect(m.has_new_data_since(version) && !read(60, 1, 180000), "clear invalidates data and analysis");
    expect(m.get_merged_grouped("B", 60000, 60, 1, 120000), "clearing one symbol preserves another");
    Manager live;
    const auto market = Manager::market_key("binancef", "BTC");
    const auto other = Manager::market_key("hl", "BTC");
    auto observed = [&](int64_t as_of, int64_t tf = 60) {
        return live.get_merged_grouped(market, 60000, tf, 1, as_of);
    };
    live.on_trade(market, 61000, 100, 2, true);
    auto current = observed(61000);
    expect(current && current->total_buy == 2 && current->observed_trades,
           "forming 1m footprint includes observed trades before minute close");
    live.on_trade(market, 62000, 100, 3, false);
    current = observed(62000);
    expect(current && current->total_volume == 5 && current->delta == -1,
           "next trade updates the existing cached footprint and side totals");
    expect(!observed(61500), "aggregated provisional volume never leaks a later trade into an earlier read");
    live.on_trade(other, 63000, 100, 99, true);
    expect(observed(63000)->total_volume == 5, "venues cannot mix provisional volume");
    live.on_trade(market, 121000, 101, 7, true);
    current = observed(121000, 300);
    expect(current && current->total_volume == 12 && current->observed_trades,
           "forming 5m candle merges observed minutes through rollover");
    live.store_footprint(market, candle(60000, {{100, 20, 30}}));
    current = observed(121000, 300);
    expect(current && current->total_volume == 57 && current->observed_trades,
           "authoritative close replaces observed volume without double counting");
    live.on_trade(market, 62000, 100, 999, true);
    expect(observed(121000)->total_volume == 50 && !observed(121000)->observed_trades,
           "late trade cannot alter authoritative closed minute");
    live.store_footprint(market, candle(120000, {{101, 9, 1}}));
    expect(observed(180000, 300)->total_volume == 60 && !observed(180000, 300)->observed_trades,
           "all authoritative minutes remove observed marker");
    live.on_trade(market, 900001, 100, 1, true);
    expect(!observed(119000), "old provisional minutes expire instead of accumulating forever");
    live.clear(market);
    expect(!observed(180000, 300), "clear removes both historical and observed volume");

    // History requests: covered only on a successful reply, retried after a
    // failure, a refusal or no reply, one request in flight per market.
    Manager h;
    const int64_t retry = Manager::kHistoryRetryMinMs, timeout = Manager::kHistoryReplyTimeoutMs;
    int64_t s = 0, e = 0;
    auto begin = [&](int64_t start, int64_t end, int64_t now, const std::string& key) {
        s = start; e = end;
        return h.begin_history_request(key, s, e, now);
    };
    auto ask = [&](int64_t start, int64_t end, int64_t now) { return begin(start, end, now, market); };
    expect(ask(1000, 2000, 0) && h.is_loading(market), "first request goes out");
    expect(!ask(3000, 4000, 1), "one request in flight per market");
    expect(begin(1000, 2000, 1, other) && h.is_loading(other), "a pending market does not block another");
    h.finish_history_request(market, 0, 0, false, 10);
    expect(!h.is_loading(market) && !ask(1200, 1800, 11), "legacy sentinel completes and covers the range");
    expect(ask(500, 1500, 12) && s == 500 && e == 2000, "extension asks for the union");
    h.finish_history_request(market, 500, 2000, true, 20);
    expect(!h.is_loading(market), "failure sentinel clears loading");
    expect(!ask(500, 1500, 20 + retry - 1), "failed range waits for the backoff");
    expect(!ask(1000, 2000, 20 + retry) && !h.is_loading(market), "coverage from before the failure stays");
    expect(ask(500, 1500, 20 + retry) && s == 500 && e == 2000, "failed range is never marked covered and is asked again");
    h.finish_history_request(market, 500, 2000, true, 100);
    expect(!ask(500, 1500, 100 + 2 * retry - 1) && ask(500, 1500, 100 + 2 * retry),
           "backoff doubles per consecutive failure");
    h.finish_history_request(market, 9, 99, false, 7000);
    expect(h.is_loading(market), "a reply echoing another range does not answer the request");
    h.finish_history_request(market, 500, 2000, false, 7000);
    expect(!h.is_loading(market) && !ask(600, 1900, 7001), "echoed success covers the range and resets the backoff");

    expect(ask(600, 2500, 8000) && s == 500 && e == 2500, "panning past coverage asks for the union");
    expect(!ask(600, 2500, 8000 + timeout - 1) && h.is_loading(market), "no reply: in flight until the timeout");
    expect(!ask(600, 2500, 8000 + timeout) && !h.is_loading(market),
           "no reply within the timeout counts as a failure and backs off");
    h.finish_history_request(market, 500, 2500, true, 8000 + timeout + 1);
    int64_t t = 8000 + timeout + retry;
    expect(!ask(1000, 2000, t), "a timed-out extension leaves earlier coverage intact");
    expect(ask(600, 2500, t) && s == 500 && e == 2500, "a late failure adds no backoff; the timed-out range is asked again");
    expect(!ask(600, 2500, t + timeout), "second timeout");
    h.finish_history_request(market, 500, 2500, false, t + timeout + 5);
    expect(!ask(500, 2500, t + timeout + 6), "a reply that outlived the timeout still covers its range");
    expect(ask(400, 2500, t + timeout + 6), "and clears the backoff");
    h.finish_history_request(market, 400, 2500, false, t + timeout + 7);

    for (int i = 0; i < 20; ++i) {
        const int64_t now = 1000000LL * (i + 1);
        if (ask(3000 + i, 4000, now)) h.finish_history_request(market, s, e, true, now);
    }
    t = 20000000;
    expect(!ask(3000, 4000, t + Manager::kHistoryRetryMaxMs - 1) && ask(3000, 4000, t + Manager::kHistoryRetryMaxMs),
           "backoff is capped");
    h.clear(market);
    expect(!h.is_loading(market) && ask(1000, 2000, t + Manager::kHistoryRetryMaxMs), "clear forgets coverage and the request");
    h.finish_history_request(":", 0, 0, true, 0);
    expect(h.is_loading(market), "a sentinel for an unknown market changes nothing");
    std::printf("Footprint direction, thresholds, adjacency, cache, as-of and history requests: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}

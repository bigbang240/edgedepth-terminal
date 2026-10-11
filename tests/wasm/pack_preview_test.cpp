#include "replayer/pack_preview.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
static void check(bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); } }
int main() {
    pb::PackHeader header;
    header.set_format_version(2); header.set_exchange("binancef"); header.set_symbol("tutusdt"); header.set_event_id("event");
    header.set_start_ts_ms(120000); header.set_end_ts_ms(265000);
    pb::Candles candles; candles.set_timeframe(60);
    for (int64_t ts : {60000, 120000, 180000, 240000}) {
        auto* c = candles.add_values(); c->set_timestamp_ms(ts);
        c->set_open(10); c->set_high(12); c->set_low(9); c->set_close(11);
    }
    auto* seed = header.add_candle_seeds(); seed->set_timeframe_sec(60);
    auto decode = [&] { seed->set_candles(candles.SerializeAsString()); auto raw = header.SerializeAsString();
        return PackPreview::decode(raw.data(), raw.size(), "binancef", "tutusdt", "event"); };
    auto preview = decode();
    check(preview.bars.size() == 2 && preview.bars.front().time_ms == 120000 && preview.bars.back().time_ms == 180000,
          "must exclude pre-roll and partial final minute");
    header.set_symbol("wrong"); check(decode().bars.empty(), "wrong symbol accepted"); header.set_symbol("tutusdt");
    header.set_exchange("bybit"); check(decode().bars.empty(), "wrong venue accepted"); header.set_exchange("binancef");
    header.set_event_id("other"); check(decode().bars.empty(), "wrong event accepted"); header.set_event_id("event");
    candles.mutable_values(2)->set_high(std::numeric_limits<double>::quiet_NaN());
    check(decode().bars.empty(), "nonfinite price accepted"); candles.mutable_values(2)->set_high(12);
    candles.mutable_values(2)->set_low(11); check(decode().bars.empty(), "inverted OHLC accepted"); candles.mutable_values(2)->set_low(9);
    candles.mutable_values(2)->set_timestamp_ms(120000); check(decode().bars.empty(), "duplicate time accepted");
    check(PackPreview::decode("bad", 3, "binancef", "tutusdt", "event").bars.empty(), "malformed protobuf accepted");
    check(PackPreview::decode("bad", PackPreview::max_header_bytes+1, "x", "y", "").bars.empty(), "oversized input accepted");
    std::puts("PASS pack preview identity, window and data validation");
}

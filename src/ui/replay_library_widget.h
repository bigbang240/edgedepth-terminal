#pragma once

#include "core/app_context.h"
#include "ui/widget.h"
#include "replayer/pack_preview.h"

#include <cstdint>
#include <string>
#include <vector>

// Public, manifest-driven .edpack catalog. Packs are static objects and replay
// entirely through PackReplayEngine, so this widget needs no account, backend
// replay session, or additional data path.
class ReplayLibraryWidget final : public Widget {
public:
    explicit ReplayLibraryWidget(const AppContext& ctx);
    ~ReplayLibraryWidget() override;

    void render() override;
    void update() override;

    [[nodiscard]] WidgetType type() const override {
        return WidgetType::ReplayLibrary;
    }
    [[nodiscard]] const char* title() const override {
        return "Replay Library###replay_library";
    }
    [[nodiscard]] UpdateFrequency update_frequency() const override {
        return UpdateFrequency::Slow;
    }

private:
    struct Pack {
        std::string id;
        std::string title;
        std::string summary;
        std::string exchange;
        std::string symbol;
        std::string display_symbol;
        std::string captured_at;
        std::string pack_url;
        std::string exchange_id, event_id, fingerprint;
        std::string base_asset;      // "TUT" from "TUT/USDT", for the coin mark
        std::string metadata_line;   // "9 Aug 2026 · 06:50 UTC · 27m · 74.8 MB"
        std::vector<std::string> tags;  // shown as chips, words not hyphens
        int64_t duration_seconds = 0;
        int64_t size_bytes = 0;
        bool featured = false;
        bool details_open = false, preview_requested = false, preview_done = false;
        PackPreview::Chart preview;
    };

    const AppContext& ctx_;
    std::vector<Pack> packs_;
    std::string manifest_url_;
    std::string error_;
    std::string notice_;
    double notice_until_ = 0.0;
    int manifest_revision_ = -1;
    int pending_pack_ = -1;
    int preview_request_ = 0, preview_pack_ = -1;
    bool loading_ = false;
    bool catalog_ready_ = false;
    bool replace_popup_requested_ = false;
    int hovered_pack_ = -1;  // the card under the pointer last frame (its hairline brightens)

    // ── Event finder (roadmap Phase 4) ────────────────────────────────────────
    // Searches the self-hosted recorder daemon's /events feed (same server as
    // the catalog manifest) and can replay any hit straight into its pack.
    struct EventHit {
        int64_t ts_ms = 0, end_ms = 0;
        int kind = 0;               // 0 cascade · 1 liquidation · 2 big fill · 3 volume spike
        int side = 0;               // 0 longs/forced-sell · 1 shorts/forced-buy (trades: taker side)
        double usd = 0.0, price = 0.0;
        int64_t count = 0;          // cascade chain size
        bool live = false;          // inside the still-open episode (no pack yet)
        std::string detail, pack_file, symbol;
        int pack_index = -1;        // resolved against the loaded catalog (-1 = pruned)
    };
    std::vector<EventHit> event_hits_;
    std::string event_error_;
    int event_kind_ = 0;            // 0 all · 1 cascades · 2 liquidations · 3 big fills · 4 vol spikes
    double event_min_usd_ = 500000.0;
    char event_symbol_[24] = "";    // empty = every recorded symbol
    int event_revision_ = -1;       // last feed revision applied
    bool event_requested_ = false;
    int pending_event_pack_ = -1;   // pack to open for the pending event replay
    int64_t pending_event_seek_ms_ = 0; // consumed when the replace popup confirms

    void begin_event_fetch();
    void apply_events(const std::string& payload);
    void render_event_finder();
    void play_event(int hit_index);
    // "https://h/manifest.json?XTP=9002" + "events" → absolute URL pieces the
    // JS fetch helper combines (the routing query rides along).
    void split_manifest_url(std::string& base, std::string& query) const;
    std::string event_endpoint_url(const char* endpoint) const;

    void begin_fetch(bool force);
    bool parse_manifest(const std::string& payload, std::string& parse_error);
    void play_pack(const Pack& pack, int64_t seek_ms = 0);
    void render_pack(Pack& pack, int index);
    // Draws the candle preview; returns true when it was clicked (it doubles as a play button).
    bool render_preview(Pack& pack, ImVec2 size);
    void request_play(int index);
    void update_preview();
    void render_replace_confirmation();

    static bool is_http_url(const std::string& value);
    static std::string format_duration(int64_t seconds);
    static std::string format_size(int64_t bytes);
};

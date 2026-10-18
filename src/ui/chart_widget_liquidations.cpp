#include "ui/chart_widget.h"
#include "core/candle_manager.h"
#include "core/liquidation_heatmap_manager.h"
#include "core/display_time_zone.h"
#include "core/entitlements.h"
#include "core/education_boot.h"
#include "replayer/replay_manager.h"
#include "stream_handler.h"
#include "rendering/observed_liquidations.h"
#include "core/liqmap2_store.h"
#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"
#include "ui/indicators/volume_indicator.h"
#include <emscripten.h>
#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <cmath>
#include <cstdio>

bool ChartWidget::liquidation_activity_visible() const {
    return reported_liquidations_enabled() && (rt_mode_ ? rt_liq_strip_ : candle_liq_strip_ && ct_allows_time_overlays(chart_type_));
}
// Candle charts: the reported-liquidation row's settings in Layers, under the row while it is on. The everyday
// controls first (callouts and their threshold, the activity subplot); the rest in a collapsed Options group.
void ChartWidget::render_liquidation_settings() {
    if(Theme::menu_toggle("Callouts on candles",liq_markers_))liq_markers_=!liq_markers_;
    if(ImGui::IsItemHovered())Theme::tooltip("A $ pill at the wick of candles whose liquidations were unusual for this market:\nshorts above the high, longs below the low. Every candle stays in the activity subplot.");
    if(liq_markers_){
        static const char* const kAt[]={"1.5\xc3\x97","2\xc3\x97","3\xc3\x97","5\xc3\x97"};
        static constexpr float kAtX[]={1.5f,2.0f,3.0f,5.0f};
        int at=0;
        for(int i=1;i<4;++i)if(std::abs(kAtX[i]-liq_callout_x_)<std::abs(kAtX[at]-liq_callout_x_))at=i;
        const int next=Theme::menu_choice_row("Callout at","liq_callout_at",kAt,4,at);
        if(next!=at)liq_callout_x_=kAtX[next];
        if(ImGui::IsItemHovered())Theme::tooltip("Times this market's typical candle: the mean long or short total per candle\nover its last 200 candles.");
    }
    if(Theme::menu_toggle("Activity subplot",candle_liq_strip_))candle_liq_strip_=!candle_liq_strip_;
    if(Theme::begin_menu_group("Options##reported")){
        if(Theme::menu_toggle("Price profile",liq_report_profile_))liq_report_profile_=!liq_report_profile_;
        if(ImGui::IsItemHovered())Theme::tooltip(observed_liquidations::bankruptcy_prices(pair_.exchange)
            ?"Reported dollars by price over the visible time, at the right edge.\nBybit prices are bankruptcy prices, not where the orders traded."
            :"Reported dollars by price over the visible time, at the right edge.");
        if(Theme::menu_field("Ignore reports under $",&liq_obs_min_usd_)){
            if(!std::isfinite(liq_obs_min_usd_))liq_obs_min_usd_=0;
            liq_obs_min_usd_=std::clamp(liq_obs_min_usd_,0.0f,1e15f);
        }
        if(ImGui::IsItemHovered())Theme::tooltip("Callouts only. The subplot and profile keep every report.");
        if(Theme::menu_item(drawing::UiIcon::Refresh,"Reload recorded reports",nullptr,true,true))
            ctx_.liq_heatmap_mgr().reported_history(pair_).reload();
        Theme::menu_note(observed_liquidations::feed_line(pair_.exchange));
        Theme::end_menu_group();
    }
}
void ChartWidget::prepare_liquidations() {
    const auto limits=ImPlot::GetPlotLimits();
    const int64_t now=int64_t(emscripten_date_now());
    const bool replay=ctx_.replay_mgr().is_active();
    liq_cutoff_=rt_mode_?rt_clock_ms_:replay?ctx_.replay_mgr().interpolated_time_ms():now;
    const int64_t shift=rt_mode_?0:candles().timeframe_seconds()*500;
    liq_view_from_=std::max<int64_t>(1,int64_t(limits.X.Min)+shift);
    liq_view_to_=std::min(int64_t(limits.X.Max)+shift,liq_cutoff_+1);
    auto& history=ctx_.liq_heatmap_mgr().reported_history(pair_);
    if(!replay){
        history.connection(ctx_.stream_mgr().connected(),now,ctx_.stream_mgr().connection_generation());
        if(Entitlements::hosted() && Entitlements::is_pro() && !(rt_mode_ && rt_paused_) && !EducationBoot::instance().is_pack()){
            // The whole view and, on candles, the window the callouts' typical candle is measured over, planned
            // together: one walk, newest first, so the callouts settle with the view instead of after it.
            const int64_t typical_from=rt_mode_?0:liq_cutoff_-observed_liquidations::kTypicalCandles*candles().timeframe_seconds()*1000;
            auto req=history.request(pair_.exchange,pair_.symbol,liq_view_from_,liq_view_to_,now,Entitlements::history_lookback_ms(),
                                     emscripten_get_now(),typical_from,rt_mode_?0:liq_cutoff_+1);
            if(!req.empty()){
                const char* token=emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
                req["data"]["entitlement_token"]=token?token:"";
                ctx_.stream_mgr().send_message(req.dump());
            }
        }
    }
    // One short status. It reaches the chart only when it changes what the viewer sees (loading, interrupted,
    // failed); the venue's feed is described once, in the hovers (observed_liquidations::feed_line).
    const char* message="";
    bool attention=false;
    if(replay)message="Replay: reports up to the playhead";
    else if(Entitlements::hosted() && !Entitlements::is_pro())message="Live reports only; history is Pro";
    else if(rt_mode_ && rt_paused_)message="Paused";
    else if(!history.connected){message="Reconnecting; reports so far are kept";attention=true;}
    else if(!history.error.empty()){message=history.error.c_str();attention=true;}
    else if(history.loading()){message="Loading recorded reports";attention=true;}
    snprintf(liq_status_,sizeof(liq_status_),"%s",message);
    liq_status_attention_=attention;
}

void ChartWidget::render_realtime_liquidation_strip() {
    bool& collapsed=rt_mode_?rt_liq_collapsed_:candle_liq_collapsed_;
    ImGui::PushID("ReportedActivity");
    float& requested_height=rt_mode_?rt_liq_height_:candle_liq_height_;
    auto* header_draw=ImGui::GetWindowDrawList();
    if(!collapsed) {
        ImGui::InvisibleButton("##resize",ImVec2(-1,6));
        const bool active=ImGui::IsItemActive(),hovered=ImGui::IsItemHovered();
        const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        const float cy=(a.y+b.y)*0.5f,cx=(a.x+b.x)*0.5f;
        header_draw->AddLine(ImVec2(a.x,cy),ImVec2(b.x,cy),Theme::u32(Theme::Tokens::BD1));
        header_draw->AddLine(ImVec2(cx-18,cy),ImVec2(cx+18,cy),
            Theme::u32(active?Theme::Tokens::BRAND:hovered?Theme::Tokens::TX1:Theme::Tokens::TX3),2);
        if(active||hovered)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(active)requested_height=std::clamp(requested_height-ImGui::GetIO().MouseDelta.y,60.0f,600.0f);
        if(hovered)Theme::tooltip("Drag to resize liquidation pane. Double-click to reset.");
        if(hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))requested_height=100;
    }
    const auto button=ImGui::GetCursorScreenPos();
    const bool toggle=ImGui::InvisibleButton("##collapse",ImVec2(22,20));
    const bool hovered=ImGui::IsItemHovered();
    if(hovered)header_draw->AddRectFilled(button,ImVec2(button.x+22,button.y+20),Theme::u32(Theme::Tokens::HOVER),3);
    const float d=collapsed?-1.0f:1.0f,cx=button.x+11,cy=button.y+10;
    const ImVec2 chevron[]={{cx-4,cy-2*d},{cx,cy+2*d},{cx+4,cy-2*d}};
    header_draw->AddPolyline(chevron,3,Theme::u32(hovered?Theme::Tokens::TX1:Theme::Tokens::TX2),0,1.5f);
    if(hovered)Theme::tooltip(collapsed?"Expand liquidation pane":"Collapse liquidation pane");
    if(toggle)collapsed=!collapsed;
    ImGui::SameLine(0,6);ImGui::TextUnformatted("Reported liquidations");
    if(ImGui::IsItemHovered()){
        Theme::begin_tooltip();
        ImGui::Text("Reported liquidations \xc2\xb7 %s %s",liqmap2::venue_name(pair_.exchange),pair_.symbol.c_str());
        ImGui::TextDisabled("%s",observed_liquidations::feed_line(pair_.exchange));
        ImGui::TextUnformatted("Shorts liquidated (forced buys) above the line, longs below.");
        if(liq_status_[0])ImGui::TextDisabled("%s",liq_status_);
        Theme::end_tooltip();
    }
    if(collapsed){ImGui::PopID();return;}
    const auto& events=realtime_liquidations();
    const int64_t candle_ms=candles().timeframe_seconds()*1000;
    const auto view=observed_liquidations::aggregate(events,liq_view_from_,liq_view_to_,liq_cutoff_,rt_mode_?1000:candle_ms);
    char shorts[32]={},longs[32]={};
    Indicators::format_volume_usdt(view.shorts,shorts,sizeof(shorts),nullptr);
    Indicators::format_volume_usdt(view.longs,longs,sizeof(longs),nullptr);
    const bool wide=ImGui::GetContentRegionAvail().x>=620;
    if(wide)ImGui::SameLine(0,12);
    ImGui::TextColored(Theme::Tokens::UP,"Shorts $%s",shorts);
    ImGui::SameLine(0,12);ImGui::TextColored(Theme::Tokens::DOWN,"Longs $%s",longs);
    if(liq_status_attention_ && wide){
        // Loading, interrupted or failed: a dot and a word, the reason in the hover.
        ImGui::SameLine(0,14);
        const auto p=ImGui::GetCursorScreenPos();
        const bool bad=!ctx_.liq_heatmap_mgr().reported_history(pair_).error.empty() ||
                       !ctx_.liq_heatmap_mgr().reported_history(pair_).connected;
        header_draw->AddCircleFilled(ImVec2(p.x+3,p.y+ImGui::GetTextLineHeight()*0.5f),3,
            Theme::u32(bad?Theme::Tokens::WARN:Theme::Tokens::TX3));
        ImGui::Dummy(ImVec2(8,ImGui::GetTextLineHeight()));ImGui::SameLine(0,5);
        ImGui::TextDisabled("%s",bad?"History interrupted":"Loading history");
        if(ImGui::IsItemHovered())Theme::tooltip("%s",liq_status_);
    } else if(view.count==0 && wide){ImGui::SameLine(0,12);ImGui::TextDisabled("No reports in view");}
    const float height=std::min(requested_height,std::max(60.0f,ImGui::GetContentRegionAvail().y-4));
    if(ImPlot::BeginPlot("##LiquidationActivity",ImVec2(-1,height),ImPlotFlags_NoTitle|ImPlotFlags_NoLegend|ImPlotFlags_NoMouseText|ImPlotFlags_NoInputs)){
        ImPlot::SetupAxis(ImAxis_X1,nullptr,ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1,nullptr,ImPlotAxisFlags_Opposite|ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1,last_visible_range_.X.Min,last_visible_range_.X.Max,ImGuiCond_Always);
        const double peak=std::max(1.0,view.peak)*1.1;
        ImPlot::SetupAxisLimits(ImAxis_Y1,-peak,peak,ImGuiCond_Always);
        setup_time_axis_ticks(last_visible_range_.X.Min,last_visible_range_.X.Max);
        ImPlot::SetupFinish();
        auto* draw=ImPlot::GetPlotDrawList();const auto pos=ImPlot::GetPlotPos(),size=ImPlot::GetPlotSize();
        crosshair_state_.last_plot_max=ImVec2(pos.x+size.x,pos.y+size.y);
        const double shift=rt_mode_?0:double(candle_ms)/2;
        // NoInputs disables ImPlot hover updates as well as pan/zoom. Keep
        // inspection active over the plot and its time-axis gutter.
        const bool hovered=ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(
            pos,ImVec2(pos.x+size.x,pos.y+size.y+24));
        const double mouse=hovered?ImPlot::GetPlotMousePos().x:crosshair_state_.plot_pos.x;
        const float zero=ImPlot::PlotToPixels(0,0).y;
        ImPlot::PushPlotClipRect();
        if(!ctx_.replay_mgr().is_active() && !(rt_mode_ && rt_paused_)) {
            const auto& history=ctx_.liq_heatmap_mgr().reported_history(pair_);
            // Time without recorded reports in hand: a dashed baseline, named only where there is room, and why in
            // its hover (still loading, before the plan's history, history needs Pro, or not loaded).
            const int64_t wall=int64_t(emscripten_date_now());
            const bool can_load=Entitlements::hosted() && Entitlements::is_pro() && !EducationBoot::instance().is_pack();
            auto gap=[&](int64_t from,int64_t to){
                if(to<=from)return;
                const float left=std::max(pos.x,ImPlot::PlotToPixels(double(from)-shift,0).x);
                const float right=std::min(pos.x+size.x,ImPlot::PlotToPixels(double(to)-shift,0).x);
                if(right<=left)return;
                const bool before=can_load && to<=wall-Entitlements::history_lookback_ms()+60000;
                const char* label=!can_load?"Live only":before?"Before your history":history.loading()?"Loading\xe2\x80\xa6":"Not loaded";
                const float y=pos.y+size.y-4;
                for(float x=left;x<right;x+=6)draw->AddLine(ImVec2(x,y),ImVec2(std::min(x+3,right),y),Theme::u32(Theme::Tokens::TX4),2);
                if(right-left>120)draw->AddText(ImVec2(left+5,y-16),Theme::u32(Theme::Tokens::TX3),label);
                if(hovered && ImGui::GetIO().MousePos.x>=left && ImGui::GetIO().MousePos.x<right && ImGui::GetIO().MousePos.y>=y-18)
                    Theme::tooltip("%s",!can_load?"Recorded history is Pro; live reports only here."
                        :before?"Before your plan's history window."
                        :history.loading()?"Loading recorded reports for this stretch."
                        :"No recorded reports loaded here. Not liquidation volume.");
            };
            int64_t cursor=liq_view_from_;
            const int64_t historical_end=history.connected?std::min(liq_view_to_,history.subscribed_at):liq_view_to_;
            for(const auto& [start,c]:history.loaded){
                if(c.end<=cursor || start>=historical_end)continue;
                gap(cursor,std::min(start,historical_end));cursor=std::max(cursor,c.end);
            }
            gap(cursor,historical_end);
        }
        draw->AddLine(ImVec2(pos.x,zero),ImVec2(pos.x+size.x,zero),Theme::u32(Theme::Tokens::BD1));
        for(size_t i=0;i<view.size;++i){
            const auto& bin=view.bins[i];
            const double from=std::max(bin.start_ms,liq_view_from_)-shift;
            const double to=std::min({bin.start_ms+view.step_ms,liq_view_to_,liq_cutoff_+1})-shift;
            if(to<=from)continue;
            const float left=ImPlot::PlotToPixels(from,0).x,right=ImPlot::PlotToPixels(to,0).x;
            const bool forming=bin.start_ms+view.step_ms>liq_cutoff_;
            if(bin.shorts>0)draw->AddRectFilled(ImVec2(left,ImPlot::PlotToPixels(0,bin.shorts).y),ImVec2(std::max(left+1,right-1),zero),Theme::u32(forming?ImVec4(Theme::Tokens::UP.x,Theme::Tokens::UP.y,Theme::Tokens::UP.z,0.55f):Theme::Tokens::UP));
            if(bin.longs>0)draw->AddRectFilled(ImVec2(left,zero),ImVec2(std::max(left+1,right-1),ImPlot::PlotToPixels(0,-bin.longs).y),Theme::u32(forming?ImVec4(Theme::Tokens::DOWN.x,Theme::Tokens::DOWN.y,Theme::Tokens::DOWN.z,0.55f):Theme::Tokens::DOWN));
            if(hovered && mouse>=bin.start_ms-shift && mouse<bin.start_ms+view.step_ms-shift && bin.count>0){
                char when[64],sh[32],lg[32];
                DisplayTimeZone::instance().format(bin.start_ms,rt_mode_?TimeZoneFormat::TimeSeconds:TimeZoneFormat::MonthDayTime,when,sizeof(when));
                observed_liquidations::hover_usd(bin.shorts,sh,sizeof(sh));
                observed_liquidations::hover_usd(bin.longs,lg,sizeof(lg));
                Theme::begin_tooltip(true);
                ImGui::TextDisabled("%s%s",when,forming?" \xc2\xb7 forming":"");
                if(bin.shorts>0)ImGui::TextColored(Theme::Tokens::UP,"Shorts liquidated  %s",sh);
                if(bin.longs>0)ImGui::TextColored(Theme::Tokens::DOWN,"Longs liquidated  %s",lg);
                ImGui::TextDisabled("%u report%s",bin.count,bin.count==1?"":"s");
                if(ImGui::GetIO().KeyAlt){
                    ImGui::TextDisabled("%s",observed_liquidations::feed_line(pair_.exchange));
                    ImGui::TextDisabled("%.3gs bucket \xc2\xb7 reported quantity x reported price",double(view.step_ms)/1000);
                }
                Theme::end_tooltip();
            }
        }
        ImPlot::PopPlotClipRect();
        if(hovered){
            crosshair_state_.is_active=true;crosshair_state_.chart_hovered=false;crosshair_state_.indicator_hovered=false;crosshair_state_.plot_pos=ImPlot::GetPlotMousePos();
            crosshair_state_.hovered_plot_min=pos;crosshair_state_.hovered_plot_max=ImVec2(pos.x+size.x,pos.y+size.y);
            crosshair_state_.hovered_y_min=0;crosshair_state_.hovered_y_max=0;
        }
        ImPlot::EndPlot();
    }
    ImGui::PopID();
}

void ChartWidget::render_liquidation_profile() {
    const auto limits=ImPlot::GetPlotLimits();
    const auto pos=ImPlot::GetPlotPos(),size=ImPlot::GetPlotSize();
    if(size.x<320 || limits.Y.Max<=limits.Y.Min)return;
    struct Row{double longs=0,shorts=0;unsigned count=0;};std::array<Row,128> rows{};
    const double tick=std::max(1e-12,double(tick_size_));
    const double step=std::max(tick,std::ceil((limits.Y.Max-limits.Y.Min)/std::clamp(double(size.y)/12,8.0,126.0)/tick)*tick);
    const double origin=std::floor(limits.Y.Min/step)*step;
    double peak=0;unsigned outside=0;
    const auto& events=realtime_liquidations();
    auto first=std::lower_bound(events.begin(),events.end(),liq_view_from_,[](const auto& e,int64_t t){return e.timestamp_ms<t;});
    for(auto it=first;it!=events.end() && it->timestamp_ms<liq_view_to_;++it){
        const double price=observed_liquidations::price(*it),n=observed_liquidations::notional(*it);
        if(n<=0)continue;
        if(price<limits.Y.Min || price>limits.Y.Max){++outside;continue;}
        const int i=int(std::floor((price-origin)/step));if(i<0 || i>=int(rows.size()))continue;
        auto& row=rows[i];if(it->is_buy)row.shorts+=n;else row.longs+=n;++row.count;peak=std::max({peak,row.shorts,row.longs});
    }
    auto* draw=ImPlot::GetPlotDrawList();const float right=pos.x+size.x-4,left=right-90;
    ImPlot::PushPlotClipRect();
    draw->AddRectFilled(ImVec2(left-4,pos.y),ImVec2(right+4,pos.y+size.y),Theme::u32(Theme::Tokens::PANEL));

    const auto mouse=ImGui::GetIO().MousePos;
    for(size_t i=0;i<rows.size();++i){const auto& row=rows[i];if(!row.count || peak<=0)continue;
        const float top=ImPlot::PlotToPixels(0,origin+(i+1)*step).y,bottom=ImPlot::PlotToPixels(0,origin+i*step).y,mid=(top+bottom)/2;

        if(row.shorts>0)draw->AddRectFilled(ImVec2(right-86*float(row.shorts/peak),top+1),ImVec2(right,mid),Theme::u32(Theme::Tokens::UP));
        if(row.longs>0)draw->AddRectFilled(ImVec2(right-86*float(row.longs/peak),mid),ImVec2(right,bottom-1),Theme::u32(Theme::Tokens::DOWN));
        if(mouse.x>=left && mouse.x<=right && mouse.y>=top && mouse.y<bottom && ImPlot::IsPlotHovered()){
            Theme::begin_tooltip();char low[40],high[40],sh[32],lg[32];fmt_.format_price(low,sizeof(low),origin+i*step);fmt_.format_price(high,sizeof(high),origin+(i+1)*step);
            observed_liquidations::hover_usd(row.shorts,sh,sizeof(sh));observed_liquidations::hover_usd(row.longs,lg,sizeof(lg));
            ImGui::TextDisabled("%s to %s \xc2\xb7 visible time",low,high);
            if(row.shorts>0)ImGui::TextColored(Theme::Tokens::UP,"Shorts liquidated  %s",sh);
            if(row.longs>0)ImGui::TextColored(Theme::Tokens::DOWN,"Longs liquidated  %s",lg);
            ImGui::TextDisabled("%u report%s%s",row.count,row.count==1?"":"s",
                observed_liquidations::bankruptcy_prices(pair_.exchange)?" \xc2\xb7 at bankruptcy prices":"");
            if(outside)ImGui::TextDisabled("%u more outside the visible prices",outside);
            Theme::end_tooltip();
        }
    }
    draw->AddText(ImVec2(left,pos.y+4),Theme::u32(Theme::Tokens::TX1),"Reported");
    draw->AddText(ImVec2(left,pos.y+18),Theme::u32(Theme::Tokens::TX2),"Visible time");
    ImPlot::PopPlotClipRect();
}

// Candle charts: a $ pill at the wick tip of each candle whose long or short liquidations were unusual for this
// market (observed_liquidations::callouts), shorts above the high, longs below the low, largest first and never
// overlapping. The candles stay clean; every candle's totals stay in the activity pane.
void ChartWidget::render_liq_callouts() {
    const auto& events=realtime_liquidations();
    const int64_t tf=std::max<int64_t>(1,candles().timeframe_seconds()*1000);
    const auto& ts=candles().timestamps();const auto& hi=candles().highs();const auto& lo=candles().lows();
    const bool building=candles().has_building_candle();
    const int64_t b_open=building?candles().building_candle().timestamp_ms:INT64_MIN;
    const auto open_of=[&](int64_t t){
        if(building && t>=b_open && t<b_open+tf)return b_open;
        auto it=std::upper_bound(ts.begin(),ts.end(),double(t));
        if(it!=ts.begin() && double(t)<*std::prev(it)+double(tf))return int64_t(*std::prev(it));
        return t/tf*tf;
    };
    const auto extremes=[&](int64_t open,double& h,double& l){
        if(building && open==b_open){h=candles().building_candle().high;l=candles().building_candle().low;return true;}
        auto it=std::lower_bound(ts.begin(),ts.end(),double(open));
        if(it==ts.end() || *it!=double(open))return false;
        const size_t i=size_t(it-ts.begin());h=hi[i];l=lo[i];return true;
    };
    auto& typ=liq_typical_;
    if(typ.data!=events.data() || typ.n!=events.size() || typ.tf!=tf || typ.cutoff_bucket!=liq_cutoff_/tf || typ.min_usd!=liq_obs_min_usd_)
        typ={events.data(),events.size(),tf,liq_cutoff_/tf,liq_obs_min_usd_,
             observed_liquidations::typical_candle(events,liq_cutoff_,tf,liq_obs_min_usd_)};
    // The reports the view holds, as callouts() reads them: [from, to).
    const auto by_time=[](const auto& e,int64_t t){return e.timestamp_ms<t;};
    const size_t first=size_t(std::lower_bound(events.begin(),events.end(),liq_view_from_,by_time)-events.begin());
    const size_t last=std::max(first,size_t(std::lower_bound(events.begin(),events.end(),liq_view_to_,by_time)-events.begin()));
    const uint64_t revision=ctx_.liq_heatmap_mgr().reported_history(pair_).revision;
    auto& co=liq_callouts_;
    if(!co.valid || co.events!=&events || co.data!=events.data() || co.size!=events.size() || co.revision!=revision ||
       co.first!=first || co.last!=last || co.typical!=typ.usd || co.multiple!=liq_callout_x_ || co.min_usd!=liq_obs_min_usd_ ||
       co.tf!=tf || co.building!=b_open || co.candles!=&ts || co.candles_revision!=candles().history_revision()) {
        co.marks=observed_liquidations::callouts(events,liq_view_from_,liq_view_to_,typ.usd,liq_callout_x_,liq_obs_min_usd_,open_of);
        co.labels.resize(co.marks.size());
        for(size_t i=0;i<co.marks.size();++i)observed_liquidations::compact_usd(co.marks[i].usd,co.labels[i].data(),co.labels[i].size());
        co.font=nullptr;
        co.valid=true;co.events=&events;co.data=events.data();co.size=events.size();co.revision=revision;
        co.first=first;co.last=last;co.typical=typ.usd;co.multiple=liq_callout_x_;co.min_usd=liq_obs_min_usd_;
        co.tf=tf;co.building=b_open;co.candles=&ts;co.candles_revision=candles().history_revision();
    }
    const auto& marks=co.marks;
    if(marks.empty())return;
    const auto pos=ImPlot::GetPlotPos(),size=ImPlot::GetPlotSize();
    auto* draw=ImPlot::GetPlotDrawList();const auto mouse=ImGui::GetIO().MousePos;
    ImPlot::PushPlotClipRect();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    if(co.font!=ImGui::GetFont() || co.font_size!=ImGui::GetFontSize()) {
        co.font=ImGui::GetFont();co.font_size=ImGui::GetFontSize();
        co.sizes.clear();
        for(const auto& label:co.labels)co.sizes.push_back(ImGui::CalcTextSize(label.data()));
    }
    const float th=ImGui::GetTextLineHeight();
    constexpr float kPadX=5.0f,kPadY=2.0f,kGap=7.0f,kPointer=5.0f;
    std::vector<ImVec4> placed;  // (x0,y0,x1,y1)
    const observed_liquidations::CandleSide* hovered=nullptr;
    for(size_t i=0;i<marks.size();++i){
        const auto& m=marks[i];
        double h=0,l=0;
        if(!extremes(m.open,h,l))continue;
        const char* text=co.labels[i].data();
        const ImVec2 tsz=co.sizes[i];
        const float x=std::round(ImPlot::PlotToPixels(double(m.open),0).x);
        const float tip=ImPlot::PlotToPixels(0,m.shorts?h:l).y;
        const float w=tsz.x+2*kPadX,hgt=th+2*kPadY;
        float y0=m.shorts?tip-kGap-hgt:tip+kGap;
        // Kept inside the plot; a pill pushed back over its candle loses its pointer.
        const float top=pos.y+2.0f,bottom=pos.y+size.y-2.0f-hgt;
        const bool clamped=y0<top || y0>bottom;
        y0=std::clamp(y0,top,std::max(top,bottom));
        const ImVec2 a(std::round(x-w*0.5f),std::round(y0)),b(a.x+w,a.y+hgt);
        if(b.x<pos.x || a.x>pos.x+size.x)continue;
        bool overlaps=false;
        for(const auto& r:placed)overlaps=overlaps || (a.x<r.z+2 && b.x>r.x-2 && a.y<r.w+2 && b.y>r.y-2);
        if(overlaps)continue;
        placed.push_back(ImVec4(a.x,a.y,b.x,b.y));
        const ImU32 fill=Theme::u32(m.shorts?Theme::Tokens::UP:Theme::Tokens::DOWN,0.94f);
        draw->AddRectFilled(ImVec2(a.x-1,a.y-1),ImVec2(b.x+1,b.y+1),Theme::u32(Theme::Tokens::BASE,0.85f),3.0f);
        draw->AddRectFilled(a,b,fill,2.0f);
        if(!clamped){
            const float py=m.shorts?b.y:a.y,dir=m.shorts?1.0f:-1.0f;
            draw->AddTriangleFilled(ImVec2(x-4,py),ImVec2(x+4,py),ImVec2(x,py+dir*kPointer),fill);
        }
        draw->AddText(ImVec2(a.x+kPadX,a.y+kPadY),Theme::u32(Theme::Tokens::BRAND_INK),text);
        if(mouse.x>=a.x && mouse.x<=b.x && mouse.y>=a.y-kPointer && mouse.y<=b.y+kPointer)hovered=&m;
    }
    ImGui::PopFont();
    ImPlot::PopPlotClipRect();
    if(!hovered || !ImPlot::IsPlotHovered())return;
    const auto& m=*hovered;
    char total[24],lo_s[40],hi_s[40],first_t[64],last_t[64],typical[24];
    liqmap2::money(m.usd,total,sizeof(total));
    liqmap2::money(typ.usd,typical,sizeof(typical));
    fmt_.format_price(lo_s,sizeof(lo_s),m.low);fmt_.format_price(hi_s,sizeof(hi_s),m.high);
    DisplayTimeZone::instance().format(m.first,TimeZoneFormat::MonthDayTime,first_t,sizeof(first_t));
    DisplayTimeZone::instance().format(m.last,TimeZoneFormat::TimeMinutes,last_t,sizeof(last_t));
    Theme::begin_tooltip(true);
    ImGui::TextColored(m.shorts?Theme::Tokens::UP:Theme::Tokens::DOWN,"%s liquidated  %s",m.shorts?"Shorts":"Longs",total);
    ImGui::Text("%.1f\xc3\x97 this market's typical candle (%s)",m.usd/typ.usd,typical);
    // Bybit's prices are bankruptcy prices, so its range would mislead here; the time span is what reads.
    if(observed_liquidations::bankruptcy_prices(pair_.exchange))
        ImGui::TextDisabled("%u report%s \xc2\xb7 %s to %s",m.count,m.count==1?"":"s",first_t,last_t);
    else
        ImGui::TextDisabled("%u report%s at %s to %s \xc2\xb7 %s to %s",m.count,m.count==1?"":"s",lo_s,hi_s,first_t,last_t);
    ImGui::TextDisabled("%s",observed_liquidations::feed_line(pair_.exchange));
    Theme::end_tooltip();
}

void ChartWidget::render_liq_observed() {
    if(!rt_mode_){render_liq_callouts();return;}
    // Real-time: diamonds at the report's time on the tape. Binance and Hyperliquid report the price the forced
    // order filled at; Bybit reports the bankruptcy price, often percent away and off this zoom, so its diamonds sit
    // on the print the forced order made (observed_liquidations::tape_price) and the hover gives both.
    const auto& events=realtime_liquidations();const auto limits=ImPlot::GetPlotLimits();
    const auto size=ImPlot::GetPlotSize();if(size.x<=0 || size.y<=0)return;
    // History backfill remains in the activity pane, but cannot float before
    // the real-time depth history shown by this chart (including paused views).
    const auto& samples=realtime_samples();
    if(samples.empty())return;
    const int64_t marker_from=std::max(liq_view_from_,samples.front()->timestamp_ms);
    const auto reference=liq_obs_auto_size_?
        liq_obs_size_reference_.update(events,ctx_.liq_heatmap_mgr().reported_history(pair_).revision,liq_cutoff_)
        :observed_liquidations::MarketSizeReference::Reading{};
    const bool on_tape=observed_liquidations::bankruptcy_prices(pair_.exchange);
    const auto& tape=realtime_trades();
    // Explicit screen clusters; original report coordinates are never modified.
    // Separate sides, anchored on each cluster's largest report. Zoom reveals detail.
    struct Cluster{const Terminal::Liquidation* largest=nullptr;double total=0,low=0,high=0,at=0;int64_t first=0,last=0;unsigned count=0;};
    std::map<std::tuple<int64_t,int64_t,bool>,Cluster> clusters;
    const int cols=std::clamp(int(size.x/20),1,64),rows=std::clamp(int(size.y/20),1,32);
    const double time_step=observed_liquidations::stable_marker_step(limits.X.Size()/cols,1.0);
    const double price_step=observed_liquidations::stable_marker_step(limits.Y.Size()/rows,std::max(1e-12,double(tick_size_)));
    auto first=std::lower_bound(events.begin(),events.end(),marker_from,[](const auto& e,int64_t t){return e.timestamp_ms<t;});
    unsigned filtered=0;
    for(auto it=first;it!=events.end() && it->timestamp_ms<liq_view_to_;++it){
        const double n=observed_liquidations::notional(*it);
        const double p=on_tape?observed_liquidations::tape_price(tape,it->timestamp_ms,it->is_buy):observed_liquidations::price(*it);
        if(n<=0 || n<liq_obs_min_usd_ || !std::isfinite(p) || p<limits.Y.Min || p>limits.Y.Max){++filtered;continue;}
        // World-aligned buckets do not move when the viewport pans or follows
        // live. Power-of-two scales avoid regrouping on tiny auto-fit changes.
        const int64_t x=int64_t(std::floor(double(it->timestamp_ms)/time_step));
        const int64_t y=int64_t(std::floor(p/price_step));
        auto& c=clusters[{x,y,it->is_buy}];
        const double reported=observed_liquidations::price(*it);
        if(!c.count){c.first=it->timestamp_ms;c.low=reported;c.high=reported;}
        c.last=it->timestamp_ms;c.low=std::min(c.low,reported);c.high=std::max(c.high,reported);c.total+=n;++c.count;
        if(!c.largest || n>observed_liquidations::notional(*c.largest)){c.largest=&*it;c.at=p;}
    }
    auto* draw=ImPlot::GetPlotDrawList();const auto mouse=ImGui::GetIO().MousePos;
    ImPlot::PushPlotClipRect();
    const Cluster* hovered=nullptr;float nearest=1000;
    for(const auto& [key,c]:clusters){if(!c.count)continue;const auto& e=*c.largest;
        const auto center=ImPlot::PlotToPixels(double(e.timestamp_ms),c.at);
        const float radius=liq_obs_auto_size_?observed_liquidations::relative_marker_radius(c.total/reference.usd)
            :observed_liquidations::marker_radius(c.total,liq_obs_ref_usd_);
        const ImVec2 points[]={{center.x,center.y-radius},{center.x+radius,center.y},{center.x,center.y+radius},{center.x-radius,center.y}};
        draw->AddConvexPolyFilled(points,4,Theme::u32(Theme::Tokens::BASE));
        draw->AddPolyline(points,4,Theme::u32(Theme::Tokens::BASE),ImDrawFlags_Closed,4);
        draw->AddPolyline(points,4,Theme::u32(e.is_buy?Theme::Tokens::UP:Theme::Tokens::DOWN),ImDrawFlags_Closed,1.5f);
        draw->AddCircleFilled(center,1.25f,Theme::u32(Theme::Tokens::TX1));
        if(c.count>1 && ImGui::GetIO().KeyAlt){char count[16];snprintf(count,sizeof(count),"%u",c.count);draw->AddText(ImVec2(center.x+radius,center.y-radius),Theme::u32(Theme::Tokens::TX1),count);}
        const float d=std::abs(mouse.x-center.x)+std::abs(mouse.y-center.y);if(d<radius+4 && d<nearest){hovered=&c;nearest=d;}
    }
    ImPlot::PopPlotClipRect();
    if(!hovered || ImGui::GetIO().KeyAlt || !realtime_hover_ready())return;
    const auto& c=*hovered;const auto& e=*c.largest;
    char total[32],when[64],last_time[64],price[40],at[40];
    observed_liquidations::hover_usd(c.total,total,sizeof(total));
    DisplayTimeZone::instance().format(c.first,TimeZoneFormat::TimeSeconds,when,sizeof(when));
    DisplayTimeZone::instance().format(c.last,TimeZoneFormat::TimeSeconds,last_time,sizeof(last_time));
    Theme::begin_tooltip(true);
    ImGui::TextColored(e.is_buy?Theme::Tokens::UP:Theme::Tokens::DOWN,"%s  %s",e.is_buy?"Shorts liquidated":"Longs liquidated",total);
    if(c.count>1)ImGui::TextDisabled("%u reports \xc2\xb7 %s to %s",c.count,when,last_time);
    else ImGui::TextDisabled("%s",when);
    ImGui::TextDisabled("%s",observed_liquidations::feed_line(pair_.exchange));
    if(ImGui::GetIO().KeyAlt){
        ImGui::Separator();
        fmt_.format_price(at,sizeof(at),c.at);
        if(c.count>1){
            char low[40],high[40];fmt_.format_price(low,sizeof(low),c.low);fmt_.format_price(high,sizeof(high),c.high);
            ImGui::Text("Reported prices %s to %s",low,high);
        } else {
            fmt_.format_price(price,sizeof(price),observed_liquidations::price(e));
            ImGui::Text("%s %s",on_tape?"Bankruptcy price":e.avg_price>0?"Average fill":"Order price",price);
        }
        if(on_tape)ImGui::Text("Drawn on the tape at %s",at);
        if(liq_obs_auto_size_)
            ImGui::Text("%.1f\xc3\x97 this market's mean report in the last hour ($%.0f, %zu reports)",c.total/reference.usd,reference.usd,reference.count);
        else ImGui::Text("Sized against $%.0f",liq_obs_ref_usd_);
        ImGui::Text("Reported quantity %.8g%s",e.qty,e.is_buy?" \xc2\xb7 forced buy":" \xc2\xb7 forced sell");
        if(e.trade_timestamp_ms>0){char time[64];DisplayTimeZone::instance().format(e.trade_timestamp_ms,TimeZoneFormat::DateTimeSeconds,time,sizeof(time));ImGui::Text("Source trade time %s.%03lld",time,static_cast<long long>(e.trade_timestamp_ms%1000));}
        if(filtered)ImGui::TextDisabled("%u reports hidden by the minimum or the visible prices",filtered);
    }
    Theme::end_tooltip();
}

#include "ui/chart_widget.h"
#include "core/activity_strip.h"
#include "core/terminal_request.h"
#include "core/candle_manager.h"
#include "core/orderbook_manager.h"
#include "core/display_time_zone.h"
#include "core/entitlements.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"
#include <emscripten.h>
#include <cctype>
#include <cstdlib>

EM_JS(void, activity_request_start, (int id,const char* symbol,double from,double to,double clock,int activity,int alerts), {
    var slots=Module['terminalRequests']||(Module['terminalRequests']={});
    var old=slots[id];if(old){clearTimeout(old.timer);old.controller.abort();}
    var slot={controller:new AbortController(),result:null,timer:null};slots[id]=slot;
    slot.timer=setTimeout(function(){slot.controller.abort();},10000);
    var q=new URLSearchParams({symbol:UTF8ToString(symbol),from:String(from),to:String(to),clock:String(clock),activity:String(activity),alerts:String(alerts)});
    fetch('/api/admin/watchlist/terminal?'+q,{credentials:'same-origin',cache:'no-store',signal:slot.controller.signal})
      .then(async function(r){if(r.status===403)return 'locked';if(!r.ok)return 'unavailable';var body=await r.text();return body.length<=262144?body:'unavailable';})
      .catch(function(){return 'unavailable';}).then(function(body){clearTimeout(slot.timer);if(slots[id]===slot)slot.result=body;});
});
// Local journal references only. A browser Save As action is the only filesystem
// write; reopening goes through the normal admitted replay route.
EM_JS(void, activity_bookmark_save, (const char* exchange,const char* symbol,double t,const char* note), {
    var item={exchange:UTF8ToString(exchange),symbol:UTF8ToString(symbol),t:t,note:UTF8ToString(note)};
    var name=item.symbol+'-'+t+'.json',text=JSON.stringify(item,null,2)+'\n';
    Module['activityBookmarkStatus']='Saving...';
    function download(){
      var url=URL.createObjectURL(new Blob([text],{type:'application/json'}));var a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(function(){URL.revokeObjectURL(url);},1000);
      Module['activityBookmarkStatus']='Download requested. Move it into the setup journal.';
    }
    (async function(){
      try {
        if(window.showSaveFilePicker){
          var handle=await window.showSaveFilePicker({id:'edgedepth-setup-journal',suggestedName:name,types:[{description:'Replay bookmark',accept:{'application/json':['.json']}}]});
          var writer=await handle.createWritable();await writer.write(text);await writer.close();
          Module['activityBookmarkStatus']='Bookmark saved. Recording is not preserved.';
        } else {
          download();
        }
      } catch(e){if(e.name==='SecurityError')download();else Module['activityBookmarkStatus']=e.name==='AbortError'?'Save cancelled.':'Could not save bookmark.';}
    })();
});
EM_JS(void, activity_bookmark_open, (), {
    var input=document.createElement('input');input.type='file';input.accept='.json,application/json';
    input.onchange=async function(){try{
      var file=input.files&&input.files[0];if(!file||file.size>16384)throw new Error();
      var b=JSON.parse(await file.text());
      if(!b||!['binancef','bybit','hl'].includes(b.exchange)||typeof b.symbol!=='string'||!/^[A-Za-z0-9_]{1,40}$/.test(b.symbol)||!Number.isSafeInteger(b.t)||b.t<=0||b.t>Date.now()||typeof b.note!=='string'||b.note.length>4096)throw new Error();
      var from=Math.max(1,b.t-3600000),to=Math.min(Date.now(),b.t+2*3600000);
      var q=new URLSearchParams({replay:b.symbol,exchange:b.exchange,from:String(from),to:String(to),t:String(b.t)});
      var url='/terminal?'+q;
      try{var open=window.__edgedepthOpenReplay;if(typeof open==='function'&&open(url)===true)return;}catch(oe){}
      window.location.assign(url);
    }catch(e){Module['activityBookmarkStatus']='Invalid replay bookmark.';}};input.click();
});

bool ChartWidget::activity_visible() const {
    return activity_enabled_ && pair_.exchange=="binancef" && Entitlements::is_admin() && (rt_mode_||ct_allows_time_overlays(chart_type_));
}
float ChartWidget::activity_height() const {
    if(!activity_visible())return 0;
    const float header=activity_header_height_;
    return header+(activity_collapsed_?0:84.0f);
}
void ChartWidget::update_activity() {
    const bool replay=ctx_.replay_mgr().is_active();
    const int64_t clock=replay?ctx_.replay_mgr().interpolated_time_ms():int64_t(emscripten_date_now());
    const auto generation=ctx_.ob_mgr().realtime_generation();
    const std::string market=pair_.exchange+"/"+pair_.symbol;
    const int64_t rewind=replay?ctx_.replay_mgr().rewind_cutoff_ms():0;
    if(market!=activity_market_||(!replay&&clock+2000<activity_clock_)||(rewind>0&&rewind!=activity_rewind_cutoff_)||replay!=activity_replay_||generation!=activity_book_generation_||ctx_.replay_mgr().is_loading()){
        if(activity_request_id_)terminal_request_cancel(activity_request_id_);
        activity_pending_=false;activity_data_={};activity_book_.reset();activity_request_key_.clear();activity_next_read_=0;
    }
    activity_rewind_cutoff_=rewind;activity_market_=market;activity_replay_=replay;activity_clock_=clock;activity_book_generation_=generation;
    if(!Entitlements::is_admin()||pair_.exchange!="binancef"||ctx_.replay_mgr().is_pack_mode()||ctx_.replay_mgr().is_loading()||
        !(activity_enabled_||alert_lines_||alert_markers_)){
        if(activity_request_id_)terminal_request_cancel(activity_request_id_);activity_pending_=false;return;
    }
    if(activity_book_enabled_ && ctx_.ob_mgr().realtime_ready(pair_,clock)) {
        const auto* book=ctx_.ob_mgr().get_orderbook(pair_);
        if(book&&!book->bids.empty()&&!book->asks.empty()&&book->timestamp_ms<=clock&&clock-book->timestamp_ms<=5000){
            const double mid=(book->bids.begin()->first+book->asks.begin()->first)*.5;
            double bids=0,asks=0;for(const auto& [p,q]:book->bids)if(p>=mid*.98&&p<=mid)bids+=p*q;
            for(const auto& [p,q]:book->asks)if(p>=mid&&p<=mid*1.02)asks+=p*q;
            if(bids+asks>0)activity_book_.observe(book->timestamp_ms,bids/(bids+asks));
        }
    }
    int64_t to=clock/60000*60000;
    if(stored_x_max_>0)to=std::min(to,(int64_t(stored_x_max_)+59999)/60000*60000);
    int64_t from=stored_x_min_>0?int64_t(stored_x_min_)/60000*60000:to-180*60000;
    from=std::max<int64_t>(from,to-240*60000);
    if(to<from)return;
    // A moving playhead reuses its bounded minute request, but every render is
    // gated again by the current clock. Seeks invalidate the book generation;
    // tiny backward interpolation corrections must not starve the HTTP read.
    const int64_t request_clock=replay?clock:to+int64_t(emscripten_date_now())%60000;
    const std::string key=std::to_string(from)+"/"+std::to_string(to)+"/"+std::to_string(activity_enabled_)+"/"+std::to_string(alert_lines_||alert_markers_);
    if(!activity_request_id_)activity_request_id_=next_terminal_request_id();
    if(char* raw=terminal_request_take(activity_request_id_)){
        activity_pending_=false;
        {
            if(std::string_view(raw)=="locked")activity_data_.status="Admin access required";
            else if(std::string_view(raw)=="unavailable")activity_data_.status="Source unavailable";
            else {
                const auto j=nlohmann::json::parse(raw,nullptr,false);
                activity_data_=activity_strip::parse(j,pair_.symbol,activity_from_,activity_to_,activity_requested_clock_);
            }
        }
        std::free(raw);
    }
    const double now=emscripten_get_now();
    if(activity_pending_)return;
    if(key==activity_request_key_ && now<activity_next_read_)return;
    if(now<activity_request_sent_+1000)return;
    activity_request_key_=key;activity_from_=from;activity_to_=to;activity_requested_clock_=request_clock;
    activity_next_read_=now+15000;activity_request_sent_=now;activity_pending_=true;
    activity_request_start(activity_request_id_,pair_.symbol.c_str(),double(from),double(to),double(request_clock),activity_enabled_,alert_lines_||alert_markers_);
}
// Layers ends with this action in both chart modes: a saved replay reference is not a layer.
void ChartWidget::render_bookmark_action() {
    if(Entitlements::hosted()&&Theme::menu_item(drawing::UiIcon::Library,"Open replay bookmark\xe2\x80\xa6"))activity_bookmark_open();
}
void ChartWidget::render_activity_settings() {
    if(!Entitlements::is_admin()||pair_.exchange!="binancef")return;
    if(Theme::menu_toggle("Activity strip",activity_enabled_,false))activity_enabled_=!activity_enabled_;
    if(activity_enabled_){
        Theme::begin_menu_detail();
        if(Theme::begin_menu_group("Options##activity")){
            ImGui::Checkbox("Trades multiple",&activity_trades_);ImGui::Checkbox("Volume multiple",&activity_volume_);
            ImGui::Checkbox("5m open interest",&activity_oi_);ImGui::Checkbox("5m taker buys",&activity_buy_);
            ImGui::Checkbox("State shading",&activity_shading_);ImGui::Checkbox("Book balance (+/-2%)",&activity_book_enabled_);
            if(ImGui::IsItemHovered())Theme::tooltip("Bid dollars / (bid + ask dollars) within 2%% of mid. Recorded depth received here; gaps remain blank.");
            Theme::end_menu_group();
        }
        Theme::end_menu_detail();
    }
    if(Theme::menu_toggle("Watcher lines",alert_lines_,false))alert_lines_=!alert_lines_;
    if(Theme::menu_toggle("Watcher markers",alert_markers_,false))alert_markers_=!alert_markers_;
    if((alert_lines_||alert_markers_)&&activity_data_.alerts_status=="unavailable")Theme::menu_note("Watcher source unavailable");
}
void ChartWidget::render_activity_strip() {
    if(!activity_visible())return;
    ImGui::PushID("MarketActivity");
    const float header_top=ImGui::GetCursorScreenPos().y;
    const double shift=rt_mode_?0:candles().timeframe_seconds()*500.0;
    int64_t at=crosshair_state_.is_active?int64_t(crosshair_state_.plot_pos.x+shift)/60000*60000:activity_clock_/60000*60000-60000;
    const activity_strip::Point* selected=nullptr;
    for(const auto& p:activity_data_.points)if(p.t==at&&p.t+60000<=activity_clock_)selected=&p;
    // The header, one family with the Reported liquidations pane: a drawn chevron, the name, the minute's state as a
    // chip, then the readings (label quiet, value in mono; a muted "-" when a reading is not in).
    auto* header_draw=ImGui::GetWindowDrawList();
    {
        const auto button=ImGui::GetCursorScreenPos();
        const bool toggle=ImGui::InvisibleButton("##collapse",ImVec2(22,20));
        const bool hovered=ImGui::IsItemHovered();
        if(hovered)header_draw->AddRectFilled(button,ImVec2(button.x+22,button.y+20),Theme::u32(Theme::Tokens::HOVER),3);
        const float d=activity_collapsed_?-1.0f:1.0f,cx=button.x+11,cy=button.y+10;
        const ImVec2 chevron[]={{cx-4,cy-2*d},{cx,cy+2*d},{cx+4,cy-2*d}};
        header_draw->AddPolyline(chevron,3,Theme::u32(hovered?Theme::Tokens::TX1:Theme::Tokens::TX2),0,1.5f);
        if(toggle)activity_collapsed_=!activity_collapsed_;
    }
    ImGui::SameLine(0,6);ImGui::TextUnformatted("Activity");
    if(ImGui::IsItemHovered())Theme::tooltip("5 closed minutes / median of the preceding 60. Both >=3x: active. Active + $50k reported on one liquidation side in 15m: forced.\nMinute labels are opens; readings appear after the minute closes. Binance 1m candles match jdc-watch. Liquidations combine Binance (sampled, a floor) and Bybit (every liquidation).\nOI uses recorded contract polls within 2m of each endpoint; gaps over 2m stay blank.");
    {
        std::string state=selected?selected->state:activity_data_.status.empty()?"No closed minutes":activity_data_.status;
        if(!state.empty())state[0]=char(std::toupper((unsigned char)state[0]));
        const bool forced=selected&&selected->state=="forced",active=selected&&selected->state=="active";
        const ImVec4 fg=forced?Theme::Tokens::WARN:active?Theme::Tokens::UP:Theme::Tokens::TX3;
        ImGui::SameLine(0,10);
        const ImVec2 cp=ImGui::GetCursorScreenPos();
        const ImVec2 cs=ImGui::CalcTextSize(state.c_str());
        const float ch=cs.y+4,cw=cs.x+14;
        header_draw->AddRectFilled(ImVec2(cp.x,cp.y+(ImGui::GetTextLineHeight()-ch)*0.5f),ImVec2(cp.x+cw,cp.y+(ImGui::GetTextLineHeight()+ch)*0.5f),
                                   Theme::u32(forced||active?ImVec4(fg.x,fg.y,fg.z,0.12f):Theme::Tokens::CONTROL),Theme::Radius::R1);
        header_draw->AddText(ImVec2(cp.x+7,cp.y),Theme::u32(fg),state.c_str());
        ImGui::Dummy(ImVec2(cw,ImGui::GetTextLineHeight()));
    }
    auto metric=[&](const char* label,double value,const char* format){
        char text[60];
        const bool have=std::isfinite(value);
        if(have)snprintf(text,sizeof(text),format,value);else snprintf(text,sizeof(text),"-");
        ImGui::PushFont(Theme::Fonts::mono_sm());const float vw=ImGui::CalcTextSize(text).x;ImGui::PopFont();
        if(ImGui::GetItemRectMax().x+ImGui::CalcTextSize(label).x+vw+30<ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x)ImGui::SameLine(0,16);
        ImGui::TextColored(Theme::Tokens::TX3,"%s",label);ImGui::SameLine(0,6);
        ImGui::PushFont(Theme::Fonts::mono_sm());ImGui::TextColored(have?Theme::Tokens::TX1:Theme::Tokens::TX4,"%s",text);ImGui::PopFont();
    };
    if(activity_trades_)metric("Trades",selected?selected->trades:activity_strip::missing,"%.1fx");
    if(activity_volume_)metric("Volume",selected?selected->volume:activity_strip::missing,"%.1fx");
    if(activity_oi_)metric("OI 5m",selected?selected->oi*100:activity_strip::missing,"%+.1f%%");
    if(activity_buy_)metric("Buy 5m",selected?selected->buy*100:activity_strip::missing,"%.0f%%");
    if(activity_book_enabled_){auto p=activity_book_.closed.find(at);metric("Bids",p!=activity_book_.closed.end()?p->second*100:activity_strip::missing,"%.0f%%");}
    activity_header_height_=std::max(28.f,ImGui::GetCursorScreenPos().y-header_top);
    if(activity_collapsed_){ImGui::PopID();return;}
    ImGui::PushFont(Theme::Fonts::mono_sm());
    if(ImPlot::BeginPlot("##Activity",ImVec2(-1,84),ImPlotFlags_NoTitle|ImPlotFlags_NoLegend|ImPlotFlags_NoMouseText|ImPlotFlags_NoInputs)){
        ImPlot::SetupAxis(ImAxis_X1,nullptr,ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxis(ImAxis_Y1,nullptr,ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisLimits(ImAxis_X1,last_visible_range_.X.Min,last_visible_range_.X.Max,ImGuiCond_Always);
        double peak=3;for(const auto& p:activity_data_.points)if(p.t+60000<=activity_clock_&&p.t-shift>=last_visible_range_.X.Min&&p.t-shift<=last_visible_range_.X.Max){if(activity_trades_&&std::isfinite(p.trades))peak=std::max(peak,p.trades);if(activity_volume_&&std::isfinite(p.volume))peak=std::max(peak,p.volume);}
        const double top=std::log1p(peak)*1.08;
        ImPlot::SetupAxisLimits(ImAxis_Y1,0,top,ImGuiCond_Always);
        const double ticks[]={std::log(2.),std::log(4.),std::log(11.)};const char* labels[]={"1x","3x","10x"};ImPlot::SetupAxisTicks(ImAxis_Y1,ticks,peak>=10?3:2,labels);
        ImPlot::SetupFinish();const auto pos=ImPlot::GetPlotPos(),size=ImPlot::GetPlotSize();auto* draw=ImPlot::GetPlotDrawList();
        crosshair_state_.last_plot_max=ImVec2(pos.x+size.x,pos.y+size.y);
        const bool hovered=ImGui::IsWindowHovered()&&ImGui::IsMouseHoveringRect(pos,ImVec2(pos.x+size.x,pos.y+size.y));
        if(hovered){crosshair_state_.is_active=true;crosshair_state_.indicator_hovered=true;crosshair_state_.plot_pos=ImPlot::GetPlotMousePos();crosshair_state_.hovered_plot_min=pos;crosshair_state_.hovered_plot_max=ImVec2(pos.x+size.x,pos.y+size.y);}
        ImPlot::PushPlotClipRect();
        const activity_strip::Point* previous=nullptr;
        for(const auto& p:activity_data_.points){if(p.t+60000>activity_clock_)break;
            if(activity_shading_&&(p.state=="active"||p.state=="forced")){
                const auto a=ImPlot::PlotToPixels(double(p.t)-shift,0),b=ImPlot::PlotToPixels(double(p.t+60000)-shift,top);
                draw->AddRectFilled(ImVec2(a.x,pos.y),ImVec2(b.x,pos.y+size.y),Theme::u32(p.state=="forced"?Theme::Tokens::WARN_SOFT:Theme::Tokens::ACTIVITY_ACTIVE));
            }
            auto line=[&](double current,double prior,ImU32 color){if(previous&&p.t-previous->t==60000&&std::isfinite(current)&&std::isfinite(prior))draw->AddLine(ImPlot::PlotToPixels(double(previous->t)-shift,std::log1p(prior)),ImPlot::PlotToPixels(double(p.t)-shift,std::log1p(current)),color,1.2f);};
            if(activity_trades_)line(p.trades,previous?previous->trades:activity_strip::missing,Theme::u32(Theme::Tokens::ACTIVITY_TRADES));
            if(activity_volume_)line(p.volume,previous?previous->volume:activity_strip::missing,Theme::u32(Theme::Tokens::TX1));
            previous=&p;
        }
        if(activity_book_enabled_){
            draw->AddLine(ImVec2(pos.x,pos.y+size.y*.5f),ImVec2(pos.x+size.x,pos.y+size.y*.5f),Theme::u32(Theme::Tokens::BD3));
            int64_t prev=0;double share=0;
            for(const auto& [t,b]:activity_book_.closed){if(t+60000>activity_clock_)break;if(prev&&t-prev==60000)draw->AddLine(ImPlot::PlotToPixels(double(prev)-shift,share*top),ImPlot::PlotToPixels(double(t)-shift,b*top),Theme::u32(Theme::Tokens::WARN),1.2f);prev=t;share=b;}
            draw->AddText(ImVec2(pos.x+5,pos.y+size.y*.5f-16),Theme::u32(Theme::Tokens::TX3),"Bids 50%");
        }
        ImPlot::PopPlotClipRect();ImPlot::EndPlot();
    }
    ImGui::PopID();ImGui::PopFont();
}
void ChartWidget::render_activity_alerts() {
    if(!Entitlements::is_admin()||pair_.exchange!="binancef"||(!alert_lines_&&!alert_markers_))return;
    auto* draw=ImPlot::GetPlotDrawList();const auto lim=ImPlot::GetPlotLimits();
    ImPlot::PushPlotClipRect();
    if(alert_lines_)for(const auto& l:activity_data_.lines){
        if(l.known>activity_clock_)continue;
        auto segment=[&](bool support,double factor,ImU32 color,bool dashed){
            const double start=std::max(lim.X.Min,double(support?l.support_from:l.origin));
            const double end=l.range?std::min(lim.X.Max,double(l.until)):lim.X.Max;if(end<=start||(support?l.bottom(end):l.top(end))*factor<=0)return;
            const auto a=ImPlot::PlotToPixels(start,(support?l.bottom(start):l.top(start))*factor),b=ImPlot::PlotToPixels(end,(support?l.bottom(end):l.top(end))*factor);
            if(dashed){const float len=std::hypot(b.x-a.x,b.y-a.y);for(float d=0;d<len;d+=8){const float u=d/std::max(1.f,len),v=std::min(d+4,len)/std::max(1.f,len);draw->AddLine(ImVec2(a.x+(b.x-a.x)*u,a.y+(b.y-a.y)*u),ImVec2(a.x+(b.x-a.x)*v,a.y+(b.y-a.y)*v),color,1);}}
            else draw->AddLine(a,b,color,1.3f);
            const auto mouse=ImGui::GetMousePos();const float dx=b.x-a.x,dy=b.y-a.y,len=dx*dx+dy*dy,u=len>0?std::clamp(((mouse.x-a.x)*dx+(mouse.y-a.y)*dy)/len,0.f,1.f):0;
            if(ImPlot::IsPlotHovered()&&std::hypot(mouse.x-a.x-u*dx,mouse.y-a.y-u*dy)<5){
                const double t=ImPlot::GetPlotMousePos().x;char value[40];snprintf(value,sizeof(value),fmt_.price_fmt,(support?l.bottom(t):l.top(t))*factor);
                Theme::tooltip("%s | %s %s\n%s",l.label.c_str(),support?"Support":factor>1?"Buy-stop":factor<1?"Research stop":"Line",value,l.source.c_str());
            }
        };
        segment(false,1,Theme::u32(Theme::Tokens::ACTIVITY_TRADES),false);segment(true,1,Theme::u32(Theme::Tokens::TX3),false);
        segment(false,1.005,Theme::u32(Theme::Tokens::UP_LINE),true);
        if(!l.range)segment(false,.97,Theme::u32(Theme::Tokens::DOWN_LINE),true);
    }
    if(alert_markers_)for(const auto& m:activity_data_.markers){
        if(m.known>activity_clock_||m.t>activity_clock_||m.t<lim.X.Min||m.t>lim.X.Max)continue;
        const auto p=ImPlot::PlotToPixels(double(m.t),m.price);const ImU32 color=Theme::u32(m.setup?Theme::Tokens::WARN:Theme::Tokens::TX2);
        draw->AddCircleFilled(p,m.setup?4.5f:3.f,color);if(m.setup)draw->AddText(ImVec2(p.x+7,p.y-16),color,"Setup");
        const auto mouse=ImGui::GetMousePos();if(ImPlot::IsPlotHovered()&&std::hypot(mouse.x-p.x,mouse.y-p.y)<9){char time[64];DisplayTimeZone::instance().format(m.t,TimeZoneFormat::FullInspection,time,sizeof(time));Theme::tooltip("%s | %s",m.label.c_str(),time);}
    }
    ImPlot::PopPlotClipRect();
}
void ChartWidget::render_activity_bookmark() {
    if(!Entitlements::hosted())return;
    if(ctx_.replay_mgr().is_active()&&ImGui::Button("Bookmark",ImVec2(0,28))){bookmark_time_=ctx_.replay_mgr().interpolated_time_ms();bookmark_note_[0]=0;ImGui::OpenPopup("Replay bookmark");}
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,12));
    if(ImGui::BeginPopup("Replay bookmark")){
        char time[64];DisplayTimeZone::instance().format(bookmark_time_,TimeZoneFormat::FullInspection,time,sizeof(time));ImGui::Text("%s | %s",pair_.symbol.c_str(),time);
        ImGui::TextUnformatted("Note (optional)");
        ImGui::SetNextItemWidth(std::min(360.f,ImGui::GetMainViewport()->WorkSize.x-60));ImGui::InputTextMultiline("##note",bookmark_note_,sizeof(bookmark_note_),ImVec2(0,70));
        ImGui::TextWrapped("Saves a reference, not the recording. Data can expire. Use the event archiver to preserve replay evidence.");
        ImGui::TextWrapped("Choose your setup-journal/bookmarks folder when saving.");
        if(ImGui::Button("Save JSON..."))activity_bookmark_save(pair_.exchange.c_str(),pair_.symbol.c_str(),double(bookmark_time_),bookmark_note_);
        ImGui::SameLine();if(ImGui::Button("Open bookmark..."))activity_bookmark_open();
        const char* status=emscripten_run_script_string("Module['activityBookmarkStatus'] || ''");if(status&&*status)ImGui::TextWrapped("%s",status);
        if(ImGui::IsItemHovered())Theme::tooltip("Choose todos-edgedepth/outputs/setup-journal/bookmarks when saving. Open uses normal replay access and retained-data availability.");
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

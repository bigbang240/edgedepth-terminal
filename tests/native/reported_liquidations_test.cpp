#include "core/reported_liquidation_history.h"
#include "rendering/observed_liquidations.h"
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
struct Event { double price=0,avg_price=0,qty=0;bool is_buy=false;int64_t timestamp_ms=0,trade_timestamp_ms=0;double filled_qty=0; };
using History=ReportedLiquidationHistory<Event>;
using Json=nlohmann::json;
int main(){
 int failures=0;auto check=[&](bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);++failures;}};
 const int64_t now=1800000000000, from=now-900000;
 History h;h.connection(true,now);
 auto req=h.request("binancef","gtcusdt",from,now-3000,now,86400000,1000);
 check(!req.empty(),"opening a live chart requests prior recorded reports");
 auto make=[&](const Json& request){return Json{{"request_id",request["data"]["request_id"]},{"exchange","binancef"},{"symbol","gtcusdt"},{"from_ms",request["data"]["from_ms"]},{"to_ms",request["data"]["to_ms"]},{"coverage","recorded_reports_only"},{"retrieved_at_ms",now},{"reports",Json::array()}};};
 Event e{.price=.17812,.avg_price=.162015,.qty=1228.9,.is_buy=true,.timestamp_ms=from+1000};
 h.add(e);h.add(e);check(h.events.size()==1,"two charts cannot duplicate a received report");
 auto response=make(req);
 response["reports"].push_back({{"timestamp_ms",e.timestamp_ms},{"price",e.price},{"avg_price",e.avg_price},{"qty",e.qty},{"is_buy",true},{"trade_timestamp_ms",e.timestamp_ms-1005},{"filled_qty",e.qty}});
 response["reports"].push_back(response["reports"][0]);
 History::receive(response);check(h.events.size()==1 && h.events[0].trade_timestamp_ms==e.timestamp_ms-1005,"history/live overlap deduplicates and preserves source details");
 check(h.covers(from,now-3000),"successful recorded window is tracked");
 e.timestamp_ms=from+60000;e.is_buy=false;e.qty=100;h.add(e);
 auto bins=observed_liquidations::aggregate(h.events,from,from+60000,now,60000);
 check(bins.count==1 && bins.shorts>0 && bins.longs==0,"candle boundaries are half-open");
 bins=observed_liquidations::aggregate(h.events,from,now,from+2000,1000);
 check(bins.count==1,"replay/pause cutoff excludes future reports");
 bins=observed_liquidations::aggregate(h.events,from,now,now,60000);
 check(bins.count==2 && std::abs(bins.shorts-199.1002335)<1e-6 && std::abs(bins.longs-16.2015)<1e-6,"both sides remain separate with exact reported notional");
 h.reset_requests();req=h.request("binancef","gtcusdt",from,now-3000,now,86400000,2000);response=make(req);response["symbol"]="btcusdt";
 History::receive(response);check(!h.error.empty() && h.loaded.empty() && h.events.size()==2,"wrong-market response cannot mutate the cache");
 h.reset_requests();req=h.request("binancef","gtcusdt",from,now-3000,now,86400000,3000);response=make(req);
 h.trim_after(from+2000);History::receive(response);check(h.loaded.empty() && h.events.size()==1,"rewind cancels historical callbacks");
 e.qty=std::numeric_limits<double>::infinity();h.add(e);check(h.events.size()==1,"invalid live reports cannot poison aggregates");
 h.reset_requests();req=h.request("binancef","gtcusdt",from,now-3000,now,86400000,4000);response=make(req);
 h.connection(false,now);h.connection(true,now+1000);History::receive(response);
 check(h.loaded.empty() && h.events.size()==1,"disconnect cancels stale response but preserves received records");
 req=h.request("binancef","gtcusdt",from,now-3000,now,86400000,5000);response=make(req);
 response["reports"]={{{"timestamp_ms",from+1000},{"price",0.1},{"avg_price",0.1},{"qty",5},{"is_buy",true},{"trade_timestamp_ms",0},{"filled_qty",0}},{{"timestamp_ms",now+1}}};
 History::receive(response);check(h.events.size()==1 && h.loaded.empty(),"malformed batch rejected atomically");
 h.reset_requests();req=h.request("binancef","gtcusdt",from,now-3000,now,86400000,6000);History::receive(make(req));
 check(h.covers(from,now-3000),"empty successful read is recorded, not a source completeness claim");
 check(h.request("binancef","gtcusdt",from,now-3000,now,86400000,7000).empty(),"fresh empty tail is not polled each frame");
 req=h.request("binancef","gtcusdt",from,now-3000,now+11000,86400000,18000);check(!req.empty(),"recent empty history is refreshed for delayed writes");
 response=make(req);
 h.connection(true,now+20000,2);History::receive(response);
 check(h.loaded.empty(),"connection generation rejects old responses even without a disconnected render frame");
 History batch;batch.connection(true,now);
 const int64_t six_hours=24*History::tile_ms;
 auto wide=batch.request("binancef","gtcusdt",now-six_hours,now-3000,now,86400000,1000);
 check(wide["data"]["from_ms"]==now-six_hours,"six-hour viewport loads in one bounded request");
 History::receive(make(wide));
 check(batch.loaded.size()==24 && batch.covers(now-six_hours,now-3000),"batch records every covered tile");
 auto older=batch.request("binancef","gtcusdt",now-2*six_hours,now-3000,now,86400000,1400);
 check(older["data"]["to_ms"]==now-six_hours && older["data"]["from_ms"]==now-2*six_hours,"zoom-out reuses loaded history");
 History::receive(make(older));
 auto tail=batch.request("binancef","gtcusdt",now-2*six_hours,now,now+11000,86400000,13000);
 check(tail["data"]["to_ms"].get<int64_t>()-tail["data"]["from_ms"].get<int64_t>()<=History::tile_ms,"tail refresh stays small");
 batch.reset_requests();
 wide=batch.request("binancef","gtcusdt",now-six_hours,now-3000,now,86400000,20000);
 History::receive({{"request_id",wide["data"]["request_id"]},{"error","report_limit"}});
 auto split=batch.request("binancef","gtcusdt",now-six_hours,now-3000,now,86400000,20400);
 check(batch.loaded.empty() && split["data"]["from_ms"]==now-six_hours/2,"dense batches shrink without claiming coverage");
 History::receive({{"request_id",split["data"]["request_id"]},{"error","outside_history_window"}});
 auto legacy=batch.request("binancef","gtcusdt",now-six_hours,now-3000,now,86400000,20800);
 check(legacy["data"]["from_ms"]==now-History::tile_ms,"old server falls back to fifteen-minute requests");
 History::receive(make(wide));
 check(batch.loading() && batch.loaded.empty(),"late batch cannot complete replacement request");
 History::receive(make(legacy));
 check(batch.covers(now-History::tile_ms,now-3000) && !batch.covers(now-six_hours,now-3000),"fallback coverage stays honest");
 History full;full.connection(true,now);
 Event small{.price=1,.avg_price=1,.qty=1,.is_buy=true,.timestamp_ms=from};
 for(size_t n=0;n<=History::max_events;++n){small.timestamp_ms=from+int64_t(n);full.add(small);}
 check(full.limited && full.events.size()==History::max_events,"cache has an explicit retention bound");
 full.reload();
 check(!full.limited && full.events.empty() && !full.request("binancef","gtcusdt",from,now-3000,now,86400000,1000).empty(),"explicit reload recovers an exhausted cache for a new viewport");
 // A candle view several days wide loads all of it, newest first, a day per request (it used to stop at the
 // newest 24 h of the view, so older candles never showed their liquidations).
 {
  History days;days.connection(true,now);
  const int64_t day=History::day_tiles*History::tile_ms;
  const int64_t lookback=90*day;
  int64_t expect_to=now-3000;
  for(int k=0;k<4;++k){
   auto r=days.request("binancef","gtcusdt",now-3*day-3600000,now-3000,now,lookback,1000+1000*k);
   check(!r.empty(),"each older day of a wide view is requested");
   if(r.empty())break;
   check(r["data"]["to_ms"]==expect_to,"newest first, contiguous");
   check(r["data"]["to_ms"].get<int64_t>()-r["data"]["from_ms"].get<int64_t>()<=day,"at most a day per request");
   expect_to=r["data"]["from_ms"].get<int64_t>();
   History::receive(make(r));
  }
  check(days.covers(now-3*day-3600000,now-3000),"the whole three-day view is covered");
  check(days.request("binancef","gtcusdt",now-3*day-3600000,now-3000,now,lookback,6000).empty(),"a settled view asks nothing more");
  check(days.request("binancef","gtcusdt",now-200*day,now-180*day,now,lookback,7000).empty(),"before the lookback nothing is asked");
  // A server before 2026-10-11 refuses a day: drop to six hours, not fifteen minutes.
  History refused;refused.connection(true,now);
  auto r=refused.request("binancef","gtcusdt",now-2*day,now-3000,now,lookback,1000);
  History::receive({{"request_id",r["data"]["request_id"]},{"error","outside_history_window"}});
  r=refused.request("binancef","gtcusdt",now-2*day,now-3000,now,lookback,1400);
  check(r["data"]["to_ms"].get<int64_t>()-r["data"]["from_ms"].get<int64_t>()==6*3600000-3000,"a day refused falls back to six hours");
 }
 // Bybit reports bankruptcy prices; a real-time diamond sits on the forced side's print.
 {
  struct Trade{double price;int64_t timestamp_ms;bool is_buy;};
  std::vector<Trade> tape{{0.1040,1000,true},{0.1041,1500,false},{0.1043,2100,true},{0.1039,2600,false},{0.1050,9000,true}};
  check(std::abs(observed_liquidations::tape_price(tape,2000,false)-0.1041)<1e-12,"forced sell: the nearest sell print");
  check(std::abs(observed_liquidations::tape_price(tape,2000,true)-0.1043)<1e-12,"forced buy: the nearest buy print");
  check(std::abs(observed_liquidations::tape_price(tape,6000,true)-0.1039)<1e-12,"no side print near: the last print within 5 s");
  check(std::isnan(observed_liquidations::tape_price(tape,30000,true)),"no print near: no place on the tape");
  check(std::string(observed_liquidations::feed_line("bybit")).find("every liquidation")!=std::string::npos,"Bybit's feed is complete");
  check(observed_liquidations::sampled_feed("binancef") && !observed_liquidations::sampled_feed("bybit"),"only Binance is sampled");
  char usd[32];observed_liquidations::hover_usd(2650.674,usd,sizeof(usd));check(std::string(usd)=="$2,650.67","exact dollars under 100K");
  observed_liquidations::hover_usd(597800,usd,sizeof(usd));check(std::string(usd)=="$597.8K","compact dollars above");
 }
 std::printf("reported liquidation cache and aggregation: %d failures\n",failures);return failures?1:0;
}

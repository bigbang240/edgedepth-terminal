#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/touch_odds.h"
#include "touch_odds_lr_fixture.h"
#include <cassert>
using namespace touch_odds;
using Json = nlohmann::json;
Json frame(){
 Json bands=Json::array();
 for(int side:{-1,1})for(int off=1;off<=3;off++){double c=100*std::exp(side*off*std::log(1.0025));bands.push_back({{"side",side},{"offset",off},{"low",c/1.00125},{"high",c*1.00125},{"p",.9/off}});}
 return {{"v",1},{"venue","binancef"},{"symbol","aaveusdt"},{"status","scored"},{"asof",1000},{"expires_at",1201000},{"hide_at",601000},{"mark",100.0},{"bands",bands},
  {"cascade",{{"tier","top1"},{"hour_start",0},{"hour_end",3600000}}},{"server_now",31000}};
}
int main(){
 { auto j=frame(); j["history"]=true; j["available_at"]=31000; Snapshot past; assert(parse(j,past)); assert(past.history); assert(!past.bands_at(30999)); assert(past.bands_at(31000)); }
 Snapshot s;assert(parse(frame(),s));assert(s.band_count==6&&s.tier==Tier::Top1&&s.symbol=="aaveusdt");
 assert(s.bands_at(30000)&&s.bands_at(600999)&&!s.bands_at(601000));assert(s.tier_at(30000)&&!s.tier_at(3600000));
 // Every malformed variant leaves the previous snapshot untouched.
 for(int which=0;which<12;which++){
  auto bad=frame();
  switch(which){
   case 0:bad["v"]=2;break; case 1:bad["venue"]="kraken";break; case 2:bad["symbol"]="AAVEUSDT";break;
   case 3:bad["bands"][0]["p"]=1.2;break; case 4:bad["bands"][1]=bad["bands"][0];break; case 5:bad["bands"][2]["high"]=1;break;
   case 6:bad["bands"].erase(0);break; case 7:bad["hide_at"]=2000000;break; case 8:bad["cascade"]["tier"]="top10";break;
   case 9:bad["mark"]="100";break; case 10:bad["bands"][0]["side"]=0;break; default:bad["asof"]=1.5;break;
  }
  Snapshot keep=s;assert(!parse(bad,keep));assert(keep.band_count==6&&keep.asof==1000);
 }
 // Uncovered markets carry a reason and no bands; the tier can still arrive alone.
 auto held=frame();held["status"]="outside_frozen_universe";held["bands"]=Json::array();held.erase("asof");
 assert(parse(held,s)&&s.band_count==0&&!s.bands_at(30000)&&s.tier_at(30000));
 assert(std::string(missing_reason(s.status))=="not in the tested market set");
 held["bands"]=frame()["bands"];assert(!parse(held,s));
 held["status"]="mystery";held["bands"]=Json::array();assert(!parse(held,s));
 auto none=frame();none["status"]="no_current_frame";none["bands"]=Json::array();none["cascade"]=nullptr;
 assert(parse(none,s)&&s.tier==Tier::None&&!s.tier_at(30000));
 assert(!parse(Json::parse("[1,2]"),s));assert(!parse(Json(),s));
 // Long range: parity with the research code at every fixture case, and refusal of broken models.
 auto lr=frame();lr["longrange"]={{"asof",1000},{"hide_at",601000},{"mark",100.0},{"rv",{.002,.003,.004}},{"distance_range",{.005,.1}},{"horizons",Json::parse(kTouchLrHorizons)}};
 for(const auto& c:kTouchLrCases){
  lr["longrange"]["rv"]={c.rv[0],c.rv[1],c.rv[2]};Snapshot q;assert(parse(lr,q)&&q.longrange.at(600999)&&!q.longrange.at(601000));
  const Horizon* h=nullptr;for(const auto& x:q.longrange.horizons)if(x.name==c.horizon)h=&x;assert(h);
  const double got=probability(q.longrange,*h,100.0*(1+c.side*c.distance));
  assert(std::abs(got-c.p)<1e-9);
 }
 Snapshot q;parse(lr,q);assert(probability(q.longrange,q.longrange.horizons[0],100.3)<0&&probability(q.longrange,q.longrange.horizons[0],80.0)<0);
 assert(probability(q.longrange,q.longrange.horizons[2],101.0)>probability(q.longrange,q.longrange.horizons[2],103.0));
 assert(probability(q.longrange,q.longrange.horizons[2],102.0)>probability(q.longrange,q.longrange.horizons[0],102.0));
 for(int which=0;which<4;which++){
  auto bad=lr;
  switch(which){case 0:bad["longrange"]["rv"][1]=0;break;case 1:bad["longrange"]["horizons"].erase(0);break;
   case 2:bad["longrange"]["horizons"][0]["knots"]["p"][1]=0;break;default:bad["longrange"]["horizons"][1]["transform"]["sd"][0]=0;break;}
  Snapshot keep=s;assert(!parse(bad,keep));
 }
 // The same recorded state gives the same odds live and in replay. Reference
 // price is owned by the forecast, and availability is checked on both clocks.
 auto original=lr; original["longrange"]["available_at"]=31000;
 Snapshot live_snapshot; assert(parse(original,live_snapshot));
 original["history"]=true; original["available_at"]=31000;
 Snapshot replay_snapshot; assert(parse(original,replay_snapshot));
 assert(!live_snapshot.longrange.at(30999)&&live_snapshot.longrange.at(31000));
 assert(!replay_snapshot.longrange.at(30999)&&replay_snapshot.longrange.at(31000));
 for(size_t i=0;i<3;++i){
  const auto& live_lr=live_snapshot.longrange; const auto& replay_lr=replay_snapshot.longrange;
  const double live_p=probability(live_lr,live_lr.horizons[i],102.0);
  assert(live_p==probability(replay_lr,replay_lr.horizons[i],102.0));
  auto next=live_lr; next.mark=101.0;
  assert(std::abs(live_p-probability(next,next.horizons[i],102.0))>1e-8);
 }
 auto nolr=frame();nolr["longrange"]=nullptr;assert(parse(nolr,q)&&!q.longrange.valid);
 // Venue identity: kept, limited to served venues, and a reason-only payload on other venues.
 assert(parse(frame(),s)&&s.venue=="binancef"&&s.longrange_status.empty());
 for(const char* v:{"bybit","hl"}){
  Json other={{"v",1},{"venue",v},{"symbol","kpepe"},{"status","venue_not_supported"},{"bands",Json::array()},{"cascade",nullptr},{"longrange",nullptr},{"longrange_status","venue_not_supported"}};
  Snapshot o;assert(parse(other,o)&&o.venue==v&&o.band_count==0&&o.tier==Tier::None&&!o.longrange.valid&&!o.bands_at(30000));
  assert(std::string(missing_reason(o.status))=="not yet available on this venue");
  other["bands"]=frame()["bands"];assert(!parse(other,o));
 }
 for(const char* v:{"","BINANCEF","binance"}){auto bad=frame();bad["venue"]=v;Snapshot keep=s;assert(!parse(bad,keep)&&keep.venue=="binancef");}
 { auto bad=frame();bad["venue"]=1;assert(!parse(bad,q)); }
 auto fail=frame();fail["status"]="forecast_unavailable";fail["bands"]=Json::array();fail["cascade"]=nullptr;fail["history"]=true;
 assert(parse(fail,q)&&q.history&&std::string(missing_reason(q.status))=="forecast unavailable");
 // Long-range absence reasons survive the bridge; unknown ones are refused.
 auto warm=frame();warm["longrange_status"]="warming";assert(parse(warm,q)&&q.band_count==6&&std::string(longrange_reason(q.longrange_status))=="1h/4h/24h odds warming up (needs 7 days of 5m marks)");
 for(const char* st:{"scored","no_current_frame","zero_volatility","not_covered","venue_not_supported","forecast_unavailable"}){auto x=frame();x["longrange_status"]=st;assert(parse(x,q));}
 assert(longrange_reason("scored")==nullptr&&longrange_reason("no_current_frame")==nullptr);
 // Hyperliquid: trade-price long-range odds for an admitted market, no bands or tier.
 { Json hl={{"v",1},{"venue","hl"},{"symbol","kpepe"},{"status","bands_not_tested"},{"price_kind","trade"},{"bands",Json::array()},{"cascade",nullptr},
     {"longrange",lr["longrange"]},{"longrange_status","scored"}};
   Snapshot h;assert(parse(hl,h)&&h.venue=="hl"&&h.price_kind=="trade"&&h.longrange.valid&&h.band_count==0&&h.tier==Tier::None);
   assert(std::string(missing_reason(h.status))=="20-min bands not tested on this venue");
   hl["longrange"]=nullptr;hl["longrange_status"]="not_admitted";assert(parse(hl,h)&&!h.longrange.valid&&std::string(longrange_reason(h.longrange_status))=="1h/4h/24h odds: data check pending");
   for(const char* st:{"uncounted_minutes","invalid_input"}){hl["longrange_status"]=st;assert(parse(hl,h)&&std::string(longrange_reason(h.longrange_status))=="1h/4h/24h odds: incomplete trade data");}
   hl["price_kind"]="index";Snapshot keep=h;assert(!parse(hl,keep)&&keep.price_kind=="trade");
   hl["price_kind"]=1;assert(!parse(hl,keep));
   auto bin=frame();bin["price_kind"]="mark";assert(parse(bin,h)&&h.price_kind=="mark"); }
 { auto bad=frame();bad["longrange_status"]="mystery";assert(!parse(bad,q)); bad["longrange_status"]=3;assert(!parse(bad,q)); }
 // Release access (web touchRelease.ts): older hosts send none and mean the admin preview.
 { Snapshot a; assert(parse(frame(),a)&&a.access=="admin_preview"&&!a.released()); }
 { auto r=frame(); r["access"]="free_taste"; r["release_label"]="Tested forward Oct 1-28 2026"; r["status"]="bands_not_released"; r["bands"]=Json::array(); r["cascade"]=nullptr;
   Snapshot a; assert(parse(r,a)&&a.released()&&a.release_label=="Tested forward Oct 1-28 2026"&&a.band_count==0&&a.tier==Tier::None);
   r.erase("release_label"); Snapshot b; assert(!parse(r,b));  // a released payload always names its record
   r["release_label"]=std::string(81,'x'); assert(!parse(r,b)); r["release_label"]="x"; r["access"]="vip"; assert(!parse(r,b)); }
 { Json locked={{"v",1},{"venue","binancef"},{"symbol","ethusdt"},{"status","locked"},{"access","locked"},{"release_label","Tested forward"},
   {"free_symbols",{"btcusdt"}},{"bands",Json::array()},{"cascade",nullptr},{"longrange",nullptr},{"longrange_status","locked"}};
   Snapshot a; assert(parse(locked,a)&&a.access=="locked"&&a.free_symbols.size()==1&&a.free_symbols[0]=="btcusdt"&&!a.released());
   auto leak=locked; leak["cascade"]={{"tier","top1"},{"hour_start",0},{"hour_end",3600000}}; Snapshot b; assert(!parse(leak,b));  // never a forecast when locked
   auto mismatch=locked; mismatch["access"]="released"; assert(!parse(mismatch,b));
   auto many=locked; many["free_symbols"]={"a","b","c","d"}; assert(!parse(many,b));
   auto nr=locked; nr["status"]="not_released"; nr["access"]="not_released"; nr["longrange_status"]="not_released"; nr.erase("free_symbols"); nr.erase("release_label");
   assert(parse(nr,b)&&b.access=="not_released"); }
 return 0;
}

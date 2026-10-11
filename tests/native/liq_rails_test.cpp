#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/liq_rails.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
using namespace liq_rails;
int main(){
 // A fake touch model: chance falls with distance and rises with horizon; negative outside 0.5%-10%.
 auto prob=[](double level,int h){const double d=std::abs(level/100.0-1.0);if(d<0.005*(1-1e-9)||d>0.10*(1+1e-9))return -1.0;return std::min(1.0,(0.02*(h+1))/d*0.1);};
 const std::vector<touch_zones::Fuel> f={{101.6,5},{101.7,3},{98.0,4},{100.2,10},{99.9,2},{115.0,40},{80.0,40},{100.7,1}};
 const auto r=build(f,100.0,0.005,0.10,std::log(1.01),prob);
 // 115 and 80 are past 10%; 101.6 and 101.7 share a bin; 100.2 and 99.9 sit on opposite sides.
 double above=0,below=0;
 for(const auto& x:r){(x.side>0?above:below)+=x.fuel;assert(x.side==(x.price>100.0?1:-1));}
 assert(above==5+3+10+1&&below==4+2);  // all in-range fuel kept, on its own side
 for(size_t i=1;i<r.size();++i)assert(r[i-1].price<r[i].price);
 for(const auto& x:r)for(int h=0;h<3;++h){assert(x.p[h]>=0&&std::abs(x.w[h]-x.fuel*x.p[h])<1e-12);if(h)assert(x.p[h]>=x.p[h-1]);}
 // The 101.6/101.7 bin sits at its fuel-weighted price.
 bool merged=false;for(const auto& x:r)if(x.fuel==8){merged=true;assert(std::abs(x.price-(101.6*5+101.7*3)/8)<1e-9);}
 assert(merged);
 // Nearer than 0.5%: the 0.5% chance on that side, a floor.
 for(const auto& x:r)if(std::abs(x.price/100.0-1.0)<0.005)for(int h=0;h<3;++h)assert(std::abs(x.p[h]-prob(100.0*(1+x.side*0.005),h))<1e-12);
 // Strongest: per side, separated, strongest first.
 const auto s=strongest(r,1,1,2,0.01);
 assert(!s.empty()&&s.size()<=2&&s[0].side==1);
 if(s.size()==2){assert(s[0].w[1]>=s[1].w[1]);assert(std::abs(std::log(s[0].price/s[1].price))>=0.01);}
 assert(build(f,0.0,0.005,0.10,0.01,prob).empty()&&build({},100.0,0.005,0.10,0.01,prob).empty());
 // History: only fuel standing at each point's time counts, weighted by that point's own chance, on fixed rows.
 { const double bw=std::log(1.0006), bin=std::log(1.01);
   std::vector<Run> runs={{101.6,101.6*std::exp(2*bw),5.0,0,200},   // three buckets, gone at t=200
                          {98.0,98.0,4.0,100,INT64_MAX},             // appears at t=100, never consumed
                          {150.0,150.0,9.0,0,INT64_MAX}};            // beyond 10%: never drawn
   std::vector<Point> pts={{50,100.0},{150,100.0},{250,100.0}};
   auto chance=[&](size_t k,double level){return prob(level,1)*(k==2?2.0:1.0);};
   auto cells=history_cells(runs,bw,pts,0.005,0.10,bin,chance);
   auto at=[&](int64_t t,int side){double w=0;for(const auto& c:cells)if(c.t==t&&c.side==side)w+=c.w;return w;};
   assert(at(50,1)>0&&at(50,-1)==0);              // before the long run starts: shorts only
   assert(at(150,1)>0&&at(150,-1)>0);             // both standing
   assert(at(250,1)==0&&at(250,-1)>0);            // the short run was consumed at 200
   assert(std::abs(at(250,-1)-2.0*4.0*prob(98.0,1))<1e-9);              // the point's own chance
   long long r1=-1,r2=-2;for(const auto& c:cells){if(c.side<0&&c.t==150)r1=c.row;if(c.side<0&&c.t==250)r2=c.row;}
   assert(r1==r2&&r1==(long long)std::floor(std::log(98.0)/bin));       // the same level keeps its row
   for(const auto& c:cells)assert(c.price<111&&c.price>89);
   assert(history_cells(runs,0.0,pts,0.005,0.10,bin,chance).empty()); }
 // Scenario segments become runs; a still-standing segment never ends.
 { struct Seg{double price_lo,price_hi,intensity;int64_t start_ms,end_ms;};
   const int64_t pending=-7;
   const auto rs=runs_from_segments(std::vector<Seg>{{99.0,99.5,3.0,10,20},{101.0,101.0,2.0,15,pending}},pending);
   assert(rs.size()==2&&rs[0].lo==99.0&&rs[0].hi==99.5&&rs[0].fuel==3.0&&rs[0].start==10&&rs[0].end==20);
   assert(rs[1].end==INT64_MAX); }
 // Observed fuel keeps each band's long and short notional with its side; empty or bad bands are skipped.
 { struct Band{double price_mid,est_long_usd,est_short_usd;};
   const std::vector<Band> bands={{95.0,1000.0,0.0},{104.0,0.0,500.0},{102.0,300.0,0.0},{0.0,9.0,9.0},{97.0,0.0,0.0},{98.0,200.0,50.0}};
   const auto obs=observed(bands,1234,0.25);
   assert(obs.kind==FuelKind::Observed&&obs.asof_ms==1234&&obs.coverage==0.25);
   assert(obs.standing.size()==5&&obs.side.size()==5);
   // From 100: longs below and shorts above stand; a long at 102 and a short at 98 are on the wrong side.
   double dropped=-1;
   const auto at=standing_at(obs,100.0,&dropped);
   double longs=0,shorts=0;for(const auto& x:at)(x.price<100.0?longs:shorts)+=x.mass;
   assert(longs==1000.0+200.0&&shorts==500.0&&dropped==300.0+50.0);
   // A move up turns the 102 long into fuel below price; a short above stays only while above.
   double d2=0;const auto up=standing_at(obs,103.0,&d2);
   double l2=0;for(const auto& x:up)if(x.price<103.0)l2+=x.mass;
   assert(l2==1000.0+300.0+200.0&&d2==50.0);
   // Mismatched sides on an observed source draw nothing rather than guessing.
   auto broken=obs;broken.side.pop_back();
   assert(standing_at(broken,100.0).empty());
   // Scenario fuel has no side: all of it stands, and nothing is dropped.
   FuelSource sc;sc.standing={{95.0,1.0},{105.0,2.0}};
   double d3=-1;assert(standing_at(sc,100.0,&d3).size()==2&&d3==0); }
 // Modelled (V2) fuel keeps each level's side like observed fuel and names its claim.
 { struct Level{double price,long_usd,short_usd;};
   const auto mo=modelled(std::vector<Level>{{95.0,2e6,0.0},{101.0,0.0,3e6},{0.0,1.0,1.0},{99.0,0.0,0.0}},5000,"tested");
   assert(mo.kind==FuelKind::Modelled&&mo.asof_ms==5000&&mo.standing.size()==2&&mo.side.size()==2);
   double dropped=-1;
   assert(standing_at(mo,100.0,&dropped).size()==2&&dropped==0);
   assert(standing_at(mo,102.0,&dropped).size()==1&&dropped==3e6);  // the short at 101 is behind price
   char out[300];caption(mo,65000,"",out,sizeof(out));
   assert(std::string(out).find("V2 modelled $ (tested, 60s old)")!=std::string::npos);
   assert(std::string(out).find("not a magnet")!=std::string::npos); }
 // The caption names the fuel and always says it is not a magnet.
 { char out[300];
   FuelSource sc;
   caption(sc,0,"",out,sizeof(out));
   assert(std::string(out)=="RAILS \xc2\xb7 brightness = touch chance x scenario fuel \xc2\xb7 not a magnet");  // unchanged wording
   sc.fallback="HL positions stale";
   caption(sc,0," \xc2\xb7 history: recorded 4h chance",out,sizeof(out));
   assert(std::string(out).find("scenario fuel (HL positions stale)")!=std::string::npos);
   assert(std::string(out).find("not a magnet \xc2\xb7 history: recorded 4h chance")!=std::string::npos);
   FuelSource ob;ob.kind=FuelKind::Observed;ob.asof_ms=1000;ob.coverage=0.234;
   caption(ob,46000,"",out,sizeof(out));
   assert(std::string(out).find("Hyperliquid positions (sampled, 23% located, 45s old)")!=std::string::npos);
   assert(std::string(out).find("not a magnet")!=std::string::npos);
   ob.coverage=-1;caption(ob,1000+300000,"",out,sizeof(out));
   assert(std::string(out).find("coverage unknown, 5m old")!=std::string::npos); }
 return 0;
}

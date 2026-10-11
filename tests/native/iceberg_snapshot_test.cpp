#include "core/iceberg_snapshot.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
static void check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr,"%s\n",why); std::exit(1); } }
int main() {
 const int64_t now=1000000;
 IcebergSnapshot state, in{now,now-1000,now-300000,now,now-600000,2,1,200,100,false,.6,10000,4};
 check(!state.fresh(now),"missing is not a fresh zero");
 check(state.apply(in,now) && state.fresh(now),"valid snapshot rejected");
 check(std::abs(state.bias()-1.0/3.0)<1e-9,"bias uses observed notional");
 check(!state.fresh(now+90001),"snapshot did not expire");
 auto stale=in;stale.source_trade_ms=now-90001; check(state.apply(stale,now)&&!state.fresh(now),"timer revives stale trade source");
 auto bad=in;bad.timestamp_ms--;check(!state.apply(bad,now),"late update accepted");
 bad=in;bad.buy_usd=std::numeric_limits<double>::quiet_NaN();check(!state.apply(bad,now),"NaN accepted");
 bad=in;bad.buy_clusters=0;check(!state.apply(bad,now),"contradictory counts accepted");
 bad=in;bad.timestamp_ms++;bad.end_ms++;bad.start_ms++;check(!state.apply(bad,now),"future snapshot accepted");
 in.buy_clusters=0;in.sell_clusters=0;in.buy_usd=0;in.sell_usd=0;
 check(state.apply(in,now)&&state.fresh(now)&&state.bias()==0,"measured zero lost");
 in.detector_start_ms=now-30000;in.history_limited=true;
 check(state.apply(in,now)&&state.warming()&&state.history_limited,"partial coverage lost");
 std::puts("PASS iceberg freshness, missingness and observed-flow semantics");
}

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/stats_oi_history.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main() {
    StatsOIHistory h; const int64_t now=1800000000000LL+30000;
    assert(h.add(now, 120, now));
    double d=0; assert(!h.change(3600000,now,d));
    // Historical response arrives AFTER the current live value.
    for(int i=245;i>=1;--i) assert(h.seed(now/60000*60000-i*60000,100,now));
    assert(!h.seed(now/60000*60000,1000,now));
    assert(h.change(3600000,now,d) && std::abs(d-.2)<1e-9);
    assert(h.change(4*3600000,now,d) && std::abs(d-.2)<1e-9);
    assert(h.change(60000,now,d,true) && d>0 && d<.2);
    assert(!h.change(60000,now+120001,d));
    assert(!h.add(now+1,100,now));
    assert(!h.add(now,std::numeric_limits<double>::quiet_NaN(),now));
    assert(!h.add(now,0,now));
    StatsOIHistory gap; gap.add(now-3600000-120000,100,now); gap.add(now,200,now);
    assert(!gap.change(3600000,now,d));
    // High-rate updates remain bounded by minute and preserve a 4h anchor.
    for(int i=1;i<=5*3600;++i) h.add(now+i*1000,120,now+i*1000);
    assert(h.change(4*3600000,now+5*3600000,d) && std::abs(d)<1e-9);
    std::puts("PASS OI history: units, late seed, partial minute, anchors, gaps, retention");
}

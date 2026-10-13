// SPDX-License-Identifier: GPL-3.0-only
#include "resolve_ownership.h"
#include <iostream>
using namespace bo3::native;
int main() {
  uint32_t checks=0; auto check=[&](bool ok){if(!ok) throw std::runtime_error("ownership interval mismatch");++checks;};
  try {
    UnavailableResolveRanges r; r.Add(100,100); r.Add(300,50);
    check(!r.Intersects(200,100)); check(r.Intersects(199,1)); check(r.Intersects(99,2));
    r.CpuWritten(120,20); check(!r.Intersects(120,20)); check(r.Intersects(119,1)); check(r.Intersects(140,1));
    r.Add(110,40); check(r.Intersects(120,20) && r.ranges().size()==2);
    r.Add(200,100); check(r.ranges().size()==1 && r.ranges()[0].begin==100 && r.ranges()[0].end==350);
    r.CpuWritten(90,310); check(r.ranges().empty());
    r.Add(0x1fffffff,1); check(r.Intersects(0x1fffffff,1)); check(!r.Intersects(0x20000000,0));
    bool overflow=false; try {r.Add(0x1fffffff,2);}catch(...){overflow=true;} check(overflow && r.ranges().size()==1);
    bool exhausted=false; UnavailableResolveRanges full;
    for(uint32_t i=0;i<512;++i) full.Add(i*4,1);
    try {full.Add(2048,1);}catch(...){exhausted=true;} check(exhausted && full.ranges().size()==512);
    std::cout<<checks<<" resolve ownership checks passed; no game launched\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

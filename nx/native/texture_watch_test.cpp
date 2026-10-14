// SPDX-License-Identifier: GPL-3.0-only
#include "texture_validity.h"
#include <rex/system/xmemory.h>
#include <iostream>
#include <stdexcept>
#include <atomic>
using namespace bo3::native;
struct Watch {
  TextureValidity validity;
  TextureValidity other;
  uint32_t calls=0;
  static std::pair<uint32_t,uint32_t> Invalidated(void* context,uint32_t address,uint32_t bytes,bool) {
    auto& self=*static_cast<Watch*>(context);++self.calls;
    std::cout<<"callback range "<<std::hex<<address<<" + "<<bytes<<std::dec<<'\n';
    self.validity.Invalidate(address,bytes);self.other.Invalidate(address,bytes);return {address,bytes};
  }
};
int main() {
 try {
  uint32_t checks=0;
  auto check=[&](bool value,const char* name){if(!value)throw std::runtime_error(name);++checks;};
  rex::memory::Memory memory;
  if(!memory.Initialize())throw std::runtime_error("SDK memory initialization");
  auto* heap=memory.LookupHeapByType(true,4096);uint32_t address=0;
  if(!heap || !heap->Alloc(8192,4096,rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite,false,&address))throw std::runtime_error("physical test allocation");
  const uint32_t physical=memory.GetPhysicalAddress(address);
  Watch watch{{physical,4096,physical+4096,4096,true},{physical+4096,4096,0,0,true}};
  auto* handle=memory.RegisterPhysicalMemoryInvalidationCallback(Watch::Invalidated,&watch);
  memory.EnablePhysicalMemoryAccessCallbacks(physical,8192,true,false);
  volatile auto* first=memory.TranslateVirtual<uint8_t*>(address);
  first[8]=1;
  // The MMIO write-fault handler calls back outside ordinary C++ flow.
  // Force reloads of handler state after each faulting store in this fixture.
  std::atomic_signal_fence(std::memory_order_seq_cst);
  check(watch.calls>0 && !watch.validity.clean,"real virtual write fault invalidates backing");
  uint32_t calls=watch.calls;first[9]=2;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  check(watch.calls==calls,"write watch is one-shot per page");
  const bool other_invalidated=!watch.other.clean;
  first[4096]=3;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  std::cout<<"after page2 write: calls="<<watch.calls<<" prior="<<calls<<" otherClean="<<watch.other.clean<<'\n';
  check(other_invalidated || (watch.calls>calls && !watch.other.clean),"separate resource cannot stay clean after a mip page write");
  watch.validity.clean=true;memory.EnablePhysicalMemoryAccessCallbacks(physical,8192,true,false);
  calls=watch.calls;first[4096]=4;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  check(watch.calls>calls && !watch.validity.clean,"mip page write invalidates rearmed image");
  watch.validity.clean=true;memory.EnablePhysicalMemoryAccessCallbacks(physical,4096,true,false);
  calls=watch.calls;first[10]=4;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  check(watch.calls>calls && !watch.validity.clean,"dirty page can be rearmed");
  watch.validity.clean=true;memory.EnablePhysicalMemoryAccessCallbacks(physical,4096,true,false);
  calls=watch.calls;memory.TranslatePhysical<uint8_t*>(physical)[11]=5;
  check(watch.calls==calls,"direct physical mapping bypasses guest write watches");
  // Production native exports invalidate explicitly, not via a fault.
  watch.validity.Invalidate(physical+11,1);check(!watch.validity.clean,"explicit native write invalidates image");
  watch.validity.clean=true;watch.validity.Invalidate(physical+8192,1);check(watch.validity.clean,"adjacent bytes do not invalidate image");
  watch.validity.Invalidate(physical+4095,2);check(!watch.validity.clean,"write spanning base/mips invalidates");
  watch.validity.clean=true;
  memory.EnablePhysicalMemoryAccessCallbacks(physical,8192,true,false);
  heap->Release(address);check(!watch.validity.clean,"physical allocation release invalidates image");
  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
  std::cout<<checks<<" texture write-watch checks passed; no game or GPU rendering launched\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

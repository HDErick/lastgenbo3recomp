/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/platform.h>
#include <rex/system/flags.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

// Synthesizing a service endpoint locally is what lets a title's back-end
// enumeration succeed without reaching Microsoft's Live Server Platform. Empty
// address restores the old refuse-everything behaviour.
REXCVAR_DEFINE_STRING(xonline_service_address, "127.0.0.1", "Network",
                      "Address reported for every XOnlineGetServiceInfo query "
                      "(empty = report no service)");
REXCVAR_DEFINE_UINT32(xonline_service_port, 30003, "Network",
                      "Port reported for every XOnlineGetServiceInfo query");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      //
      // Returning ERROR_CONNECTION_INVALID here is what stops Black Ops 2 ever
      // reaching a back-end: Live_Base_StartLSPEnumeration calls
      // XOnlineGetServiceInfo and XTitleServerCreateEnumerator, and with no
      // service info the enumeration task never completes -- neither
      // Live_Base_TitleServerEnumerationSuccess nor ...Failure runs, so
      // Live_Base_ConnectToLobby (the Demonware connect) is never called.
      //
      // Unlike the other messages, arg1/arg2 are NOT (buffer, length) here: arg1 is
      // dwServiceID and arg2 is the guest pointer to the XONLINE_SERVICE_INFO to
      // fill. (Established from a live call: arg1 came through as 0x415607E9, a
      // service id, and treating it as a buffer address faults.)
      const uint32_t service_id = buffer_ptr;
      const uint32_t service_info_ptr = buffer_length;

      const std::string target = REXCVAR_GET(xonline_service_address);
      if (target.empty()) {
        REXKRNL_INFO("CXLiveLogon::GetServiceInfo(service={:08X}) -> refused", service_id);
        return 0x80151802;  // ERROR_CONNECTION_INVALID
      }

      uint32_t addr = inet_addr(target.c_str());
      if (addr == INADDR_NONE) {
        REXKRNL_ERROR("xonline_service_address is not a valid IPv4 address: '{}'", target);
        return 0x80151802;
      }
      const uint16_t port = static_cast<uint16_t>(REXCVAR_GET(xonline_service_port));

      // Black Ops 2 first probes with pServiceInfo == nullptr just to learn whether
      // the service exists, then asks again with a real buffer. Report success for
      // the probe rather than treating it as a bad request.
      if (service_info_ptr) {
        // XONLINE_SERVICE_INFO { u32 dwServiceID; IN_ADDR ina; IN_ADDR inaOnline;
        //                        u16 wServicePort; u16 wReserved; u32 dwFlags; }
        auto* info = memory_->TranslateVirtual(service_info_ptr);
        std::memset(info, 0, 20);
        memory::store_and_swap<uint32_t>(info + 0, service_id);
        // IN_ADDR holds network byte order already, so copy the bytes rather than
        // byte-swapping them into guest order.
        std::memcpy(info + 4, &addr, 4);
        std::memcpy(info + 8, &addr, 4);
        memory::store_and_swap<uint16_t>(info + 12, port);
      }

      REXKRNL_INFO("CXLiveLogon::GetServiceInfo(service={:08X}) -> {}:{}", service_id, target,
                   port);
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex

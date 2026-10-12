/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

#include <rex/platform.h>
#include <rex/system/flags.h>

#include <cstring>
#include <string>

#include <fmt/format.h>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

// Defined alongside the XOnlineGetServiceInfo handler in xlivebase_app.cpp; the
// synthetic title-server entry has to advertise the same host.
REXCVAR_DECLARE(std::string, xonline_service_address);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L518
uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, mapped_void buffer_ptr,
                        uint32_t buffer_size, uint32_t* items_returned, uint32_t overlapped_ptr) {
  assert_true(flags == 0);

  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer_ptr.guest_address(), buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length,
                       mapped_u32 items_returned, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result =
      xeXamEnumerate(handle, flags, buffer, buffer_length,
                     !overlapped.guest_address() ? &dummy : nullptr, overlapped.guest_address());
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}

// Live Server Platform enumeration, as issued by the xonline library statically
// linked into a title. Black Ops 2's XTitleServerCreateEnumerator calls
// XamCreateEnumeratorHandle(user=0xFFFFFFFF, app=0xFC, open=0x00058039,
// close=0x0005803A, item_size=212, item_count=cItems, flags=0, &handle) and
// propagates any failure straight out as ERROR_INVALID_PARAMETER, which is why
// hard-failing here stalled the title's whole back-end handshake.
static constexpr uint32_t kTitleServerOpenMessage = 0x00058039;
static constexpr uint32_t kTitleServerCloseMessage = 0x0005803A;

// XTITLE_SERVER_INFO { IN_ADDR inaServer; DWORD dwFlags; CHAR szServerDescription[200]; }
// 208 bytes, matching the 208-byte stride the title uses when it walks the results.
// The title asks for 212-byte items, so there are 4 bytes of slack at the end.
static void AppendSyntheticTitleServer(XStaticUntypedEnumerator* e) {
  const std::string target = REXCVAR_GET(xonline_service_address);
  if (target.empty()) {
    return;
  }
  uint32_t addr = inet_addr(target.c_str());
  if (addr == INADDR_NONE) {
    REXKRNL_ERROR("xonline_service_address is not a valid IPv4 address: '{}'", target);
    return;
  }
  uint8_t* item = e->AppendItem();
  if (!item) {
    return;
  }
  std::memset(item, 0, e->item_size());
  // IN_ADDR is already network byte order; copy the bytes rather than swapping.
  std::memcpy(item + 0, &addr, 4);
  const char kDescription[] = "rexglue";
  std::memcpy(item + 8, kDescription, sizeof(kDescription));
  REXKRNL_INFO("XamCreateEnumeratorHandle: advertising title server {}", target);
}

u32 XamCreateEnumeratorHandle_entry(u32 user_index, u32 app_id, u32 open_message,
                                    u32 close_message, u32 item_size, u32 item_count, u32 flags,
                                    mapped_u32 handle_out) {
  if (!item_size || !item_count || !handle_out) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), item_count, item_size);
  auto result = e->Initialize(user_index, app_id, open_message, close_message, flags);
  if (XFAILED(result)) {
    return result;
  }

  if (open_message == kTitleServerOpenMessage && close_message == kTitleServerCloseMessage) {
    AppendSyntheticTitleServer(e.get());
  }

  *handle_out = e->handle();
  REXKRNL_INFO(
      "XamCreateEnumeratorHandle(user={:08X}, app={:08X}, open={:08X}, close={:08X}, "
      "item_size={}, item_count={}) -> handle {:08X}",
      user_index, app_id, open_message, close_message, item_size, item_count, e->handle());
  return X_ERROR_SUCCESS;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, mapped_u32 out_object_ptr) {
  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  e->RetainHandle();

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e->guest_object();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamEnumerate, rex::kernel::xam::XamEnumerate_entry)
REX_EXPORT(__imp__XamCreateEnumeratorHandle, rex::kernel::xam::XamCreateEnumeratorHandle_entry)
REX_EXPORT(__imp__XamGetPrivateEnumStructureFromHandle,
           rex::kernel::xam::XamGetPrivateEnumStructureFromHandle_entry)

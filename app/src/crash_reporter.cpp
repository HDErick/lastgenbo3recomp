#include "crash_reporter.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

namespace bo3 {
namespace {

// Written with stdio rather than the runtime's logger: spdlog may be mid-format
// on this thread, or the crash may be inside logging itself.
FILE* OpenReport(const wchar_t* name = L"crash.txt") {
  wchar_t exe_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return nullptr;
  }
  wchar_t* slash = wcsrchr(exe_path, L'\\');
  if (slash) {
    *(slash + 1) = L'\0';
  }
  wchar_t report_path[MAX_PATH];
  _snwprintf_s(report_path, MAX_PATH, _TRUNCATE, L"%s%s", exe_path, name);
  FILE* f = nullptr;
  _wfopen_s(&f, report_path, L"w");
  return f;
}

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
    case 0xC0000409:                      return "STACK_BUFFER_OVERRUN/__fastfail";
    default:                              return "?";
  }
}

void WriteFrame(FILE* f, int index, DWORD64 addr) {
  // SYMBOL_INFO's name is a trailing array; give it room for mangled C++ names.
  alignas(SYMBOL_INFO) unsigned char sym_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
  std::memset(sym_buf, 0, sizeof(sym_buf));
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = MAX_SYM_NAME;

  DWORD64 displacement = 0;
  const char* name = "<no symbol>";
  if (SymFromAddr(GetCurrentProcess(), addr, &displacement, sym)) {
    name = sym->Name;
  }

  char module_name[MAX_PATH] = "?";
  IMAGEHLP_MODULE64 mod{};
  mod.SizeOfStruct = sizeof(mod);
  if (SymGetModuleInfo64(GetCurrentProcess(), addr, &mod)) {
    std::snprintf(module_name, sizeof(module_name), "%s", mod.ModuleName);
  }

  IMAGEHLP_LINE64 line{};
  line.SizeOfStruct = sizeof(line);
  DWORD line_disp = 0;
  if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &line_disp, &line)) {
    std::fprintf(f, "  #%-2d 0x%016llX  %s!%s+0x%llX  (%s:%lu)\n", index,
                 static_cast<unsigned long long>(addr), module_name, name,
                 static_cast<unsigned long long>(displacement), line.FileName, line.LineNumber);
  } else {
    std::fprintf(f, "  #%-2d 0x%016llX  %s!%s+0x%llX\n", index,
                 static_cast<unsigned long long>(addr), module_name, name,
                 static_cast<unsigned long long>(displacement));
  }
}

LONG CALLBACK OnUnhandledException(PEXCEPTION_POINTERS info) {
  FILE* f = OpenReport();
  if (!f) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  std::fprintf(f, "unhandled exception 0x%08lX (%s) at 0x%016llX  thread %lu\n", rec->ExceptionCode,
               ExceptionName(rec->ExceptionCode),
               reinterpret_cast<unsigned long long>(rec->ExceptionAddress), GetCurrentThreadId());
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    static const char* kOps[] = {"read", "write", "execute"};
    ULONG_PTR op = rec->ExceptionInformation[0];
    std::fprintf(f, "  %s of 0x%016llX\n", op <= 2 ? kOps[op] : "?",
                 static_cast<unsigned long long>(rec->ExceptionInformation[1]));
  }

  const CONTEXT* c = info->ContextRecord;
  std::fprintf(f,
               "\nrip=%016llX rsp=%016llX rbp=%016llX\n"
               "rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n"
               "rsi=%016llX rdi=%016llX r8 =%016llX r9 =%016llX\n"
               "r10=%016llX r11=%016llX r12=%016llX r13=%016llX\n"
               "r14=%016llX r15=%016llX\n\n",
               c->Rip, c->Rsp, c->Rbp, c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8,
               c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);

  // StackWalk64 writes through its context, so hand it a copy.
  CONTEXT walk_context = *c;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = walk_context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = walk_context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = walk_context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;

  std::fprintf(f, "stack:\n");
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame,
                     &walk_context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                     nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }
    WriteFrame(f, i, frame.AddrPC.Offset);
  }

  std::fflush(f);
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;  // still let the process die normally
}

void OnAbort(int) {
  FILE* file = OpenReport(L"abort.txt");
  if (!file) return;
  std::fprintf(file, "SIGABRT on thread %lu\n", GetCurrentThreadId());
  void* frames[64]{};
  const auto count = CaptureStackBackTrace(0, 64, frames, nullptr);
  for (unsigned index = 0; index < count; ++index)
    WriteFrame(file, index, reinterpret_cast<DWORD64>(frames[index]));
  std::fflush(file);
  std::fclose(file);
}

}  // namespace

void InstallCrashReporter() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  SetUnhandledExceptionFilter(OnUnhandledException);
  // CRT abort uses fail-fast on Windows, bypassing the exception filter.
  std::signal(SIGABRT, OnAbort);
}

void DumpAllThreadStacks(const char* reason) {
  struct Sample {
    DWORD tid;
    std::vector<DWORD64> pcs;
  };
  std::vector<Sample> samples;

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return;
  }
  const DWORD own_pid = GetCurrentProcessId();
  const DWORD own_tid = GetCurrentThreadId();

  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID != own_pid || entry.th32ThreadID == own_tid) {
        continue;
      }
      HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                 FALSE, entry.th32ThreadID);
      if (!thread) {
        continue;
      }
      // Capture return addresses only while suspended. Symbolizing here would
      // deadlock the moment a suspended thread happened to hold a CRT or
      // dbghelp lock, so that waits until every thread is running again.
      Sample sample{entry.th32ThreadID, {}};
      if (SuspendThread(thread) != DWORD(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
          STACKFRAME64 frame{};
          frame.AddrPC.Offset = context.Rip;
          frame.AddrPC.Mode = AddrModeFlat;
          frame.AddrFrame.Offset = context.Rbp;
          frame.AddrFrame.Mode = AddrModeFlat;
          frame.AddrStack.Offset = context.Rsp;
          frame.AddrStack.Mode = AddrModeFlat;
          for (int i = 0; i < 48; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame,
                             &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                             nullptr) ||
                frame.AddrPC.Offset == 0) {
              break;
            }
            sample.pcs.push_back(frame.AddrPC.Offset);
          }
        }
        ResumeThread(thread);
      }
      CloseHandle(thread);
      samples.push_back(std::move(sample));
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);

  wchar_t name[64];
  _snwprintf_s(name, 64, _TRUNCATE, L"stacks_%hs.txt", reason);
  FILE* f = OpenReport(name);
  if (!f) {
    return;
  }
  std::fprintf(f, "thread stacks at %s (%zu threads)\n\n", reason, samples.size());
  for (const auto& sample : samples) {
    std::fprintf(f, "thread %lu:\n", sample.tid);
    if (sample.pcs.empty()) {
      std::fprintf(f, "  <no frames captured>\n");
    }
    for (size_t i = 0; i < sample.pcs.size(); ++i) {
      WriteFrame(f, static_cast<int>(i), sample.pcs[i]);
    }
    std::fprintf(f, "\n");
  }
  std::fflush(f);
  std::fclose(f);
}

void StartStackSampler() {
  const char* spec = std::getenv("BO3_SAMPLE_SECS");
  if (!spec || !*spec) {
    return;
  }
  std::vector<int> offsets;
  for (const char* p = spec; *p;) {
    char* end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == p) {
      break;
    }
    if (v > 0) {
      offsets.push_back(static_cast<int>(v));
    }
    p = (*end == ',') ? end + 1 : end;
  }
  if (offsets.empty()) {
    return;
  }
  std::thread([offsets] {
    int elapsed = 0;
    for (int at : offsets) {
      if (at <= elapsed) {
        continue;
      }
      std::this_thread::sleep_for(std::chrono::seconds(at - elapsed));
      elapsed = at;
      char reason[32];
      std::snprintf(reason, sizeof(reason), "%ds", at);
      DumpAllThreadStacks(reason);
    }
  }).detach();
}

}  // namespace bo3


#include "stdafx.h"

#include <cstdlib>

#include <ApiLock.h>
#include <PipeChannel.h>

using namespace weasel;
using namespace std;
using namespace boost;

#define _ThrowLastError throw ::GetLastError()
#define _ThrowCode(__c) throw __c
#define _ThrowIfNot(__c)                 \
  {                                      \
    DWORD err;                           \
    if ((err = ::GetLastError()) != __c) \
      throw err;                         \
  }

namespace {

// Attribution for "the input method stopped responding": every IPC wait runs on
// the host application's UI thread, so knowing which stage consumed the time
// (connect / write / read) is the difference between a guess and a diagnosis.
// Set RIME_WEASEL_IPC_TRACE=1 to report slow stages to the watchdog log.
bool IpcTraceEnabled() {
  static const bool enabled = []() {
#pragma warning(suppress : 4996)
    return std::getenv("RIME_WEASEL_IPC_TRACE") != nullptr;
  }();
  return enabled;
}

struct StageTimer {
  const char* stage;
  unsigned long long start_ms;
  explicit StageTimer(const char* name) : stage(name), start_ms(NowMs()) {}
  ~StageTimer() {
    if (!IpcTraceEnabled())
      return;
    const unsigned long long elapsed = NowMs() - start_ms;
    if (elapsed < 50)
      return;
    char line[256];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "ipc: %s took %llu ms", stage,
                elapsed);
    WatchdogLog(line);
  }
};

}  // namespace

PipeChannelBase::PipeChannelBase(std::wstring&& pn_cmd,
                                 size_t bs = 4 * 1024,
                                 SECURITY_ATTRIBUTES* s = NULL)
    : pname(pn_cmd), buff_size(bs), sa(s) {};

DWORD weasel::IpcReadTimeoutMs() {
  static const DWORD timeout = []() -> DWORD {
#pragma warning(suppress : 4996)
    if (const char* env = std::getenv("RIME_WEASEL_IPC_TIMEOUT_MS")) {
      const int value = std::atoi(env);
      if (value >= 100 && value <= 60000)
        return static_cast<DWORD>(value);
    }
    return 3000;
  }();
  return timeout;
}

bool PipeChannelBase::_WaitReadable(HANDLE pipe, DWORD timeout_ms) const {
  StageTimer timer("read-wait");
  // ReadFile() on a message-mode pipe blocks until a whole message arrives.
  // Poll first so the wait can be bounded and the UI thread stays responsive.
  //
  // The budget must be measured in wall-clock time: PeekNamedPipe() is cheap but
  // not free (~10ms per call against a stalled peer), and counting only the
  // sleeps in between made the "bounded" wait overshoot by an order of
  // magnitude (measured: 7.8s for a 1s budget, ~23s for the default 3s).  That
  // overshoot is precisely the unresponsive typing this deadline exists to
  // prevent.
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL))
      return false;  // server gone / connection closed
    if (available > 0)
      return true;
    if (::GetTickCount64() >= deadline)
      return false;
    ::Sleep(2);
  }
}

PipeChannelBase::~PipeChannelBase() {
  // Thread-specific pointers are cleaned up automatically
}

bool PipeChannelBase::_Ensure() {
  try {
    HANDLE* phandle = _GetPipeHandle();
    if (_Invalid(*phandle)) {
      *phandle = _Connect(pname.c_str());
      return !_Invalid(*phandle);
    }
  } catch (...) {
    return false;
  }

  return true;
}

HANDLE PipeChannelBase::_Connect(const wchar_t* name) {
  StageTimer timer("connect");
  HANDLE pipe = INVALID_HANDLE_VALUE;
  // Connecting can block forever when every server pipe instance is busy --
  // which is exactly what a stalled server looks like.  This call happens on
  // the host application's UI thread (_EnsureServerConnected -> _Reconnect), so
  // an unbounded wait here froze every application that touched the IME,
  // explorer.exe included.  Give up after the same deadline used for reads.
  const DWORD deadline = ::GetTickCount() + IpcReadTimeoutMs();
  while (_Invalid(pipe = _TryConnect())) {
    if (::GetTickCount() >= deadline)
      return INVALID_HANDLE_VALUE;
    ::WaitNamedPipe(name, 100);
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL)) {
    _ThrowLastError;
  }
  return pipe;
}

void PipeChannelBase::_Reconnect() {
  HANDLE* phandle = _GetPipeHandle();
  _FinalizePipe(*phandle);
  _Ensure();
}

HANDLE PipeChannelBase::_TryConnect() {
  auto pipe = ::CreateFile(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (!_Invalid(pipe)) {
    // connected to the pipe
    return pipe;
  }
  // being busy is not really an error since we just need to wait.
  _ThrowIfNot(ERROR_PIPE_BUSY);
  // All pipe instances are busy
  return INVALID_HANDLE_VALUE;
}

size_t PipeChannelBase::_WritePipe(HANDLE pipe, size_t s, char* b) {
  StageTimer timer("write");
  DWORD lwritten;
  if (!::WriteFile(pipe, b, s, &lwritten, NULL) || lwritten <= 0) {
    _ThrowLastError;
  }
  // Deliberately no FlushFileBuffers() here.  On a named pipe it blocks until
  // the peer has read the data, so a stalled peer turns this into an unbounded
  // wait; on the server side that wait used to happen while holding the global
  // API mutex, which froze every application talking to the IME (explorer.exe
  // included) until the machine was powered off.  Message-mode pipes deliver
  // each WriteFile as one complete message, so the request/response protocol
  // does not need an explicit flush.
  return lwritten;
}

void PipeChannelBase::_FinalizePipe(HANDLE& p) {
  if (!_Invalid(p)) {
    StageTimer timer("disconnect");
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
  p = INVALID_HANDLE_VALUE;
}

void PipeChannelBase::_Receive(HANDLE pipe, LPVOID msg, size_t rec_len) {
  StageTimer timer("read");
  DWORD lread;
  BOOL success = ::ReadFile(pipe, msg, rec_len, &lread, NULL);
  if (!success) {
    _ThrowIfNot(ERROR_MORE_DATA);

    auto ctx = _GetContext();
    memset(ctx->buffer.get(), 0, buff_size);
    success = ::ReadFile(pipe, ctx->buffer.get(), buff_size, &lread, NULL);
    if (!success) {
      _ThrowLastError;
    }
  }
  _GetContext()->has_body = false;
}

HANDLE PipeChannelBase::_ConnectServerPipe(std::wstring& pn) {
  HANDLE pipe =
      CreateNamedPipe(pn.c_str(), PIPE_ACCESS_DUPLEX,
                      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                      PIPE_UNLIMITED_INSTANCES, buff_size, buff_size, 0, sa);
  if (pipe == INVALID_HANDLE_VALUE) {
    _ThrowLastError;
  }
  if (!::ConnectNamedPipe(pipe, NULL)) {
    const DWORD err = ::GetLastError();
    // A client may connect between CreateNamedPipe() and ConnectNamedPipe();
    // that is success, not an error.  Discarding the instance here threw away a
    // live connection and left the client waiting on a dead handle.
    if (err != ERROR_PIPE_CONNECTED) {
      ::CloseHandle(pipe);
      _ThrowCode(err);
    }
  }
  return pipe;
}

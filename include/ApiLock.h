#pragma once
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

// Every librime call in WeaselServer runs under one process-wide lock, because
// librime is not thread safe.  That lock used to be a plain std::mutex taken for
// the whole duration of a pipe request -- which covered candidate-window
// painting, cross-thread window moves and the tray refresh as well -- while the
// server message thread took the same lock for deferred language-model work.
//
// Two threads could then wait for each other forever: a pipe worker holding the
// lock while a *cross-thread* window operation needed the message thread to
// pump its queue, and the message thread blocked in lock().  Closing the pipe on
// a client timeout cannot break that -- it wakes I/O waits, not a mutex -- so
// the server stayed blocked and every later keystroke timed out even though the
// desktop itself looked healthy.
//
// This lock keeps the serialization, but makes every wait bounded and records
// who holds it, so a watchdog can report a long hold instead of silently
// freezing every input session on the machine.

namespace weasel {

inline unsigned long long NowMs() {
  return static_cast<unsigned long long>(::GetTickCount64());
}

class ApiLock {
 public:
  struct Snapshot {
    bool locked = false;
    unsigned long long held_ms = 0;
    const char* op = "";
    bool warning_reported = false;
  };

  static ApiLock& instance() {
    // Process lifetime: librime and the ORT sessions outlive static teardown.
    static ApiLock* lock = new ApiLock();
    return *lock;
  }

  // Bounded acquisition.  Re-entrant for the thread that already owns it, so a
  // nested helper (e.g. Initialize() called while EndMaintenance() holds the
  // lock) can never deadlock against itself.  Returns false on timeout.
  bool Acquire(DWORD timeout_ms, const char* op) {
    const DWORD tid = ::GetCurrentThreadId();
    if (owner_tid_.load(std::memory_order_relaxed) == tid) {
      ++depth_;
      return true;
    }
    if (timeout_ms == 0) {
      if (!mutex_.try_lock())
        return false;
    } else if (!mutex_.try_lock_for(std::chrono::milliseconds(timeout_ms))) {
      return false;
    }
    depth_ = 1;
    owner_tid_.store(tid, std::memory_order_relaxed);
    holder_op_.store(op ? op : "", std::memory_order_relaxed);
    held_since_.store(NowMs(), std::memory_order_relaxed);
    warning_reported_.store(false, std::memory_order_relaxed);
    return true;
  }

  void Release() {
    if (depth_ > 1) {
      --depth_;
      return;
    }
    depth_ = 0;
    owner_tid_.store(0, std::memory_order_relaxed);
    holder_op_.store("", std::memory_order_relaxed);
    held_since_.store(0, std::memory_order_relaxed);
    mutex_.unlock();
  }

  // Read-only diagnostics, safe from any thread (also from the watchdog).
  Snapshot Inspect() const {
    Snapshot snapshot;
    const unsigned long long since =
        held_since_.load(std::memory_order_relaxed);
    snapshot.op = holder_op_.load(std::memory_order_relaxed);
    snapshot.locked = since != 0;
    snapshot.held_ms = snapshot.locked ? (NowMs() - since) : 0;
    snapshot.warning_reported =
        warning_reported_.load(std::memory_order_relaxed);
    return snapshot;
  }

  // One WARNING per hold: the watchdog calls this after reporting.
  void MarkWarningReported() {
    warning_reported_.store(true, std::memory_order_relaxed);
  }

 private:
  ApiLock() = default;
  ApiLock(const ApiLock&) = delete;
  ApiLock& operator=(const ApiLock&) = delete;

  std::timed_mutex mutex_;
  std::atomic<DWORD> owner_tid_{0};
  std::atomic<unsigned long long> held_since_{0};
  std::atomic<const char*> holder_op_{""};
  std::atomic<bool> warning_reported_{false};
  int depth_ = 0;  // only the owning thread touches this
};

// RAII helper.  Always check acquired() before touching librime or the
// candidate window -- a timed-out guard still destroys safely.
class ApiLockGuard {
 public:
  ApiLockGuard(const char* op, DWORD timeout_ms)
      : acquired_(ApiLock::instance().Acquire(timeout_ms, op)) {}
  ~ApiLockGuard() {
    if (acquired_)
      ApiLock::instance().Release();
  }
  ApiLockGuard(const ApiLockGuard&) = delete;
  ApiLockGuard& operator=(const ApiLockGuard&) = delete;
  bool acquired() const { return acquired_; }
  explicit operator bool() const { return acquired_; }

 private:
  bool acquired_;
};

// Weasel's own LOG() macros compile away unless WEASEL_ENABLE_LOGGING is
// defined (see include/logging.h), so the few diagnostics that explain a
// wedged server are written to a dedicated file next to the rime logs -- the
// place a user already knows to look -- and to the debugger output.
inline void WatchdogLog(const std::string& message) {
  SYSTEMTIME now;
  ::GetLocalTime(&now);
  char line[1024];
  _snprintf_s(line, sizeof(line), _TRUNCATE, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n",
              now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
              now.wSecond, now.wMilliseconds, message.c_str());

  wchar_t temp[MAX_PATH] = {0};
  const DWORD len = ::GetTempPathW(MAX_PATH, temp);
  if (len > 0 && len < MAX_PATH) {
    std::wstring dir(temp);
    if (!dir.empty() && dir.back() != L'\\')
      dir += L'\\';
    dir += L"rime.weasel";
    ::CreateDirectoryW(dir.c_str(), NULL);
    const std::wstring path = dir + L"\\weasel-watchdog.log";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"a") == 0 && fp) {
      std::fputs(line, fp);
      std::fclose(fp);
    }
  }
  ::OutputDebugStringA(line);
}

}  // namespace weasel

// Standalone check of the ApiLock diagnostics used by the server watchdog:
// acquisition, held-time reporting and the log sink.  Build/run with
// tools/apilock_selftest.cmd; prints the log path it wrote to.
#include <ApiLock.h>

#include <cstdio>

int main() {
  auto& lock = weasel::ApiLock::instance();

  weasel::WatchdogLog("apilock_selftest: direct log write");

  if (!lock.Acquire(0, "selftest_hold")) {
    std::printf("RESULT apilock_selftest=fail (acquire)\n");
    return 1;
  }
  const auto first = lock.Inspect();
  ::Sleep(300);
  const auto second = lock.Inspect();
  lock.Release();
  const auto after = lock.Inspect();

  std::printf("locked=%d op=%s held_first=%llu held_after_sleep=%llu\n",
              first.locked ? 1 : 0, first.op, first.held_ms, second.held_ms);
  std::printf("released: locked=%d\n", after.locked ? 1 : 0);

  // Re-entrancy: the same thread must be able to take it again (nested calls,
  // e.g. Initialize() during EndMaintenance()).
  bool nested = lock.Acquire(0, "outer") && lock.Acquire(0, "inner");
  lock.Release();
  const bool still_locked = lock.Inspect().locked;
  lock.Release();
  const bool really_released = !lock.Inspect().locked;

  std::printf("nested=%d still_locked_after_inner_release=%d released=%d\n",
              nested ? 1 : 0, still_locked ? 1 : 0, really_released ? 1 : 0);

  // A second thread must not be able to take it while held (bounded failure).
  lock.Acquire(0, "holder");
  bool other_thread_acquired = true;
  std::thread([&]() {
    other_thread_acquired = weasel::ApiLock::instance().Acquire(100, "other");
  }).join();
  lock.Release();

  std::printf("other_thread_acquired_while_held=%d\n",
              other_thread_acquired ? 1 : 0);

  wchar_t temp[MAX_PATH] = {0};
  ::GetTempPathW(MAX_PATH, temp);
  std::wprintf(L"log=%lsrime.weasel\\weasel-watchdog.log\n", temp);

  const bool pass = first.locked && second.held_ms >= 250 && !after.locked &&
                    nested && still_locked && really_released &&
                    !other_thread_acquired;
  std::printf("RESULT apilock_selftest=%s\n", pass ? "pass" : "fail");
  return pass ? 0 : 1;
}

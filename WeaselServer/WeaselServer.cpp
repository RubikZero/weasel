// WeaselServer.cpp : main source file for WeaselServer.exe
//
//	WTL MessageLoop 封装了消息循环. 实现了 getmessage/dispatchmessage....

#include "stdafx.h"
#include "resource.h"
#include "WeaselService.h"
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <RimeWithWeasel.h>
#include <WeaselUtility.h>
#include <winsparkle.h>
#include <functional>
#include <ShellScalingApi.h>
#include <WinUser.h>
#include <memory>
#include <atlstr.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#pragma comment(lib, "Shcore.lib")
CAppModule _Module;

namespace {

// Every installation -- and every isolated test instance -- creates its IPC
// window with the same class and title, and (unless RIME_WEASEL_PIPE_NAME says
// otherwise) listens on the same per-user pipe name.  The executable path is
// therefore the only thing that separates "the server of this build" from "a
// WeaselServer installed somewhere else", and a quit must never reach the
// latter: doing so silently killed the user's running service whenever a build
// was started from a different directory.
std::wstring OwnImagePath() {
  WCHAR path[MAX_PATH * 4] = {0};
  DWORD len = ::GetModuleFileNameW(NULL, path, _countof(path));
  return std::wstring(path, len);
}

std::wstring ImagePathOfProcess(DWORD pid) {
  std::wstring result;
  if (!pid)
    return result;
  HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc)
    return result;
  WCHAR path[MAX_PATH * 4] = {0};
  DWORD len = _countof(path);
  if (::QueryFullProcessImageNameW(proc, 0, path, &len))
    result.assign(path, len);
  ::CloseHandle(proc);
  return result;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// The command line handed to _tWinMain still carries the white space that
// separates the options from the program name -- and some launch styles pass
// the whole command line -- so upstream's !wcscmp(L"/q", lpstrCmdLine) never
// matched.  /q therefore fell through to the "restart if already running" path,
// which shut down whichever server owned the per-user pipe (the user's
// installed one) and then became a server itself, so a build hung waiting for
// it.  Compare option tokens instead of the raw string.
bool HasCommandLineOption(LPCWSTR command_line, LPCWSTR option) {
  if (!command_line || !*command_line)
    return false;
  const std::wstring line(command_line);
  size_t pos = 0;
  while (pos < line.size()) {
    pos = line.find_first_not_of(L" \t\r\n", pos);
    if (pos == std::wstring::npos)
      break;
    size_t end = line.find_first_of(L" \t\r\n", pos);
    std::wstring token =
        line.substr(pos, end == std::wstring::npos ? std::wstring::npos
                                                   : end - pos);
    // accept "/q" as well as /q
    if (token.size() >= 2 && token.front() == L'"' && token.back() == L'"')
      token = token.substr(1, token.size() - 2);
    if (_wcsicmp(token.c_str(), option) == 0)
      return true;
    if (end == std::wstring::npos)
      break;
    pos = end;
  }
  return false;
}

// Classifies the running WeaselServer processes: *own is set for one started
// from our own executable, *foreign for any other installation or build.  The
// caller itself is skipped -- it is not a server, and counting it as one would
// hide the difference between "our server is running" and "only a foreign one
// is", which is exactly the distinction the pipe guard needs.
void FindServerProcesses(const std::wstring& own_path,
                         bool* own,
                         bool* foreign) {
  *own = *foreign = false;
  const DWORD self = ::GetCurrentProcessId();
  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return;
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  if (::Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == self)
        continue;
      if (_wcsicmp(entry.szExeFile, L"WeaselServer.exe") != 0)
        continue;
      const std::wstring image = ImagePathOfProcess(entry.th32ProcessID);
      if (image.empty())
        continue;
      if (SamePath(image, own_path))
        *own = true;
      else
        *foreign = true;
    } while (::Process32NextW(snapshot, &entry));
  }
  ::CloseHandle(snapshot);
}

// Waits for the servers started from our own executable to be gone, so that
// "WeaselServer.exe /quit" behaves like the install script's ExecWait expects:
// when it returns, the files it is about to replace are no longer in use.
void WaitForOwnServersToExit(const std::wstring& own_path, DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    bool own = false;
    bool foreign = false;
    FindServerProcesses(own_path, &own, &foreign);
    if (!own)
      return;
    if (::GetTickCount64() >= deadline)
      return;
    ::Sleep(50);
  }
}

// Asks the IPC windows backed by our own executable to quit.  The message
// thread handles WM_CLOSE without the pipe and without the api lock, so this
// still works when a stuck request would make every pipe transaction time out
// -- the case where the user previously had to kill the process by hand.
bool QuitOwnServerWindows(const std::wstring& own_path) {
  // Collect first, post afterwards: a window disappears as soon as it handles
  // WM_CLOSE, and enumerating with a stale handle would skip the rest.
  std::vector<HWND> targets;
  for (HWND wnd = ::FindWindowW(WEASEL_IPC_WINDOW, WEASEL_IPC_WINDOW); wnd;
       wnd = ::FindWindowExW(NULL, wnd, WEASEL_IPC_WINDOW,
                             WEASEL_IPC_WINDOW)) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(wnd, &pid);
    if (SamePath(ImagePathOfProcess(pid), own_path))
      targets.push_back(wnd);
  }
  bool asked = false;
  for (HWND wnd : targets) {
    if (::PostMessageW(wnd, WM_CLOSE, 0, 0))
      asked = true;
  }
  return asked;
}

}  // namespace

int WINAPI _tWinMain(HINSTANCE hInstance,
                     HINSTANCE /*hPrevInstance*/,
                     LPTSTR lpstrCmdLine,
                     int nCmdShow) {
  LANGID langId = get_language_id();
  SetThreadUILanguage(langId);
  SetThreadLocale(langId);

  if (!IsWindowsBlueOrLaterEx()) {
    CString info, cap;
    info.LoadStringW(IDS_STR_SYSTEM_VERSION_WARNING);
    cap.LoadStringW(IDS_STR_SYSTEM_VERSION_WARNING_CAPTION);
    MessageBoxExW(NULL, info, cap, MB_ICONERROR, langId);
    return 0;
  }
  SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

  // 防止服务进程开启输入法
  ImmDisableIME(-1);

  WCHAR user_name[20] = {0};
  DWORD size = _countof(user_name);
  GetUserName(user_name, &size);
  if (!_wcsicmp(user_name, L"SYSTEM")) {
    return 1;
  }

  HRESULT hRes = ::CoInitialize(NULL);
  // If you are running on NT 4.0 or higher you can use the following call
  // instead to make the EXE free threaded. This means that calls come in on a
  // random RPC thread.
  // HRESULT hRes = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
  ATLASSERT(SUCCEEDED(hRes));

  // this resolves ATL window thunking problem when Microsoft Layer for Unicode
  // (MSLU) is used
  ::DefWindowProc(NULL, 0, 0, 0L);

  AtlInitCommonControls(
      ICC_BAR_CLASSES);  // add flags to support other controls

  hRes = _Module.Init(NULL, hInstance);
  ATLASSERT(SUCCEEDED(hRes));

  if (HasCommandLineOption(lpstrCmdLine, L"/userdir")) {
    CreateDirectory(WeaselUserDataPath().c_str(), NULL);
    WeaselServerApp::explore(WeaselUserDataPath());
    return 0;
  }
  if (HasCommandLineOption(lpstrCmdLine, L"/weaseldir")) {
    WeaselServerApp::explore(WeaselServerApp::install_dir());
    return 0;
  }
  if (HasCommandLineOption(lpstrCmdLine, L"/ascii") ||
      HasCommandLineOption(lpstrCmdLine, L"/nascii")) {
    weasel::Client client;
    bool ascii = HasCommandLineOption(lpstrCmdLine, L"/ascii");
    if (client.Connect())  // try to connect to running server
    {
      if (ascii)
        client.TrayCommand(ID_WEASELTRAY_ENABLE_ASCII);
      else
        client.TrayCommand(ID_WEASELTRAY_DISABLE_ASCII);
    }
    return 0;
  }

  // command line option /q stops the running server
  bool quit = HasCommandLineOption(lpstrCmdLine, L"/q") ||
              HasCommandLineOption(lpstrCmdLine, L"/quit");

  const std::wstring own_path = OwnImagePath();
  bool own_server = false;
  bool foreign_server = false;
  FindServerProcesses(own_path, &own_server, &foreign_server);

  if (quit) {
    // Ask our own server to quit through its window: the message thread handles
    // WM_CLOSE without the pipe and without the api lock, so this also works
    // when a stuck request would make every pipe transaction time out -- the
    // case where the user previously had to kill the process by hand.
    bool stopped = QuitOwnServerWindows(own_path);
    if (!stopped && own_server && !foreign_server) {
      // Our server has no window (it did not get that far).  The per-user pipe
      // is shared with every other installation, so this request is only safe
      // because no server from another directory is running at all.
      weasel::Client client;
      if (client.Connect()) {
        client.ShutdownServer();
        stopped = true;
      }
    }
    if (stopped) {
      // The install script replaces files right after this returns.
      WaitForOwnServersToExit(own_path, 5000);
    }
    // Nothing of ours was running (or it did not answer): either way there is
    // nothing to stop, and a server from another directory is never touched.
    return 0;
  }
  // restart if already running
  {
    weasel::Client client;
    if (client.Connect())  // try to connect to running server
    {
      // Taking over the pipe means stopping whoever holds it; refuse when that
      // is a server from another directory, so starting this build cannot cut
      // the user off from their installed input method.
      if (foreign_server && !own_server)
        return 1;
      client.ShutdownServer();
      int retry = 0;
      while (client.Connect() && retry < 10) {
        client.ShutdownServer();
        retry++;
        Sleep(50);
      }
      if (retry >= 10)
        return 0;
    }
  }

  bool check_updates = HasCommandLineOption(lpstrCmdLine, L"/update");
  if (check_updates) {
    WeaselServerApp::check_update();
  }

  CreateDirectory(WeaselUserDataPath().c_str(), NULL);

  int nRet = 0;
  try {
    WeaselServerApp app;
    RegisterApplicationRestart(NULL, 0);
    nRet = app.Run();
  } catch (...) {
    // bad luck...
    nRet = -1;
  }

  _Module.Term();
  ::CoUninitialize();

  return nRet;
}

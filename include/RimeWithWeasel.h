#pragma once
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <atomic>
#include <map>
#include <string>
#include <mutex>

#include <rime_api.h>

struct CaseInsensitiveCompare {
  bool operator()(const std::string& str1, const std::string& str2) const {
    std::string str1Lower, str2Lower;
    std::transform(str1.begin(), str1.end(), std::back_inserter(str1Lower),
                   [](char c) { return std::tolower(c); });
    std::transform(str2.begin(), str2.end(), std::back_inserter(str2Lower),
                   [](char c) { return std::tolower(c); });
    return str1Lower < str2Lower;
  }
};

typedef std::map<std::string, bool> AppOptions;
typedef std::map<std::string, AppOptions, CaseInsensitiveCompare>
    AppOptionsByAppName;

struct SessionStatus {
  SessionStatus()
      : style(weasel::UIStyle()),
        __synced(false),
        session_id(0),
        lm_refresh_enabled(false) {
    RIME_STRUCT(RimeStatus, status);
  }
  weasel::UIStyle style;
  RimeStatus status;
  bool __synced;
  RimeSessionId session_id;
  bool lm_refresh_enabled;
};
typedef std::map<DWORD, SessionStatus> SessionStatusMap;
typedef DWORD WeaselSessionId;
class RimeWithWeaselHandler : public weasel::RequestHandler {
 public:
  RimeWithWeaselHandler(weasel::UI* ui);
  virtual ~RimeWithWeaselHandler();
  virtual void Initialize();
  virtual void Finalize();
  virtual DWORD FindSession(WeaselSessionId ipc_id);
  virtual DWORD AddSession(LPWSTR buffer, EatLine eat = 0);
  virtual DWORD RemoveSession(WeaselSessionId ipc_id);
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               WeaselSessionId ipc_id,
                               EatLine eat);
  virtual void CommitComposition(WeaselSessionId ipc_id);
  virtual void ClearComposition(WeaselSessionId ipc_id);
  virtual void SelectCandidateOnCurrentPage(size_t index,
                                            WeaselSessionId ipc_id);
  virtual bool HighlightCandidateOnCurrentPage(size_t index,
                                               WeaselSessionId ipc_id,
                                               EatLine eat);
  virtual bool ChangePage(bool backward, WeaselSessionId ipc_id, EatLine eat);
  virtual void FocusIn(DWORD param, WeaselSessionId ipc_id);
  virtual void FocusOut(DWORD param, WeaselSessionId ipc_id);
  virtual void UpdateInputPosition(RECT const& rc, WeaselSessionId ipc_id);
  virtual void SetSurroundingText(LPWSTR buffer, WeaselSessionId ipc_id);
  virtual void StartMaintenance();
  virtual void EndMaintenance();
  virtual void SetOption(WeaselSessionId ipc_id,
                         const std::string& opt,
                         bool val);
  virtual void UpdateColorTheme(BOOL darkMode);
  virtual void OnMaintenanceWatchdog();

  void OnUpdateUI(std::function<void()> const& cb);
  // Register a poster that runs a callback on the server message thread.
  void OnPostToServerThread(std::function<void(std::function<void()>)> poster) {
    m_post_to_server_thread = std::move(poster);
  }
  // Register a poster for *window* work.  The candidate window belongs to the
  // server message thread, so every Show/Hide/Update/MoveTo has to happen
  // there; a pipe worker doing it while holding the api lock could be left
  // waiting for the message thread, which is itself waiting for that lock.
  void OnPostUIToServerThread(
      std::function<void(std::function<void()>)> poster) {
    m_post_ui_to_server_thread = std::move(poster);
  }

 private:
  void _Setup();
  bool _IsDeployerRunning();
  bool _HasServerMessageLoop() const;
  void _PostUI(std::function<void()> fn);
  // Called with the api lock held (pipe worker or deferred task): only takes a
  // snapshot of the request and defers the window work.
  void _UpdateUI(WeaselSessionId ipc_id, bool add_session = false);
  // Runs on the server message thread, takes the api lock with a deadline.
  void _ApplyUILocked(WeaselSessionId ipc_id, bool add_session);
  // Requires the api lock to be held.
  void _ApplyUI(WeaselSessionId ipc_id, bool add_session);
  void _HideUI();
  void _ApplyInputPosition(RECT const& rc, WeaselSessionId ipc_id);
  // Re-initializes librime outside a client request: a maintenance resume can
  // take minutes (join_maintenance_thread) and must never run inside a key or
  // session request while holding the api lock.
  void _ResumeMaintenanceAsync();
  void _LoadSchemaSpecificSettings(WeaselSessionId ipc_id,
                                   const std::string& schema_id);
  void _LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                bool ignore_app_name = false);
  bool _ShowMessage(weasel::Context& ctx,
                    weasel::Status& status,
                    bool add_session);
  bool _Respond(WeaselSessionId ipc_id, EatLine eat);
  void _ReadClientInfo(WeaselSessionId ipc_id, LPWSTR buffer);
  void _GetCandidateInfo(weasel::CandidateInfo& cinfo, RimeContext& ctx);
  void _GetStatus(weasel::Status& stat,
                  WeaselSessionId ipc_id,
                  weasel::Context& ctx);
  void _GetContext(weasel::Context& ctx, RimeSessionId session_id);
  void _UpdateShowNotifications(RimeConfig* config, bool initialize = false);
  void _LoadLmRefreshSettings();

  void _UpdateInlinePreeditStatus(WeaselSessionId ipc_id);

  RimeSessionId to_session_id(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id].session_id;
  }
  SessionStatus& get_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id];
  }
  SessionStatus& new_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id] = SessionStatus();
  }

  AppOptionsByAppName m_app_options;
  weasel::UI* m_ui;  // reference
  DWORD m_active_session;
  // Read from the watchdog and from window code without the api lock.
  std::atomic<bool> m_disabled;
  std::string m_last_schema_id;
  std::string m_last_app_name;
  weasel::UIStyle m_base_style;
  std::map<std::string, bool> m_show_notifications;
  std::map<std::string, bool> m_show_notifications_base;
  std::function<void()> _UpdateUICallback;
  std::function<void(std::function<void()>)> m_post_to_server_thread;
  std::function<void(std::function<void()>)> m_post_ui_to_server_thread;
  // At most one outstanding language-model refresh: the deferred task re-reads
  // the live composition, so extra notifications only pile up on the api lock.
  std::atomic<bool> m_lm_refresh_posted{false};
  std::atomic<bool> m_resume_in_flight{false};
  // Message thread only (maintenance watchdog).
  unsigned long long m_disabled_since = 0;
  // Throttles background resume attempts (a key press while the service is
  // disabled must not spawn one thread each).
  std::atomic<unsigned long long> m_last_resume_kick{0};

  static void OnNotify(void* context_object,
                       uintptr_t session_id,
                       const char* message_type,
                       const char* message_value);
  static std::string m_message_type;
  static std::string m_message_value;
  static std::string m_message_label;
  static std::string m_option_name;
  static std::mutex m_notifier_mutex;
  SessionStatusMap m_session_status_map;
  bool m_current_dark_mode;
  bool m_global_ascii_mode;
  int m_show_notifications_time;
  bool m_lm_refresh_enabled;
  int m_lm_refresh_initial_ms;
  int m_lm_refresh_interval_ms;
  int m_lm_refresh_timeout_ms;
  DWORD m_pid;
};

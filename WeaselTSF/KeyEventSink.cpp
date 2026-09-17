#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

namespace {

// A null-window timer is delivered on the TSF host's UI thread.  Keep the
// mapping only to translate its generated id back to the text service.
std::mutex g_lm_refresh_timers_mutex;
std::map<UINT_PTR, WeaselTSF*> g_lm_refresh_timers;

}  // namespace

void WeaselTSF::_ScheduleLmRefresh() {
  if (!_lm_refresh_enabled)
    return;
  const UINT initial_ms = _lm_refresh_initial_ms;
  const UINT interval_ms = _lm_refresh_interval_ms;
  const UINT timeout_ms = _lm_refresh_timeout_ms;
  _lm_refresh_attempts =
      1 + static_cast<unsigned int>((timeout_ms - initial_ms) / interval_ms);
  if (_lm_refresh_timer)
    return;

  _lm_refresh_timer_window = _GetFocusedContextWindow();
  const UINT_PTR requested_id = reinterpret_cast<UINT_PTR>(this);
  UINT_PTR timer_id = ::SetTimer(_lm_refresh_timer_window, requested_id,
                                 initial_ms, &_LmRefreshTimerProc);
  // Some TSF hosts do not dispatch a thread-only (null-window) timer.  Binding
  // to the focused editor window makes the callback follow the application's
  // normal UI message loop.
  if (!timer_id && _lm_refresh_timer_window) {
    _lm_refresh_timer_window = nullptr;
    timer_id =
        ::SetTimer(nullptr, requested_id, initial_ms, &_LmRefreshTimerProc);
  }
  if (!timer_id)
    return;
  {
    std::lock_guard<std::mutex> lock(g_lm_refresh_timers_mutex);
    g_lm_refresh_timers[timer_id] = this;
  }
  _lm_refresh_timer = timer_id;
  _lm_refresh_first_tick = true;
}

void WeaselTSF::_CancelLmRefresh() {
  if (!_lm_refresh_timer)
    return;
  ::KillTimer(_lm_refresh_timer_window, _lm_refresh_timer);
  {
    std::lock_guard<std::mutex> lock(g_lm_refresh_timers_mutex);
    g_lm_refresh_timers.erase(_lm_refresh_timer);
  }
  _lm_refresh_timer = 0;
  _lm_refresh_timer_window = nullptr;
  _lm_refresh_attempts = 0;
  _lm_refresh_first_tick = false;
}

void WeaselTSF::_PollLmRefresh() {
  if (!_status.composing || !_pEditSessionContext ||
      _lm_refresh_attempts == 0) {
    _CancelLmRefresh();
    return;
  }

  if (_lm_refresh_first_tick) {
    _lm_refresh_first_tick = false;
    const UINT_PTR timer_id =
        ::SetTimer(_lm_refresh_timer_window, _lm_refresh_timer,
                   _lm_refresh_interval_ms, &_LmRefreshTimerProc);
    if (!timer_id) {
      _CancelLmRefresh();
      return;
    }
    if (timer_id != _lm_refresh_timer) {
      std::lock_guard<std::mutex> lock(g_lm_refresh_timers_mutex);
      g_lm_refresh_timers.erase(_lm_refresh_timer);
      g_lm_refresh_timers[timer_id] = this;
      _lm_refresh_timer = timer_id;
    }
  }

  --_lm_refresh_attempts;
  // A zero keycode is already used by the TSF focus path to request a current
  // Rime response.  It has no editing effect, unlike synthesizing a real key.
  m_client.ProcessKeyEvent(0);
  _UpdateComposition(_pEditSessionContext);
}

void CALLBACK WeaselTSF::_LmRefreshTimerProc(HWND,
                                              UINT,
                                              UINT_PTR timer_id,
                                              DWORD) {
  WeaselTSF* text_service = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lm_refresh_timers_mutex);
    const auto it = g_lm_refresh_timers.find(timer_id);
    if (it == g_lm_refresh_timers.end())
      return;
    text_service = it->second;
    text_service->AddRef();
  }
  text_service->_PollLmRefresh();
  text_service->Release();
}

void WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return;
  }
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    *pfEaten = FALSE;
  } else {
    _SendSurroundingText();
    // cheet key code when vertical auto reverse happened, swap up and down
    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    if (!keyCountToSimulate) {
      _CancelLmRefresh();
      *pfEaten = (BOOL)m_client.ProcessKeyEvent(ke);
    }

    if (ke.keycode == ibus::Caps_Lock) {
      if (prevKeyEvent.keycode == ibus::Caps_Lock && prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (keyCountToSimulate)
        keyCountToSimulate--;
    }

    prevfEaten = *pfEaten;
    prevKeyEvent = ke;
  }
}

STDMETHODIMP WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground) {
    _surrounding_text_dirty = true;
    m_client.FocusIn();
  } else {
    m_client.FocusOut();
    _CancelLmRefresh();
    _AbortComposition();
    _surrounding_text.clear();
    _surrounding_text_dirty = true;
    if (!_surrounding_text_last_sent.empty()) {
      m_client.SetSurroundingText(std::wstring());
      _surrounding_text_last_sent.clear();
    }
  }

  return S_OK;
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *
 * We assume every key event will eventually cause a OnKeyDown() call.
 * We use _fTestKeyDownPending to omit multiple OnTestKeyDown() calls,
 *  and for OnKeyDown() to check if the key has already been sent to the server.
 */

STDMETHODIMP WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyDownPending = TRUE;
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyDown(ITfContext* pContext,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    _fTestKeyDownPending = FALSE;
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyUpPending = TRUE;
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyUp(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    _fTestKeyUpPending = FALSE;
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    if (!_async_edit)
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                       REFGUID rguid,
                                       BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);
	
	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}

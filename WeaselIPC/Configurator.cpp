#include "stdafx.h"
#include "Deserializer.h"
#include "Configurator.h"

using namespace weasel;

Deserializer::Ptr Configurator::Create(ResponseParser* pTarget) {
  return Deserializer::Ptr(new Configurator(pTarget));
}

Configurator::Configurator(ResponseParser* pTarget) : Deserializer(pTarget) {}

Configurator::~Configurator() {}

void Configurator::Store(Deserializer::KeyType const& key,
                         std::wstring const& value) {
  if (!m_pTarget->p_config || key.size() < 2)
    return;
  bool bool_value = (!value.empty() && value != L"0");
  if (key[1] == L"inline_preedit") {
    m_pTarget->p_config->inline_preedit = bool_value;
  } else if (key[1] == L"lm_refresh_enabled") {
    m_pTarget->p_config->lm_refresh_enabled = bool_value;
  } else if (key[1] == L"lm_refresh_initial_ms") {
    m_pTarget->p_config->lm_refresh_initial_ms = _wtoi(value.c_str());
  } else if (key[1] == L"lm_refresh_interval_ms") {
    m_pTarget->p_config->lm_refresh_interval_ms = _wtoi(value.c_str());
  } else if (key[1] == L"lm_refresh_timeout_ms") {
    m_pTarget->p_config->lm_refresh_timeout_ms = _wtoi(value.c_str());
  } else if (key[1] == L"lm_pending") {
    m_pTarget->p_config->lm_pending = bool_value;
  }
}

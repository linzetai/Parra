// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/net/proxy_config_monitor.h"

#include <optional>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "base/win/registry.h"
#endif  // BUILDFLAG(IS_WIN)
#include "build/chromeos_buildflags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/net/proxy_service_factory.h"
#include "chrome/browser/policy/profile_policy_connector.h"
#include "chrome/browser/profiles/profile.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/proxy_config/proxy_prefs.h"
#include "components/enterprise/buildflags/buildflags.h"
#include "components/proxy_config/pref_proxy_config_tracker_impl.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/buildflags/buildflags.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/network_context.mojom.h"

#if BUILDFLAG(ENTERPRISE_PROXY)
#include "chrome/browser/enterprise/net/enterprise_proxy_service_factory.h"
#endif  // BUILDFLAG(ENTERPRISE_PROXY)

#if BUILDFLAG(IS_CHROMEOS)
#include "chrome/browser/ash/profiles/profile_helper.h"
#endif  // BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
#include "extensions/browser/api/proxy/proxy_api.h"
#endif // BUILDFLAG(ENABLE_EXTENSIONS_CORE)

using content::BrowserThread;

#if BUILDFLAG(IS_WIN)
namespace {

bool IsWindowsProxyPolicyManaged() {
  constexpr wchar_t kInternetSettingsPolicy[] =
      L"Software\\Policies\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
  constexpr wchar_t kInternetControlPanelPolicy[] =
      L"Software\\Policies\\Microsoft\\Internet Explorer\\Control Panel";
  for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    base::win::RegKey internet_settings(hive, kInternetSettingsPolicy,
                                        KEY_READ);
    if (internet_settings.Valid()) {
      DWORD value = 0;
      if (internet_settings.ReadValueDW(L"ProxySettingsPerUser", &value) ==
              ERROR_SUCCESS ||
          internet_settings.ReadValueDW(L"ProxyEnable", &value) ==
              ERROR_SUCCESS) {
        return true;
      }
      std::wstring proxy_server;
      std::wstring pac_url;
      if ((internet_settings.ReadValue(L"ProxyServer", &proxy_server) ==
               ERROR_SUCCESS &&
           !proxy_server.empty()) ||
          (internet_settings.ReadValue(L"AutoConfigURL", &pac_url) ==
               ERROR_SUCCESS &&
           !pac_url.empty())) {
        return true;
      }
    }

    base::win::RegKey control_panel(hive, kInternetControlPanelPolicy,
                                    KEY_READ);
    DWORD proxy_locked = 0;
    if (control_panel.Valid() &&
        control_panel.ReadValueDW(L"Proxy", &proxy_locked) == ERROR_SUCCESS &&
        proxy_locked != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace
#endif  // BUILDFLAG(IS_WIN)

ProxyConfigMonitor::ProxyConfigMonitor(Profile* profile) {
  CHECK_CURRENTLY_ON(content::BrowserThread::UI, base::NotFatalUntil::M161);
  CHECK(profile, base::NotFatalUntil::M161);

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  profile_ = profile;
#endif  // BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  profile_for_host_override_ = profile;
  host_override_pref_change_registrar_.Init(profile->GetPrefs());
  host_override_pref_change_registrar_.Add(
      proxy_config::prefs::kProxy,
      base::BindRepeating(
          &ProxyConfigMonitor::OnHostOverridePolicyPrefChanged,
          base::Unretained(this)));
  host_override_pref_change_registrar_.Add(
      proxy_config::prefs::kProxyOverrideRules,
      base::BindRepeating(
          &ProxyConfigMonitor::OnHostOverridePolicyPrefChanged,
          base::Unretained(this)));

// If this is the ChromeOS sign-in or lock screen profile, just create the
// tracker from global state.
#if BUILDFLAG(IS_CHROMEOS)
  if (ash::ProfileHelper::IsSigninProfile(profile) ||
      ash::ProfileHelper::IsLockScreenProfile(profile)) {
    pref_proxy_config_tracker_ =
        ProxyServiceFactory::CreatePrefProxyConfigTrackerOfLocalState(
            g_browser_process->local_state());
  }
#endif  // BUILDFLAG(IS_CHROMEOS)

  if (!pref_proxy_config_tracker_) {
    enterprise_net::EnterpriseProxyService* enterprise_proxy_service = nullptr;
#if BUILDFLAG(ENTERPRISE_PROXY)
    enterprise_proxy_service =
        EnterpriseProxyServiceFactory::GetForProfile(profile);
#endif  // BUILDFLAG(ENTERPRISE_PROXY)
    pref_proxy_config_tracker_ =
        ProxyServiceFactory::CreatePrefProxyConfigTrackerOfProfile(
            profile->GetPrefs(), g_browser_process->local_state(),
            profile->GetProfilePolicyConnector()->policy_service(),
            enterprise_proxy_service);
  }

  proxy_config_service_ = ProxyServiceFactory::CreateProxyConfigService(
      pref_proxy_config_tracker_.get(), profile);

  proxy_config_service_->AddObserver(this);
}

ProxyConfigMonitor::ProxyConfigMonitor(PrefService* local_state) {
  CHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
            !BrowserThread::IsThreadInitialized(BrowserThread::UI),
        base::NotFatalUntil::M161);

  pref_proxy_config_tracker_ =
      ProxyServiceFactory::CreatePrefProxyConfigTrackerOfLocalState(
          local_state);

  proxy_config_service_ = ProxyServiceFactory::CreateProxyConfigService(
      pref_proxy_config_tracker_.get(), nullptr);
  proxy_config_service_->AddObserver(this);
}

ProxyConfigMonitor::~ProxyConfigMonitor() {
  CHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
            !BrowserThread::IsThreadInitialized(BrowserThread::UI),
        base::NotFatalUntil::M161);
  proxy_config_service_->RemoveObserver(this);
  pref_proxy_config_tracker_->DetachFromPrefService();
}

void ProxyConfigMonitor::AddToNetworkContextParams(
  network::mojom::NetworkContextParams* network_context_params,
  bool enable_host_override,
  const std::string& host_mapping_rules,
  uint64_t host_mapping_revision) {
  mojo::PendingRemote<network::mojom::ProxyConfigClient> proxy_config_client;
  network_context_params->proxy_config_client_receiver =
      proxy_config_client.InitWithNewPipeAndPassReceiver();
  proxy_config_client_set_.Add(std::move(proxy_config_client));

  if (proxy_config_service_->UsesPolling()) {
    poller_receiver_set_.Add(this,
                             network_context_params->proxy_config_poller_client
                                 .InitWithNewPipeAndPassReceiver());
  }

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  error_receiver_set_.Add(this, network_context_params->proxy_error_client
                                    .InitWithNewPipeAndPassReceiver());
#endif  // BUILDFLAG(ENABLE_EXTENSIONS_CORE)

  net::ProxyConfigWithAnnotation proxy_config;
  net::ProxyConfigService::ConfigAvailability availability =
      proxy_config_service_->GetLatestProxyConfig(&proxy_config);
  if (availability != net::ProxyConfigService::CONFIG_PENDING)
    network_context_params->initial_proxy_config = proxy_config;

  if (enable_host_override) {
    custom_proxy_configuration_active_ =
      custom_proxy_configuration_active_ ||
      !!network_context_params->initial_custom_proxy_config ||
      network_context_params->custom_proxy_config_client_receiver.is_valid();
    network_context_params->parra_host_override_enabled = true;
    network_context_params->parra_host_override_rules = host_mapping_rules;
    network_context_params->host_mapping_revision = host_mapping_revision;
    const auto [policy, reason] =
        GetHostOverridePolicy(proxy_config, availability,
                    custom_proxy_configuration_active_);
    network_context_params->host_override_policy = policy;
    network_context_params->host_override_policy_reason = reason;

    mojo::PendingRemote<network::mojom::HostOverridePolicyClient>
        host_override_policy_client;
    network_context_params->host_override_policy_client_receiver =
        host_override_policy_client.InitWithNewPipeAndPassReceiver();
    host_override_policy_client_set_.Add(std::move(host_override_policy_client));
  }
}

std::optional<bool> ProxyConfigMonitor::IsProxyConfigured() const {
  CHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
            !BrowserThread::IsThreadInitialized(BrowserThread::UI),
        base::NotFatalUntil::M162);
  net::ProxyConfigWithAnnotation proxy_config;
  net::ProxyConfigService::ConfigAvailability availability =
      proxy_config_service_->GetLatestProxyConfig(&proxy_config);
  switch (availability) {
    case net::ProxyConfigService::CONFIG_VALID: {
      const net::ProxyConfig& config = proxy_config.value();
      // Intentionally check `config.has_pac_url()` rather than
      // `config.HasAutomaticSettings()`. On Windows, WPAD auto-detection
      // (`config.auto_detect()`) is enabled by default in OS settings even when
      // no proxy exists on the network, which would otherwise cause almost all
      // Windows connections to report a proxy as configured.
      return !config.proxy_rules().empty() || config.has_pac_url() ||
             !config.proxy_override_rules().empty() ||
             !config.dynamic_routing_config().routing_rules.empty();
    }
    case net::ProxyConfigService::CONFIG_UNSET:
      return false;
    case net::ProxyConfigService::CONFIG_PENDING:
      return std::nullopt;
  }
}

void ProxyConfigMonitor::FlushForTesting() {
  proxy_config_client_set_.FlushForTesting();
}

void ProxyConfigMonitor::OnProxyConfigChanged(
    const net::ProxyConfigWithAnnotation& config,
    net::ProxyConfigService::ConfigAvailability availability) {
  CHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
            !BrowserThread::IsThreadInitialized(BrowserThread::UI),
        base::NotFatalUntil::M161);
  for (const auto& proxy_config_client : proxy_config_client_set_) {
    switch (availability) {
      case net::ProxyConfigService::CONFIG_VALID:
        proxy_config_client->OnProxyConfigUpdated(config);
        break;
      case net::ProxyConfigService::CONFIG_UNSET:
        proxy_config_client->OnProxyConfigUpdated(
            net::ProxyConfigWithAnnotation::CreateDirect());
        break;
      case net::ProxyConfigService::CONFIG_PENDING:
        NOTREACHED();
    }
  }
  NotifyHostOverridePolicyClients(config, availability);
}

void ProxyConfigMonitor::OnHostOverridePolicyPrefChanged() {
  if (!proxy_config_service_) {
    return;
  }
  net::ProxyConfigWithAnnotation config;
  const auto availability =
      proxy_config_service_->GetLatestProxyConfig(&config);
  NotifyHostOverridePolicyClients(config, availability);
}

void ProxyConfigMonitor::NotifyHostOverridePolicyClients(
    const net::ProxyConfigWithAnnotation& config,
    net::ProxyConfigService::ConfigAvailability availability) {
  const auto [policy, reason] = GetHostOverridePolicy(
      config, availability, custom_proxy_configuration_active_);
  for (const auto& host_override_client : host_override_policy_client_set_) {
    host_override_client->UpdateHostOverridePolicy(policy, reason);
  }
}

std::pair<network::mojom::HostOverridePolicy, std::string>
ProxyConfigMonitor::GetHostOverridePolicy(
    const net::ProxyConfigWithAnnotation& config,
    net::ProxyConfigService::ConfigAvailability availability,
    bool custom_proxy_config) const {
  using Policy = network::mojom::HostOverridePolicy;
  if (availability == net::ProxyConfigService::CONFIG_PENDING ||
      !profile_for_host_override_) {
    return {Policy::kUnknown, "proxy policy source is not yet known"};
  }

  if (custom_proxy_config) {
    return {Policy::kBlockedByPolicy,
            "custom proxy configuration source is not classified"};
  }

#if BUILDFLAG(IS_WIN)
  if (IsWindowsProxyPolicyManaged()) {
    return {Policy::kBlockedByPolicy,
            "Windows Internet Settings proxy is managed by policy"};
  }
#endif  // BUILDFLAG(IS_WIN)

  const net::ProxyConfig& effective_config = config.value();
  if (!effective_config.dynamic_routing_config().routing_rules.empty()) {
    return {Policy::kBlockedByPolicy,
            "enterprise dynamic proxy routing is active"};
  }

  net::ProxyConfigWithAnnotation preference_config;
    policy::PolicyService* policy_service =
      profile_for_host_override_->GetProfilePolicyConnector()->policy_service();
  const ProxyPrefs::ConfigState source =
      PrefProxyConfigTrackerImpl::ReadPrefConfig(
          profile_for_host_override_->GetPrefs(), &preference_config,
        policy_service);

  switch (source) {
    case ProxyPrefs::CONFIG_POLICY:
    case ProxyPrefs::CONFIG_POLICY_OVERRIDE:
    case ProxyPrefs::CONFIG_POLICY_DYNAMIC_ROUTING:
      return {Policy::kBlockedByPolicy,
              "managed Chrome proxy policy forbids DIRECT override"};
    case ProxyPrefs::CONFIG_OTHER_PRECEDE:
    case ProxyPrefs::CONFIG_EXTENSION:
    case ProxyPrefs::CONFIG_EXTENSION_OVERRIDE:
    case ProxyPrefs::CONFIG_SYSTEM:
    case ProxyPrefs::CONFIG_FALLBACK:
    case ProxyPrefs::CONFIG_UNSET:
      break;
  }

  if (effective_config.pac_mandatory()) {
    return {Policy::kMandatoryProxy,
            "mandatory PAC configuration forbids DIRECT override"};
  }

  if (!effective_config.proxy_override_rules().empty() &&
      source != ProxyPrefs::CONFIG_EXTENSION_OVERRIDE) {
    return {Policy::kBlockedByPolicy,
            "proxy override rules are active but their permission is unknown"};
  }

  return {Policy::kAllowed, "proxy source permits Environment DIRECT override"};
}

void ProxyConfigMonitor::OnLazyProxyConfigPoll() {
  proxy_config_service_->OnLazyPoll();
}

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
void ProxyConfigMonitor::OnPACScriptError(int32_t line_number,
                                          const std::string& details) {
  CHECK_CURRENTLY_ON(content::BrowserThread::UI, base::NotFatalUntil::M161);
  extensions::ProxyEventRouter::GetInstance()->OnPACScriptError(
      profile_, line_number, base::UTF8ToUTF16(details));
}

void ProxyConfigMonitor::OnRequestMaybeFailedDueToProxySettings(
    int32_t net_error) {
  CHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
            !BrowserThread::IsThreadInitialized(BrowserThread::UI),
        base::NotFatalUntil::M161);

  if (net_error >= 0) {
    // If the error is obviously wrong, don't dispatch it to extensions. If the
    // PAC executor process is compromised, then |net_error| could be attacker
    // controlled.
    return;
  }

  extensions::ProxyEventRouter::GetInstance()->OnProxyError(profile_,
                                                            net_error);
}
#endif  // BUILDFLAG(ENABLE_EXTENSIONS_CORE)

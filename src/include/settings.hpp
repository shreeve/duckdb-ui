#pragma once

#include <duckdb/common/exception.hpp>
#include <duckdb/main/client_context.hpp>

#define UI_LOCAL_PORT_SETTING_NAME "ui_local_port"
#define UI_LOCAL_PORT_SETTING_DEFAULT 4213
#define UI_REMOTE_URL_SETTING_NAME "ui_remote_url"
#define UI_REMOTE_URL_SETTING_DEFAULT "https://ui.duckdb.org"
// The address a browser reaches this server at, when that is not
// http://localhost:<ui_local_port>. Empty means it is.
//
// Every data endpoint refuses a request whose Origin is not the address the
// UI is served from -- a cross-site guard, and the reason a UI reached
// through a reverse proxy loads its page and then fails every query. Naming
// the real address here keeps that guard working rather than removing it: a
// hostile page's Origin still will not match. Rewriting Origin in the proxy
// instead makes the comparison unfailable, which is not the same thing.
#define UI_PUBLIC_URL_SETTING_NAME "ui_public_url"
#define UI_PUBLIC_URL_SETTING_DEFAULT ""
#define UI_POLLING_INTERVAL_SETTING_NAME "ui_polling_interval"
#define UI_POLLING_INTERVAL_SETTING_DEFAULT 284

namespace duckdb {

namespace internal {

template <typename T>
T GetSetting(const ClientContext &context, const char *setting_name) {
  Value value;
  if (!context.TryGetCurrentSetting(setting_name, value)) {
    throw Exception(ExceptionType::SETTINGS,
                    "Setting \"" + std::string(setting_name) + "\" not found");
  }
  return value.GetValue<T>();
}
} // namespace internal

std::string GetRemoteUrl(const ClientContext &);
std::string GetPublicUrl(const ClientContext &);
uint16_t GetLocalPort(const ClientContext &);
uint32_t GetPollingInterval(const ClientContext &);

} // namespace duckdb

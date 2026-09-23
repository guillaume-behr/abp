#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "abp/AdbClient.h"
#include "abp/Device.h"
#include "abp/Manifest.h"

namespace abp::personal {

/// One row of `content query` output, keyed by column name.
using Row = std::map<std::string, std::string>;

/// Parses the text `adb shell content query --projection a:b:c` prints:
///
///     Row: 0 a=1, b=x, c=free text, possibly with ", " and newlines
///
/// The format is not escaped, so a value containing ", b=" is ambiguous.
/// The parser leans on knowing the projection: each column is looked for in
/// order, and the *last* column runs to the end of the row -- so put the one
/// free-text column (an SMS body) last and it survives commas and newlines.
/// A value printed as NULL reads as an empty string. Rows that do not match
/// the expected columns are skipped.
std::vector<Row> parseContentQuery(const std::string& output, const std::vector<std::string>& columns);

/// Percent-encodes everything but RFC 3986 unreserved characters, as
/// android.net.Uri.encode() does.
std::string encodeUriComponent(const std::string& text);

/// Builds vCard 3.0 text from rows of the contacts provider's `data` table
/// (columns contact_id, mimetype, data1, data2, data3). This is the fallback
/// for devices whose `content` tool cannot stream the provider's own vCard
/// export; it keeps names, phone numbers, emails, postal addresses,
/// organisations and notes. `*count` receives the number of contacts.
std::string buildVcards(const std::vector<Row>& dataRows, int* count);

/// Builds an iCalendar (RFC 5545) document from rows of the calendar
/// provider's `events` table (columns _id, calendar_id, dtstart, dtend,
/// allDay, rrule, duration, eventTimezone, eventLocation, title,
/// description). `calendarNames` maps calendar_id to a display name, written
/// as each event's CATEGORIES. `*count` receives the number of events.
std::string buildIcs(const std::vector<Row>& events, const std::map<std::string, std::string>& calendarNames,
                     int* count);

/// Parses `settings list <namespace>` output ("key=value" per line).
std::map<std::string, std::string> parseSettingsList(const std::string& output);

/// One saved Wi-Fi network.
struct WifiNetwork {
    std::string ssid;
    std::string security; ///< "open", "wpa2", "wpa3", "owe", or the raw key management for others.
    std::string password; ///< Empty for open networks, or when the store did not hold it in clear.
    bool hidden = false;
};

/// Parses Android 8+'s WifiConfigStore.xml.
std::vector<WifiNetwork> parseWifiConfigStore(const std::string& xml);

/// Parses Android 7-and-older's wpa_supplicant.conf.
std::vector<WifiNetwork> parseWpaSupplicant(const std::string& conf);

/// The `cmd wifi add-network ...` command that re-creates `network`, or an
/// empty string when it cannot be expressed that way (enterprise/WEP, or a
/// secured network whose password is unknown).
std::string wifiAddNetworkCommand(const WifiNetwork& network);

/// Outcome of exporting one kind of personal data.
struct ExportResult {
    std::string kind;   ///< Manifest kind: contacts, sms, mms, call_log, calendar, settings, wifi.
    std::string label;  ///< Human-readable, e.g. "SMS messages".
    int count = -1;     ///< Items exported; -1 when this kind was not exported.
    std::string reason; ///< Why it was not exported, when count is -1.
};

/// Exports contacts (vCard), SMS, MMS and call log (JSON), calendar events
/// (iCalendar) and device settings (JSON) into `outDir/personal` through
/// `adb shell content` and `settings`, and -- given root -- saved Wi-Fi
/// networks with their passwords. Each export is recorded in the manifest.
/// Every kind is best-effort: one the device refuses is skipped with the
/// reason rather than failing the backup.
std::vector<ExportResult> exportPersonalData(const AdbClient& adb, const std::filesystem::path& outDir,
                                             const DeviceInfo& device, Manifest& manifest);

/// What restorePersonalData() did.
struct RestoreResult {
    std::string contactsPath;  ///< Device path contacts.vcf was copied to, or empty.
    std::string calendarPath;  ///< Device path calendar.ics was copied to, or empty.
    int wifiRestored = -1;     ///< Networks re-added; -1 when not attempted.
    int wifiSkipped = 0;       ///< Networks that could not be re-added (enterprise, unknown password, ...).
};

/// Writes back what can be written back without root: copies contacts.vcf
/// and calendar.ics to the device's Download folder for import, and re-adds
/// saved Wi-Fi networks with `cmd wifi add-network` (Android 11+). SMS, MMS,
/// call log and settings are archival and are only reported.
RestoreResult restorePersonalData(const AdbClient& adb, const std::filesystem::path& backupDir,
                                  const Manifest& manifest, int sdkInt);

} // namespace abp::personal

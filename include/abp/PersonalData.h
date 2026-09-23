#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "abp/AdbClient.h"
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

/// What exportPersonalData() managed to read; -1 means that kind could not
/// be exported (typically the device refused access to its provider).
struct ExportCounts {
    int contacts = -1;
    int sms = -1;
    int callLog = -1;
};

/// Exports contacts (vCard), SMS and call log (JSON) into `outDir/personal`
/// through `adb shell content`, and records each export in the manifest.
/// Needs no root. Each kind is best-effort: a provider the shell user may
/// not read is skipped with a warning rather than failing the backup.
ExportCounts exportPersonalData(const AdbClient& adb, const std::filesystem::path& outDir, Manifest& manifest);

/// Copies the backup's contacts.vcf to the device's Download folder so it can
/// be imported from the Contacts app. Returns the device path, or an empty
/// string if the backup has no contacts export or the copy failed.
std::string pushContactsForImport(const AdbClient& adb, const std::filesystem::path& backupDir,
                                  const Manifest& manifest);

} // namespace abp::personal

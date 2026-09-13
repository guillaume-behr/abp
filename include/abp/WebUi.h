#pragma once

namespace abp::webui {

/// The GUI's single-page app (HTML + CSS + JS), compiled into the binary so
/// `abp gui` works from a single installed executable with no asset
/// directory to find, and no network access to fetch anything from.
const char* indexHtml();

} // namespace abp::webui

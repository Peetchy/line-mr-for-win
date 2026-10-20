// Bridge log: %LOCALAPPDATA%\LineMR\logs\bridge.log (rotated at start-up).

#pragma once

#include <QtCore/QString>

namespace line_a11y {

// Directory holding logs, dumps and request files (%LOCALAPPDATA%\LineMR), with a trailing
// separator.
const QString &bridgeDirectory();

void a11yLog(const char *fmt, ...);
void a11yLogString(const QString &line);

} // namespace line_a11y

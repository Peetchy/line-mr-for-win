// Bridge log: %LOCALAPPDATA%\LineMR\logs\bridge.log (rotated at start-up).

#pragma once

#include <QtCore/QString>

namespace line_a11y {

// Directory holding logs, dumps and request files (%LOCALAPPDATA%\LineMR), with a trailing
// separator.
const QString &bridgeDirectory();

void a11yLog(const char *fmt, ...);
void a11yLogString(const QString &line);

// Crash breadcrumbs: the bridge's last actions (renders, keys, menus, windows) in a ring kept in
// a memory-mapped file (%LOCALAPPDATA%\LineMR\breadcrumbs.bin), which Windows writes back even
// when LINE crashes. openBreadcrumbs() logs the previous session's ring when that session crashed.
void openBreadcrumbs(bool logPrevious);
void breadcrumb(const char *fmt, ...);

} // namespace line_a11y

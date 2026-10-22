// Bridge log: %LOCALAPPDATA%\LineMR\logs\bridge.log, or linecall.log / linemediaplayer.log in
// LINE's helper processes (rotated at start-up).

#pragma once

#include <QtCore/QString>

namespace line_a11y {

// Directory holding logs, dumps and request files (%LOCALAPPDATA%\LineMR), with a trailing
// separator.
const QString &bridgeDirectory();

// Is the bridge running in one of LINE's helper processes rather than in LINE.exe: LineCall.exe
// (incoming-call notification, call windows) or LineMediaPlayer.exe (photo and video viewer)?
bool inHelperProcess();
// The helper process's name in lower case ("linecall", "linemediaplayer"), empty in LINE.exe.
const QString &helperProcessName();

void a11yLog(const char *fmt, ...);
void a11yLogString(const QString &line);

// Crash breadcrumbs: the bridge's last actions (renders, keys, menus, windows) in a ring kept in
// a memory-mapped file (%LOCALAPPDATA%\LineMR\breadcrumbs.bin), which Windows writes back even
// when LINE crashes. openBreadcrumbs() logs the previous session's ring when that session crashed.
void openBreadcrumbs(bool logPrevious);
void breadcrumb(const char *fmt, ...);

} // namespace line_a11y

// Spoken announcements: raises a UI Automation notification event on a LINE window, which NVDA
// speaks (Qt 6.6 has no announcement API; QAccessible::Alert only plays a sound on Windows).

#pragma once

#include <QtCore/QString>

class QWidget;

namespace line_a11y {

// Announces `text` if `w`'s window is the active window (or always, with `evenIfInactive`).
// Identical texts within 2 s are dropped.
void announce(QWidget *w, const QString &text, bool evenIfInactive = false);

// Tells the NVDA add-on about an incoming call, also while LINE is in the background: writes `text`
// to %LOCALAPPDATA%\LineMR\incoming-call.txt and signals the event "Local\lineMR.incomingCall"
// (NVDA does not speak UI Automation notifications from background windows).
void announceIncomingCall(const QString &text);

} // namespace line_a11y

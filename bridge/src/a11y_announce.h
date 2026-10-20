// Spoken announcements: raises a UI Automation notification event on a LINE window, which NVDA
// speaks (Qt 6.6 has no announcement API; QAccessible::Alert only plays a sound on Windows).

#pragma once

#include <QtCore/QString>

class QWidget;

namespace line_a11y {

// Announces `text` if `w`'s window is the active window. Identical texts within 2 s are dropped.
void announce(QWidget *w, const QString &text);

} // namespace line_a11y

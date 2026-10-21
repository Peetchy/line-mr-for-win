// Application-wide event filter that keeps LINE's accessibility pipeline alive, repairs
// interfaces Qt cached before the bridge factory was installed, and makes LINE's custom
// controls reachable with the keyboard.

#pragma once

class QCoreApplication;
class QWidget;

namespace line_a11y {

// Removes a foreign QAccessible update handler (LINE installs a no-op one that swallows every
// accessibility event, so NVDA never hears about focus changes) and marks accessibility active.
// Logs whenever a foreign handler had to be removed.
void enforceAccessibilityHandler(const char *reason);

void installEventFilter(QCoreApplication *app);

// The user moved focus or pressed a control through a screen reader (UIA SetFocus / Invoke):
// stop sending focus back to a list LINE pulled it away from.
void endListBrowsing();

// A control was pressed (keyboard or screen reader): a panel it opens gets focus (see the .cpp).
void notePress(QWidget *pressed);

// Is the user browsing `list` (focus there, or briefly pulled into the open chat by LINE)?
bool isBrowsingList(const QWidget *list);

} // namespace line_a11y

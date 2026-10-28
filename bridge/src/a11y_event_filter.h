// Application-wide event filter that keeps LINE's accessibility pipeline alive, repairs
// interfaces Qt cached before the bridge factory was installed, and makes LINE's custom
// controls reachable with the keyboard.

#pragma once

#include <functional>

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

// A context menu item was pressed by the bridge (`how`: the signal emitted, or "click"): a window
// it opens (confirmation dialogs) gets activated instead of focus going back behind the menu.
void noteMenuPress(QWidget *item, const char *how);

// The bridge right-clicks a message (LineAccessibleList::openRowMenu): if the next context menu
// LINE shows within 1.5 s is the chat background's (Select all / Set wallpaper), it is closed and
// `retry` runs (a click on another part of the message); otherwise it is kept and focused as usual.
// A null `retry` keeps any menu.
void setMenuRetry(std::function<void()> retry);

// How many context menus LINE has shown so far.
int contextMenusShown();

// Is the user browsing `list` (focus there, or briefly pulled into the open chat by LINE)?
bool isBrowsingList(const QWidget *list);

// A photo / video was opened from `window` (Notes): when LINE comes back from its media viewer, that
// window is activated again (LINE activates its main window).
void noteMediaOpenedFrom(QWidget *window);

} // namespace line_a11y

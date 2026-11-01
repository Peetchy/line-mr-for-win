// Focus highlight for low-vision users: a coloured frame around the control that has the focus.
//
// It follows the focus the bridge reports to UI Automation (QAccessible::Focus events, after the
// bridge dropped the ones LINE makes while the user browses a list), so it also marks the bridge's
// own list rows, menu items and Qt Quick items. The frame is a native, click-through, layered
// window owned by the focused window (class "lineMR_focus_highlight"): drawing it never makes LINE
// repaint. Settings: %LOCALAPPDATA%\LineMR\focus-highlight.ini, written by the add-on.

#pragma once

class QAccessibleEvent;
class QCoreApplication;

namespace line_a11y {

void startFocusHighlight(QCoreApplication *app);

// Called by the bridge's QAccessible update handler for every event it passes on.
void noteAccessibleEvent(QAccessibleEvent *event);

} // namespace line_a11y

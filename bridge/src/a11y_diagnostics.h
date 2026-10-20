// On-demand diagnostics: creating %LOCALAPPDATA%\LineMR\dump-request makes the running bridge
// write widget-dump.txt (widget tree, properties, captured text, item-view models and the
// accessible tree) next to it, and resource-list.txt (LINE's call-related resource names).

#pragma once

class QApplication;
class QString;
class QWidget;

namespace line_a11y {

void startDiagnostics(QApplication *app);
void writeWidgetDump();
// Writes the widget and accessible tree of one window to %LOCALAPPDATA%\LineMR\<fileName>.
void writeWindowDump(QWidget *window, const QString &fileName);

} // namespace line_a11y

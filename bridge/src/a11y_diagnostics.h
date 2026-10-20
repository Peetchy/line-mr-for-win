// On-demand diagnostics: creating %LOCALAPPDATA%\LineMR\dump-request makes the running bridge
// write widget-dump.txt (widget tree, properties, captured text, item-view models and the
// accessible tree) next to it.

#pragma once

class QApplication;

namespace line_a11y {

void startDiagnostics(QApplication *app);
void writeWidgetDump();

} // namespace line_a11y

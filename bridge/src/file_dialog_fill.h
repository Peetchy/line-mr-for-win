// Answers the next Windows file dialog LINE opens (Save As, or Select Folder in Photos & videos) with
// a path of the bridge's: transcription has LINE download a voice message or video into the add-on's
// folder (%LOCALAPPDATA%\LineMR\media) through LINE's own Save, without the user seeing more than the
// dialog flash by.
//
// LINE uses the native common item dialog (#32770 in LINE's own process). A worker thread waits for
// a new one, writes the path into its file name box and presses its default button (IDOK); window
// messages work across threads, and the dialog's modal loop runs on LINE's GUI thread.
//
// Windows remembers the folder and file of every Save As (ComDlg32 LastVisitedPidlMRU /
// OpenSavePidlMRU): after the bridge's save, the user's next own "Save as..." opened in the deleted
// temporary folder. Those keys are copied before the dialog and put back once it has closed.

#pragma once

#include <functional>

class QString;

namespace line_a11y {

// Fills the next file dialog shown within `timeoutMs` with `path` (a file for Save As, a folder for
// Select Folder) and confirms it. The result is logged ("[file-dialog] ...") and passed to `done`
// on the GUI thread (true: filled and confirmed).
void fillNextFileDialog(const QString &path, std::function<void(bool)> done = {}, int timeoutMs = 15000);

} // namespace line_a11y

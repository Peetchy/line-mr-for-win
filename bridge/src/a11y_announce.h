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

// LineCall.exe: the add-on's End call command (Alt+D) signals the event "Local\lineMR.endCall";
// the call shown is then ended like its End call button does (the voice call window's, or the video
// call window's, whose controls are hidden while the mouse is elsewhere).
void startEndCallListener(QObject *context);

// Voice message and video transcription (the add-on transcribes LINE's copy of the message, else
// records LINE's sound while LINE plays a voice message / has LINE save a video into a temporary
// folder): writes %LOCALAPPDATA%\LineMR\transcribe-request.txt (lines "status <ok|notvoice|busy>",
// "seconds <length or -1>", "pid <LINE's process>", "kind <voice|video>", tab-separated) and signals
// "Local\lineMR.transcribeRequest".
void requestTranscription(const QString &status, int seconds, const QString &kind = QStringLiteral("voice"));
// True once the add-on has signalled "Local\lineMR.transcribePlay" (it is recording now, or waits for
// the video's file); the signal is consumed.
bool transcriptionPlayRequested();
// The file a voice message or video is to be saved to for transcription (the add-on's
// transcribe-target.txt), or "".
QString transcriptionTarget();
// How asking LINE to save that file went: writes transcribe-fetch.txt ("saving": LINE's Save As was
// answered, LINE downloads and writes the file; "failed": no Save As item or dialog).
void reportFileFetch(bool saving);

// The add-on's Save message command (Ctrl+S): writes %LOCALAPPDATA%\LineMR\save-request.txt
// (lines "status <text|media|nosave|unsupported|busy|unavailable>", "kind <message kind, or the
// message command (Reply, React ...) LINE does not offer for the message>", "name <suggested
// file name>", tab-separated), the text to save (status text) to save-text.txt (UTF-8), and signals
// "Local\lineMR.saveRequest". The add-on asks where to save the text, or says why nothing is saved.
void requestSave(const QString &status, const QString &kind, const QString &name = QString(), const QString &text = QString());

} // namespace line_a11y

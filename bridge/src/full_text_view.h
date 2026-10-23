// A readable text view for LINE's full-text window.
//
// "See more" under a long message opens ChatDetailTextMessageWindow, which shows the whole text as a
// single list row: a screen reader reads it in one go, with no caret to move by line or word. The
// bridge lays a transparent, read-only QPlainTextEdit holding the same lines (as LINE wraps them)
// over the list and gives it focus: a caret, lines, words and characters, as in Notepad. Its own
// accessibility interface serves them from a cached copy of the text and reports only the caret's
// line as its value. The view never repaints and LINE's list does not scroll along (both made LINE
// redraw the long row for seconds per key); mouse events pass through to LINE.

#pragma once

class QAccessibleInterface;
class QWidget;

namespace line_a11y {

// Attaches the text view to `window` (a ChatDetailTextMessageWindow) and focuses it. False if the
// text could not be read (LINE was painting, or no list); the caller may fall back.
bool attachFullTextView(QWidget *window);

// The accessibility interface of the text view (`w` is it), or nullptr for any other widget.
QAccessibleInterface *fullTextAccessible(QWidget *w);

} // namespace line_a11y

// A readable text view for LINE's full-text window.
//
// "See more" under a long message opens ChatDetailTextMessageWindow, which shows the whole text as a
// single list row: a screen reader reads it in one go, with no caret to move by line or word. The
// bridge lays a transparent, read-only QPlainTextEdit holding the same lines (as LINE wraps them)
// over the list and gives it focus. Qt's own text accessibility then offers a caret, lines, words
// and characters, as in Notepad. Mouse events pass through to LINE, and moving the caret scrolls
// LINE's list along so the text on screen follows.

#pragma once

class QWidget;

namespace line_a11y {

// Attaches the text view to `window` (a ChatDetailTextMessageWindow) and focuses it. False if the
// text could not be read (LINE was painting, or no list); the caller may fall back.
bool attachFullTextView(QWidget *window);

} // namespace line_a11y

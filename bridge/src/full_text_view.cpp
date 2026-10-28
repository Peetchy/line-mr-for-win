#include "full_text_view.h"

#include "a11y_log.h"
#include "line_introspect.h"
#include "text_capture.h"

#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QTextBoundaryFinder>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QFontMetricsF>
#include <QtGui/QKeyEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocument>
#include <QtWidgets/QAccessibleWidget>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>

#include <algorithm>

namespace line_a11y {
namespace {

const char kViewName[] = "lineMR_fulltext";

struct TextLine {
	qreal y; // baseline, in the list's content coordinates
	qreal x;
	QString text;
	qreal right = 0; // where the drawn text ends
	QFont font;
	qreal scale = 1;
};

// The text LINE paints in every row of `view`, one entry per drawn line, top to bottom. A blank
// entry stands for a paragraph gap (a line spacing much larger than usual). Empty if LINE was
// painting. `rowWidth` is the widest row, `lineSpacing` the usual distance between lines.
QList<TextLine> paintedLines(QAbstractItemView *view, bool *skipped, qreal *rowWidth, qreal *lineSpacing)
{
	*skipped = false;
	*rowWidth = 0;
	*lineSpacing = 0;
	QList<TextLine> lines;
	QAbstractItemModel *model = view->model();
	if (!model)
		return lines;
	const int scroll = view->verticalScrollBar() ? view->verticalScrollBar()->value() : 0;
	for (int row = 0; row < model->rowCount(view->rootIndex()); ++row) {
		const QRect r = view->visualRect(model->index(row, 0, view->rootIndex()));
		if (r.isEmpty())
			continue;
		const CaptureResult cap = captureRegionText(view->viewport(), r);
		if (cap.skipped) {
			*skipped = true;
			return {};
		}
		for (const CapturedText &it : cap.items) {
			if (it.text.trimmed().isEmpty())
				continue;
			lines.append({ scroll + r.top() + it.pos.y(), it.pos.x(), it.text, it.pos.x() + it.width, it.font, it.scale });
			*rowWidth = qMax(*rowWidth, qreal(r.width()));
		}
	}
	std::stable_sort(lines.begin(), lines.end(), [](const TextLine &a, const TextLine &b) {
		return qAbs(a.y - b.y) >= 1 ? a.y < b.y : a.x < b.x;
	});
	// Fragments on one baseline are one line (LINE draws runs separately).
	QList<TextLine> merged;
	for (const TextLine &l : std::as_const(lines)) {
		if (!merged.isEmpty() && qAbs(merged.last().y - l.y) < 1) {
			merged.last().text += l.text;
			merged.last().right = qMax(merged.last().right, l.right);
		} else {
			merged.append(l);
		}
	}
	// Paragraphs: LINE leaves a taller gap between them than between wrapped lines.
	QList<qreal> gaps;
	for (qsizetype i = 1; i < merged.size(); ++i)
		gaps.append(merged[i].y - merged[i - 1].y);
	if (gaps.isEmpty())
		return merged;
	QList<qreal> sorted = gaps;
	std::sort(sorted.begin(), sorted.end());
	*lineSpacing = sorted[sorted.size() / 2];
	QList<TextLine> out;
	for (qsizetype i = 0; i < merged.size(); ++i) {
		if (i > 0 && *lineSpacing > 0 && gaps[i - 1] > *lineSpacing * 1.7)
			out.append({ merged[i].y - *lineSpacing, 0, QString() });
		out.append(merged[i]);
	}
	return out;
}

bool isThai(QChar c)
{
	return c.unicode() >= 0x0E00 && c.unicode() <= 0x0E7F;
}

// Kana, CJK ideographs and full-width forms: lines may break between any two of them.
bool isCjk(QChar c)
{
	const ushort u = c.unicode();
	return (u >= 0x3000 && u <= 0x30FF) || (u >= 0x3400 && u <= 0x4DBF) || (u >= 0x4E00 && u <= 0x9FFF)
	       || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFFEF);
}

// Whether a line may break between `a` and `b` without a space (after a dash), by Unicode's rules.
bool breaksBetween(QChar a, QChar b)
{
	const QString pair = QString(a) + b;
	QTextBoundaryFinder lines(QTextBoundaryFinder::Line, pair);
	lines.setPosition(1);
	return lines.isAtBoundary();
}

qreal advance(const TextLine &line, const QString &text)
{
	return QFontMetricsF(line.font).horizontalAdvance(text) * line.scale;
}

// The text as its author wrote it: LINE's lines joined where LINE wrapped them, with a line break
// only where the author started a new line. `lineStarts` gets the offset of each of LINE's lines.
//
// LINE draws the text without its line breaks, so they are told apart by geometry: LINE (Qt) moves a
// word to the next line only if it does not fit, so a line followed by one whose first word would
// still have fitted ended with a line break. The right edge is that of the widest line, or as far
// from the row's right edge as the text starts from its left, whichever is further. Lines wrap at
// spaces; a line without one (an address, a run of Thai, which Qt does not split into words) wraps
// anywhere; Chinese / Japanese between any two characters. When the next line starts with a word
// that would not have fitted anyway (often a long run of Thai), a line break cannot be told from a
// wrap: it is taken as a wrap, so no line break is made up (measured on a 10,000-character Thai
// article: all 65 wraps right, 53 of 71 line breaks found).
QString authoredText(const QList<TextLine> &lines, qreal rowWidth, QList<int> *lineStarts, int *wraps)
{
	lineStarts->clear();
	*wraps = 0;
	qreal left = rowWidth, right = 0;
	for (const TextLine &l : lines) {
		if (l.text.isEmpty())
			continue;
		left = qMin(left, l.x);
		right = qMax(right, l.right);
	}
	const qreal limit = qMax(right, rowWidth - left);
	// %LOCALAPPDATA%\LineMR\fulltext-debug: log each line's measurements.
	static const bool debug = QFile::exists(bridgeDirectory() + QStringLiteral("fulltext-debug"));
	if (debug)
		a11yLog("[fulltext] left %.1f, widest %.1f, row %.1f", left, right, rowWidth);
	QString text;
	for (qsizetype i = 0; i < lines.size(); ++i) {
		const TextLine &b = lines[i];
		if (i > 0) {
			const TextLine &a = lines[i - 1];
			QString sep = QStringLiteral("\n");
			if (!a.text.isEmpty() && !b.text.isEmpty()) {
				const bool aSpace = a.text.back().isSpace(), bSpace = b.text.front().isSpace();
				const QString aCore = a.text.trimmed(), bCore = b.text.trimmed(); // not empty (see paintedLines)
				const QChar last = aCore.back(), first = bCore.front();
				QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, bCore);
				const qsizetype graphemeEnd = graphemes.toNextBoundary();
				const qreal firstChar = advance(b, bCore.left(graphemeEnd > 0 ? graphemeEnd : 1));
				const qsizetype space = bCore.indexOf(QLatin1Char(' '));
				const qreal firstWord = advance(b, space < 0 ? bCore : bCore.left(space));
				const qreal spaceWidth = advance(b, QStringLiteral(" "));
				bool wrapped;
				QString join;
				if (aSpace || bSpace) {
					// At a space LINE drew (a's trailing space counts in a.right).
					wrapped = a.right + (bSpace ? spaceWidth : 0) + firstWord > limit + 0.5;
				} else if (isCjk(last) || isCjk(first)) {
					wrapped = a.right + firstChar > limit + 0.5;
				} else if (!isThai(last) && breaksBetween(last, first)) {
					wrapped = a.right + firstWord > limit + 0.5; // after a dash
				} else if (!aCore.contains(QLatin1Char(' ')) && a.right + firstChar > limit + 0.5) {
					wrapped = true; // a line without a space (an address, a run of Thai) wraps anywhere
				} else {
					wrapped = a.right + spaceWidth + firstWord > limit + 0.5;
					join = QStringLiteral(" "); // the space LINE wrapped at
				}
				if (debug)
					a11yLog("[fulltext] line %d: %.1f + %.1f / %.1f of %.1f %s", int(i - 1), a.right,
					        aSpace || bSpace || !join.isEmpty() ? spaceWidth + firstWord : firstChar, firstWord, limit,
					        wrapped ? "wrapped" : "break");
				if (wrapped) {
					sep = join;
					++*wraps;
				}
			}
			text += sep;
		}
		lineStarts->append(int(text.size()));
		text += b.text;
	}
	return text;
}

// Keeps the overlay over the list's viewport and refills it when LINE re-wraps the text (window
// resized). LINE's list does not follow the caret: scrolling it made LINE redraw the long row,
// which froze LINE for 2-15 s while the user read on.
class FullTextView : public QPlainTextEdit {
public:
	explicit FullTextView(QAbstractItemView *view)
		: QPlainTextEdit(view), m_view(view)
	{
		setObjectName(QLatin1String(kViewName));
		setReadOnly(true);
		setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
		setLineWrapMode(QPlainTextEdit::NoWrap); // lines are LINE's (lineStarts()), never this view's
		setFrameStyle(QFrame::NoFrame);
		setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		// Invisible and click-through: LINE's own drawing stays what the user sees.
		setAttribute(Qt::WA_TransparentForMouseEvents);
		// Never repaint: each repaint of this transparent view (every caret move and blink) made LINE
		// redraw the message row under it, a whole long text (2-5 s per step for 10,000 characters),
		// and screen readers waiting for the caret gave up and read the old line again.
		setUpdatesEnabled(false);
		viewport()->setAutoFillBackground(false);
		setStyleSheet(QStringLiteral("QPlainTextEdit { background: transparent; color: transparent; border: none;"
		                             " selection-background-color: transparent; selection-color: transparent; }"));
		view->viewport()->installEventFilter(this);
		m_refill.setSingleShot(true);
		m_refill.setInterval(400);
		QObject::connect(&m_refill, &QTimer::timeout, this, [this] { fill(); });
	}

	// Reads LINE's lines into the view; false if LINE was painting.
	bool fill()
	{
		if (!m_view)
			return false;
		bool skipped = false;
		qreal rowWidth = 0, lineSpacing = 0;
		const QList<TextLine> lines = paintedLines(m_view, &skipped, &rowWidth, &lineSpacing);
		if (skipped || lines.isEmpty())
			return false;
		const int caret = textCursor().position();
		int wraps = 0;
		const QString text = authoredText(lines, rowWidth, &m_lineStarts, &wraps);
		if (text != toPlainText())
			setPlainText(text);
		QTextCursor cursor(document());
		cursor.setPosition(qBound(0, caret, int(text.size())));
		setTextCursor(cursor);
		m_pageLines = lineSpacing > 0 ? qMax(1, int(m_view->viewport()->height() / lineSpacing) - 1) : 10;
		++m_generation;
		setGeometry(m_view->viewport()->geometry());
		a11yLog("[fulltext] %d lines drawn, %d wrapped by LINE, %d paragraphs (row %.0f px)", int(lines.size()), wraps,
		        document()->blockCount(), rowWidth);
		return true;
	}

	// Where each line LINE draws starts in the text (offsets), and a number that changes with them.
	const QList<int> &lineStarts() const { return m_lineStarts; }
	int generation() const { return m_generation; }

	// The line (index into lineStarts()) holding `offset`.
	int lineAt(int offset) const
	{
		const auto it = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), offset);
		return qMax(0, int(it - m_lineStarts.begin()) - 1);
	}
	int lineStart(int line) const { return m_lineStarts.value(line, 0); }
	// One past the line's last character: its line break or the space LINE wrapped at belongs to it.
	int lineEnd(int line) const
	{
		return line + 1 < m_lineStarts.size() ? m_lineStarts[line + 1] : int(toPlainText().size());
	}

protected:
	// Up / Down / Page Up / Page Down / Home / End move by the lines LINE draws, as in a word
	// processor (the document's own lines are the author's paragraphs); Control+Up / Down by
	// paragraph. Shift selects.
	void keyPressEvent(QKeyEvent *e) override
	{
		const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
		const bool shift = mods & Qt::ShiftModifier;
		const Qt::KeyboardModifiers others = mods & ~Qt::ShiftModifier;
		if (m_lineStarts.isEmpty() || (others != Qt::NoModifier && others != Qt::ControlModifier)) {
			QPlainTextEdit::keyPressEvent(e);
			return;
		}
		const QTextCursor cursor = textCursor();
		const int pos = cursor.position();
		const int line = lineAt(pos);
		const int last = int(m_lineStarts.size()) - 1;
		int target = -1;
		bool vertical = false;
		if (others == Qt::ControlModifier) {
			if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
				QTextBlock block = cursor.block();
				if (e->key() == Qt::Key_Up)
					block = pos > block.position() ? block : block.previous();
				else
					block = block.next();
				target = block.isValid() ? block.position() : (e->key() == Qt::Key_Up ? 0 : int(toPlainText().size()));
			}
		} else {
			switch (e->key()) {
			case Qt::Key_Up: target = columnIn(qMax(0, line - 1), pos, line), vertical = true; break;
			case Qt::Key_Down: target = columnIn(qMin(last, line + 1), pos, line), vertical = true; break;
			case Qt::Key_PageUp: target = columnIn(qMax(0, line - m_pageLines), pos, line), vertical = true; break;
			case Qt::Key_PageDown: target = columnIn(qMin(last, line + m_pageLines), pos, line), vertical = true; break;
			case Qt::Key_Home: target = lineStart(line); break;
			case Qt::Key_End: target = lastCaretPosition(line); break;
			default: break;
			}
		}
		if (target < 0) {
			m_goalColumn = -1;
			QPlainTextEdit::keyPressEvent(e);
			return;
		}
		if (!vertical)
			m_goalColumn = -1;
		QTextCursor moved = cursor;
		moved.setPosition(target, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
		setTextCursor(moved);
		m_movedTo = target;
		e->accept();
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (m_view && watched == m_view->viewport() && event->type() == QEvent::Resize) {
			setGeometry(m_view->viewport()->geometry());
			m_refill.start(); // LINE wraps the text anew
		}
		return QPlainTextEdit::eventFilter(watched, event);
	}

private:
	// The last position the caret can take on `line`: its line break, the space LINE wrapped at, or
	// its last character (a line wrapped inside a word); the end of the text on the last line.
	int lastCaretPosition(int line) const
	{
		return line + 1 < m_lineStarts.size() ? qMax(lineStart(line), lineEnd(line) - 1) : lineEnd(line);
	}

	// Where Up / Down from `pos` on `fromLine` land on `line`: the same column as where the vertical
	// moves started, or the line's end if it is shorter.
	int columnIn(int line, int pos, int fromLine)
	{
		if (m_goalColumn < 0 || pos != m_movedTo)
			m_goalColumn = pos - lineStart(fromLine);
		return qMin(lineStart(line) + m_goalColumn, lastCaretPosition(line));
	}

	QPointer<QAbstractItemView> m_view;
	QTimer m_refill;
	QList<int> m_lineStarts;
	int m_generation = 0;
	int m_pageLines = 10;
	int m_goalColumn = -1;
	int m_movedTo = -1;
};

// The text view for screen readers: lines, words and characters of the cached plain text (Qt's own
// interface rebuilds the whole text from the document for every request), and as its value only
// the caret's line, not the whole text.
class LineAccessibleFullText : public QAccessibleWidget, public QAccessibleTextInterface {
public:
	explicit LineAccessibleFullText(QPlainTextEdit *edit)
		: QAccessibleWidget(edit, QAccessible::EditableText)
	{
	}

	void *interface_cast(QAccessible::InterfaceType t) override
	{
		if (t == QAccessible::TextInterface)
			return static_cast<QAccessibleTextInterface *>(this);
		return QAccessibleWidget::interface_cast(t);
	}

	QAccessible::State state() const override
	{
		QAccessible::State st = QAccessibleWidget::state();
		st.readOnly = true;
		st.multiLine = true;
		st.focusable = true;
		return st;
	}

	// Value: the line LINE draws at the caret. Help (UIA HelpText): "lineMR-lines:<generation>:<offsets>",
	// where those lines start, for the add-on: Qt's UI Automation text ranges know only the
	// author's line breaks as lines.
	QString text(QAccessible::Text t) const override
	{
		const FullTextView *view = fullTextView();
		if (t == QAccessible::Value) {
			if (!view || view->lineStarts().isEmpty())
				return edit()->textCursor().block().text();
			const int line = view->lineAt(edit()->textCursor().position());
			return lineText(line);
		}
		if (t == QAccessible::Help && view) {
			QStringList starts;
			starts.reserve(view->lineStarts().size());
			for (int start : view->lineStarts())
				starts.append(QString::number(start));
			return QStringLiteral("lineMR-lines:%1:%2").arg(view->generation()).arg(starts.join(QLatin1Char(',')));
		}
		return QAccessibleWidget::text(t);
	}

	// QAccessibleTextInterface (offsets are document positions: the author's text). Lines are those LINE draws.
	QString textBeforeOffset(int offset, QAccessible::TextBoundaryType type, int *startOffset, int *endOffset) const override
	{
		const FullTextView *view = fullTextView();
		if (type != QAccessible::LineBoundary || !view || view->lineStarts().isEmpty())
			return QAccessibleTextInterface::textBeforeOffset(offset, type, startOffset, endOffset);
		return lineRange(view->lineAt(clampOffset(offset)) - 1, startOffset, endOffset);
	}
	QString textAfterOffset(int offset, QAccessible::TextBoundaryType type, int *startOffset, int *endOffset) const override
	{
		const FullTextView *view = fullTextView();
		if (type != QAccessible::LineBoundary || !view || view->lineStarts().isEmpty())
			return QAccessibleTextInterface::textAfterOffset(offset, type, startOffset, endOffset);
		return lineRange(view->lineAt(clampOffset(offset)) + 1, startOffset, endOffset);
	}
	QString textAtOffset(int offset, QAccessible::TextBoundaryType type, int *startOffset, int *endOffset) const override
	{
		const FullTextView *view = fullTextView();
		if (type != QAccessible::LineBoundary || !view || view->lineStarts().isEmpty())
			return QAccessibleTextInterface::textAtOffset(offset, type, startOffset, endOffset);
		return lineRange(view->lineAt(clampOffset(offset)), startOffset, endOffset);
	}
	void selection(int selectionIndex, int *startOffset, int *endOffset) const override
	{
		const QTextCursor c = edit()->textCursor();
		*startOffset = *endOffset = 0;
		if (selectionIndex == 0 && c.hasSelection()) {
			*startOffset = c.selectionStart();
			*endOffset = c.selectionEnd();
		}
	}
	int selectionCount() const override { return edit()->textCursor().hasSelection() ? 1 : 0; }
	void addSelection(int startOffset, int endOffset) override { setSelection(0, startOffset, endOffset); }
	void removeSelection(int) override
	{
		QTextCursor c = edit()->textCursor();
		c.clearSelection();
		edit()->setTextCursor(c);
	}
	void setSelection(int, int startOffset, int endOffset) override
	{
		QTextCursor c(edit()->document());
		c.setPosition(clampOffset(startOffset));
		c.setPosition(clampOffset(endOffset), QTextCursor::KeepAnchor);
		edit()->setTextCursor(c);
	}
	int cursorPosition() const override { return edit()->textCursor().position(); }
	void setCursorPosition(int position) override
	{
		QTextCursor c = edit()->textCursor();
		c.setPosition(clampOffset(position));
		edit()->setTextCursor(c);
	}
	QString text(int startOffset, int endOffset) const override
	{
		const QString &all = plainText();
		const int start = qBound(0, startOffset, int(all.size()));
		const int end = qBound(start, endOffset, int(all.size()));
		return all.mid(start, end - start);
	}
	int characterCount() const override { return int(plainText().size()); }
	QRect characterRect(int offset) const override
	{
		QTextCursor c(edit()->document());
		c.setPosition(clampOffset(offset));
		QRect r = edit()->cursorRect(c);
		r.setWidth(qMax(r.width(), edit()->fontMetrics().averageCharWidth()));
		return QRect(edit()->viewport()->mapToGlobal(r.topLeft()), r.size());
	}
	int offsetAtPoint(const QPoint &point) const override
	{
		return edit()->cursorForPosition(edit()->viewport()->mapFromGlobal(point)).position();
	}
	void scrollToSubstring(int, int) override {}
	QString attributes(int, int *startOffset, int *endOffset) const override
	{
		*startOffset = 0;
		*endOffset = characterCount();
		return QString();
	}

private:
	QPlainTextEdit *edit() const { return static_cast<QPlainTextEdit *>(widget()); }
	const FullTextView *fullTextView() const { return dynamic_cast<const FullTextView *>(widget()); }

	// Line `line` without its line break; empty (and -1 offsets) outside the text.
	QString lineText(int line) const
	{
		int start = -1, end = -1;
		return lineRange(line, &start, &end).remove(QLatin1Char('\n'));
	}
	QString lineRange(int line, int *startOffset, int *endOffset) const
	{
		const FullTextView *view = fullTextView();
		if (line < 0 || line >= view->lineStarts().size()) {
			*startOffset = *endOffset = -1;
			return QString();
		}
		*startOffset = view->lineStart(line);
		*endOffset = view->lineEnd(line);
		return text(*startOffset, *endOffset);
	}

	int clampOffset(int offset) const { return qBound(0, offset, characterCount()); }

	const QString &plainText() const
	{
		const QTextDocument *doc = edit()->document();
		if (m_text.isNull() || doc->revision() != m_revision) {
			m_text = doc->toPlainText();
			m_revision = doc->revision();
		}
		return m_text;
	}

	mutable QString m_text;
	mutable int m_revision = -1;
};

} // namespace

QAccessibleInterface *fullTextAccessible(QWidget *w)
{
	auto *edit = qobject_cast<QPlainTextEdit *>(w);
	if (!edit || edit->objectName() != QLatin1StringView(kViewName))
		return nullptr;
	return new LineAccessibleFullText(edit);
}

bool attachFullTextView(QWidget *window)
{
	if (!window || window->findChild<QPlainTextEdit *>(QLatin1String(kViewName)))
		return true;
	QAbstractItemView *view = nullptr;
	for (auto *candidate : window->findChildren<QAbstractItemView *>()) {
		if (candidate->isVisible() && inheritsClass(candidate, "LcListView")) {
			view = candidate;
			break;
		}
	}
	if (!view)
		return false;
	auto *text = new FullTextView(view);
	// An interface Qt made before the view had its name is Qt's own: replace it.
	if (QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(text);
	    iface && !dynamic_cast<LineAccessibleFullText *>(iface))
		QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(iface));
	if (!text->fill()) {
		delete text;
		return false;
	}
	const QString title = cleanText(window->windowTitle());
	text->setAccessibleName(title.isEmpty() ? label("Message") : label("Message from %1").arg(title));
	text->show();
	text->raise();
	if (!window->isActiveWindow())
		window->activateWindow();
	text->setFocus(Qt::OtherFocusReason);
	a11yLog("[fulltext] %d lines in a text view over %s", text->document()->blockCount(),
	        window->metaObject()->className());
	return true;
}

} // namespace line_a11y

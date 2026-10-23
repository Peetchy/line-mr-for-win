#include "full_text_view.h"

#include "a11y_log.h"
#include "line_introspect.h"
#include "text_capture.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
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
};

// The text LINE paints in every row of `view`, one entry per drawn line, top to bottom. A blank
// entry stands for a paragraph gap (a line spacing much larger than usual). Empty if LINE was
// painting.
QList<TextLine> paintedLines(QAbstractItemView *view, bool *skipped)
{
	*skipped = false;
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
			lines.append({ scroll + r.top() + it.pos.y(), it.pos.x(), it.text });
		}
	}
	std::stable_sort(lines.begin(), lines.end(), [](const TextLine &a, const TextLine &b) {
		return qAbs(a.y - b.y) >= 1 ? a.y < b.y : a.x < b.x;
	});
	// Fragments on one baseline are one line (LINE draws runs separately).
	QList<TextLine> merged;
	for (const TextLine &l : std::as_const(lines)) {
		if (!merged.isEmpty() && qAbs(merged.last().y - l.y) < 1)
			merged.last().text += l.text;
		else
			merged.append(l);
	}
	// Paragraphs: LINE leaves a taller gap between them than between wrapped lines.
	QList<qreal> gaps;
	for (qsizetype i = 1; i < merged.size(); ++i)
		gaps.append(merged[i].y - merged[i - 1].y);
	if (gaps.isEmpty())
		return merged;
	QList<qreal> sorted = gaps;
	std::sort(sorted.begin(), sorted.end());
	const qreal lineSpacing = sorted[sorted.size() / 2];
	QList<TextLine> out;
	for (qsizetype i = 0; i < merged.size(); ++i) {
		if (i > 0 && lineSpacing > 0 && gaps[i - 1] > lineSpacing * 1.7)
			out.append({ merged[i].y - lineSpacing, 0, QString() });
		out.append(merged[i]);
	}
	return out;
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
		setLineWrapMode(QPlainTextEdit::NoWrap); // one line per line LINE draws
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
		const QList<TextLine> lines = paintedLines(m_view, &skipped);
		if (skipped || lines.isEmpty())
			return false;
		const int caretLine = textCursor().blockNumber();
		QStringList texts;
		for (const TextLine &l : lines)
			texts.append(l.text);
		setPlainText(texts.join(QLatin1Char('\n')));
		QTextCursor cursor(document()->findBlockByNumber(qMin(caretLine, document()->blockCount() - 1)));
		setTextCursor(cursor);
		setGeometry(m_view->viewport()->geometry());
		return true;
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (m_view && watched == m_view->viewport() && event->type() == QEvent::Resize) {
			setGeometry(m_view->viewport()->geometry());
			m_refill.start(); // LINE wraps the text anew
		}
		return QPlainTextEdit::eventFilter(watched, event);
	}

private:
	QPointer<QAbstractItemView> m_view;
	QTimer m_refill;
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

	QString text(QAccessible::Text t) const override
	{
		if (t == QAccessible::Value)
			return edit()->textCursor().block().text();
		return QAccessibleWidget::text(t);
	}

	// QAccessibleTextInterface (offsets are document positions: the plain text, one line break between lines)
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

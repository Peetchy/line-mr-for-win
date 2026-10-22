#include "full_text_view.h"

#include "a11y_log.h"
#include "line_introspect.h"
#include "text_capture.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QTextBlock>
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

// Keeps the overlay over the list's viewport, refills it when LINE re-wraps the text (window
// resized), and scrolls LINE's list to the caret's line.
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
		viewport()->setAutoFillBackground(false);
		setStyleSheet(QStringLiteral("QPlainTextEdit { background: transparent; color: transparent; border: none;"
		                             " selection-background-color: transparent; selection-color: transparent; }"));
		view->viewport()->installEventFilter(this);
		m_refill.setSingleShot(true);
		m_refill.setInterval(400);
		QObject::connect(&m_refill, &QTimer::timeout, this, [this] { fill(); });
		QObject::connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { followCaret(); });
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
		m_lineY.clear();
		for (const TextLine &l : lines) {
			texts.append(l.text);
			m_lineY.append(l.y);
		}
		m_filling = true;
		setPlainText(texts.join(QLatin1Char('\n')));
		QTextCursor cursor(document()->findBlockByNumber(qMin(caretLine, document()->blockCount() - 1)));
		setTextCursor(cursor);
		m_filling = false;
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
	void followCaret()
	{
		const int line = textCursor().blockNumber();
		if (m_filling || !m_view || line < 0 || line >= m_lineY.size())
			return;
		QScrollBar *bar = m_view->verticalScrollBar();
		if (!bar)
			return;
		const int height = m_view->viewport()->height();
		const int y = int(m_lineY[line]);
		// Keep the caret's line inside the middle part of the list.
		if (y < bar->value() + height / 5 || y > bar->value() + height * 4 / 5)
			bar->setValue(y - height / 3);
	}

	QPointer<QAbstractItemView> m_view;
	QList<qreal> m_lineY;
	QTimer m_refill;
	bool m_filling = false;
};

} // namespace

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

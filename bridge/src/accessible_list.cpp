#include "accessible_list.h"

#include "a11y_announce.h"
#include "a11y_event_filter.h"
#include "a11y_log.h"
#include "line_introspect.h"
#include "qt_signal_utils.h"
#include "text_capture.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QMetaProperty>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QEnterEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <memory>

namespace line_a11y {
namespace {

constexpr int kItemDataRole = Qt::UserRole + 1; // MultipleSectionListModel / LcSimpleListModel "itemData"
constexpr const char *kCurrentIndexSignal = "currentIndexChanged()";
// Painted row texts are reused this long (NVDA asks for a name several times per focus change).
constexpr qint64 kRowTextTtlMs = 1500;

// ---- Row text from the model's data object ------------------------------------------------------

// LINE's list models hand out one QObject per row (ChatData, ...). Most expose the row's content as
// Q_PROPERTYs, which is both cheaper and more reliable than reading the painted row.
int propertyPriority(const QByteArray &name)
{
	static const char *const groups[][8] = {
		{ "name", "displayName", "title", "senderName", "nickname", "userName", "fromName", nullptr },
		{ "memberCount", "count", nullptr },
		{ "lastMessage", "text", "message", "content", "statusMessage", "description", "subtitle", nullptr },
		{ "lastUpdatedTimeForUI", "time", "timeText", "date", "dateText", "createdTime", nullptr },
		{ "unreadCount", "badgeCount", "newCount", nullptr },
	};
	for (int g = 0; g < int(std::size(groups)); ++g) {
		for (const char *const *n = groups[g]; *n; ++n) {
			if (name == *n)
				return g;
		}
	}
	return 2; // unknown text-like properties sit with the content
}

bool isIdentifierProperty(const QByteArray &name)
{
	static const char *const suffixes[] = { "Id", "Source", "Path", "Url", "URL", "Uri", "Image", "Icon", "Color",
	                                        "Key", "WithoutQuery", "Query", "Time" };
	// "mid" is LINE's internal user / group id; "elapsedTime" duplicates lastUpdatedTimeForUI.
	if (name == "id" || name == "mid" || name == "objectName" || name == "section" || name == "sectionId"
	    || name == "type" || name == "status")
		return true;
	if (name == "lastUpdatedTimeForUI")
		return false;
	for (const char *suffix : suffixes) {
		if (name.endsWith(suffix))
			return true;
	}
	return false;
}

QString countPhrase(const QByteArray &name, int value)
{
	if (value <= 0)
		return QString();
	if (name == "memberCount")
		return label("%1 members").arg(value);
	if (name == "count") // SectionData: number of entries under a section header
		return QString::number(value);
	if (name == "unreadCount" || name == "badgeCount" || name == "newCount")
		return label("%1 unread").arg(value > 999 ? QStringLiteral("999+") : QString::number(value));
	return QString();
}

// LINE puts small HTML into last messages (call icons, profile links).
QString cleanMessage(const QString &text)
{
	static const QRegularExpression img(QStringLiteral("<img[^>]*>"));
	QString t = text;
	t.remove(img);
	return cleanText(t);
}

QString dataObjectText(const QObject *data)
{
	if (!data)
		return QString();
	struct Part {
		int priority;
		int order;
		QString text;
	};
	QList<Part> parts;
	const QMetaObject *mo = data->metaObject();
	const int first = QObject::staticMetaObject.propertyCount();
	for (int i = first; i < mo->propertyCount(); ++i) {
		const QMetaProperty prop = mo->property(i);
		const QByteArray name = prop.name();
		if (isIdentifierProperty(name))
			continue;
		const QVariant v = prop.read(data);
		QString text;
		if (v.metaType().id() == QMetaType::QString)
			text = cleanMessage(v.toString());
		else if (v.metaType().id() == QMetaType::Int)
			text = countPhrase(name, v.toInt());
		if (!text.isEmpty())
			parts.append({ propertyPriority(name), i, text });
	}
	std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) {
		return a.priority != b.priority ? a.priority < b.priority : a.order < b.order;
	});
	QStringList texts;
	for (const Part &p : std::as_const(parts)) {
		if (!texts.contains(p.text))
			texts << p.text;
	}
	return texts.join(QLatin1StringView(", "));
}

// ---- Row text from what LINE paints ------------------------------------------------------------

// Time ("2:49 PM", "14:49", "14.49 น.") and read receipt ("Read", "Read 3", "อ่านแล้ว") labels.
bool isTimeOrReceipt(const QString &text)
{
	static const QRegularExpression pattern(QStringLiteral(
		"^(\\d{1,2}[:.]\\d{2}(\\s?([AaPp][Mm]|น\\.?))?|Read( \\d+)?|อ่านแล้ว( \\d+)?)$"));
	return pattern.match(text.trimmed()).hasMatch();
}

// The menu LINE draws over photos (Save / Save as... / Share / Send to Keep Memo), painted as one
// line of adjacent labels: not part of the message.
bool isPhotoMenu(const QString &text)
{
	static const QStringList labels = {
		QStringLiteral("บันทึกเป็น..."), QStringLiteral("ส่งไปยัง Keep Memo"), QStringLiteral("บันทึก"),
		QStringLiteral("แชร์"), QStringLiteral("Save as..."), QStringLiteral("Send to Keep Memo"),
		QStringLiteral("Save"), QStringLiteral("Share"),
	};
	QString rest = text;
	rest.remove(u' ');
	bool any = false;
	for (bool progress = true; progress && !rest.isEmpty();) {
		progress = false;
		for (const QString &l : labels) {
			const QString compact = QString(l).remove(u' ');
			if (rest.startsWith(compact)) {
				rest = rest.mid(compact.size());
				progress = any = true;
				break;
			}
		}
	}
	return any && rest.isEmpty();
}

bool isReceipt(const QString &text)
{
	static const QRegularExpression pattern(QStringLiteral("^(Read( \\d+)?|อ่านแล้ว( \\d+)?)$"));
	return pattern.match(text.trimmed()).hasMatch();
}

// A chat message row: sender, content, then time and read receipt (which LINE draws beside the
// bubble). Sent messages are drawn on the right and have no sender name.
QString paintedRowText(QWidget *viewport, const QRect &rowRect, bool messages, bool *skipped)
{
	const CaptureResult cap = captureRegionText(viewport, rowRect);
	*skipped = cap.skipped;
	if (cap.skipped)
		return QString();
	if (!messages) {
		const QString text = cleanText(capturedTextInReadingOrder(cap));
		return text;
	}
	struct Line {
		qreal y;
		qreal x;
		QString text;
	};
	QList<Line> lines;
	for (const CapturedText &it : cap.items) {
		const QString t = it.text.trimmed();
		if (t.isEmpty())
			continue;
		// Fragments on one baseline that touch belong together (LINE draws runs separately).
		if (!lines.isEmpty() && qAbs(lines.last().y - it.pos.y()) < 1)
			lines.last().text += it.text;
		else
			lines.append({ it.pos.y(), it.pos.x(), it.text });
	}
	std::stable_sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) {
		return qAbs(a.y - b.y) > 4 ? a.y < b.y : a.x < b.x;
	});
	// Lines at the same indent are one bubble (joined with a space, or directly between Thai
	// characters, which LINE wraps anywhere); a change of indent (sender name above the bubble,
	// a reply quote) starts a new part.
	auto isThai = [](QChar c) { return c.unicode() >= 0x0E00 && c.unicode() <= 0x0E7F; };
	QStringList content, trailing;
	bool receipt = false;
	qreal minX = rowRect.width();
	qreal lastX = -1000;
	bool photoMenu = false;
	for (const Line &l : std::as_const(lines)) {
		const QString t = cleanText(l.text);
		if (t.isEmpty())
			continue;
		if (isTimeOrReceipt(t)) {
			receipt = receipt || isReceipt(t);
			trailing << t;
			continue;
		}
		if (isPhotoMenu(t)) {
			photoMenu = true;
			continue;
		}
		minX = qMin(minX, l.x);
		if (!content.isEmpty() && qAbs(l.x - lastX) <= 4) {
			content.last() += isThai(content.last().back()) && isThai(t.front()) ? t : QLatin1Char(' ') + t;
		} else {
			content << t;
		}
		lastX = l.x;
	}
	if (content.isEmpty() && trailing.isEmpty())
		return cap.pixmaps > 0 || photoMenu ? label("Photo or sticker") : QString();
	// Date separators and notices are a single centred line without a time.
	if (trailing.isEmpty())
		return content.join(QLatin1StringView(", "));
	// Sent messages are drawn on the right. Rows without text (photos, stickers) are placed by
	// their largest picture; small ones (avatars, icons) do not count.
	bool own = receipt;
	if (!own && !content.isEmpty()) {
		own = minX > rowRect.width() * 0.35;
	} else if (!own) {
		qreal pictureX = -1;
		for (const QRectF &r : cap.pixmapRects) {
			if (r.width() >= 48 && r.height() >= 48)
				pictureX = pictureX < 0 ? r.left() : qMin(pictureX, r.left());
		}
		own = pictureX > rowRect.width() * 0.35;
	}
	if (content.isEmpty())
		content << label("Photo or sticker");
	if (own)
		content.prepend(label("You"));
	return (content + trailing).join(QLatin1StringView(", "));
}

QElapsedTimer &clock()
{
	static QElapsedTimer t;
	if (!t.isValid())
		t.start();
	return t;
}

QString listContextName(const QWidget *view)
{
	struct Context {
		const char *ancestor;
		const char *name;
	};
	static const Context contexts[] = {
		{ "ChatMessageView", "Messages" },
		{ "MainChatPanel", "Chats" },
		{ "MainFriendPanel", "Friends" },
		{ "MainAddFriendPanel", "Add friends" },
		{ "MainCallPanel", "Calls" },
	};
	for (const Context &c : contexts) {
		if (ancestorInheriting(view, c.ancestor))
			return label(c.name);
	}
	return QString();
}

void sendClick(QWidget *target, QPointF local)
{
	QPointer<QWidget> guard(target);
	const QPointF scene = target->mapTo(target->window(), local);
	const QPointF global = target->mapToGlobal(local);
	QEnterEvent enter(local, scene, global);
	QCoreApplication::sendEvent(target, &enter);
	if (!guard)
		return;
	QMouseEvent press(QEvent::MouseButtonPress, local, scene, global, Qt::LeftButton, Qt::LeftButton,
	                  Qt::NoModifier);
	QCoreApplication::sendEvent(target, &press);
	if (!guard)
		return;
	QMouseEvent release(QEvent::MouseButtonRelease, local, scene, global, Qt::LeftButton, Qt::NoButton,
	                    Qt::NoModifier);
	QCoreApplication::sendEvent(target, &release);
}

// One row of an LcListView.
class LineAccessibleListItem : public QAccessibleInterface, public QAccessibleActionInterface {
public:
	LineAccessibleListItem(QAbstractItemView *view, int row)
		: m_view(view), m_row(row)
	{
	}

	QModelIndex index() const
	{
		QAbstractItemModel *model = m_view ? m_view->model() : nullptr;
		return model ? model->index(m_row, 0, m_view->rootIndex()) : QModelIndex();
	}

	bool isValid() const override { return index().isValid(); }
	QObject *object() const override { return nullptr; }
	QWindow *window() const override { return m_view ? m_view->window()->windowHandle() : nullptr; }
	QAccessibleInterface *parent() const override { return QAccessible::queryAccessibleInterface(m_view.data()); }
	QAccessibleInterface *child(int) const override { return nullptr; }
	int childCount() const override { return 0; }
	int indexOfChild(const QAccessibleInterface *) const override { return -1; }
	QAccessibleInterface *childAt(int, int) const override { return nullptr; }
	void setText(QAccessible::Text, const QString &) override {}
	QAccessible::Role role() const override { return QAccessible::ListItem; }

	QString text(QAccessible::Text t) const override
	{
		if (t != QAccessible::Name)
			return QString();
		const QModelIndex idx = index();
		if (!idx.isValid())
			return QString();
		QObject *data = idx.data(kItemDataRole).value<QObject *>();
		if (QString text = dataObjectText(data); !text.isEmpty())
			return text;
		return paintedText(data);
	}

	QRect rect() const override
	{
		const QModelIndex idx = index();
		if (!idx.isValid())
			return QRect();
		const QRect r = m_view->visualRect(idx);
		return QRect(m_view->viewport()->mapToGlobal(r.topLeft()), r.size());
	}

	QAccessible::State state() const override
	{
		QAccessible::State st;
		const QModelIndex idx = index();
		if (!idx.isValid()) {
			st.invalid = true;
			return st;
		}
		st.selectable = true;
		st.focusable = true;
		auto *list = dynamic_cast<LineAccessibleList *>(parent());
		// LINE moves the current row without selecting it; NVDA would say "not selected" on every row.
		st.selected = (list && list->isRowSelected(m_row)) || m_view->currentIndex() == idx;
		// NVDA ignores a focus event unless the element still has focus when it looks: count the
		// current row as focused while LINE briefly pulls focus into the chat being opened.
		st.focused = m_view->currentIndex() == idx && (m_view->hasFocus() || isBrowsingList(m_view));
		const QRect r = m_view->visualRect(idx);
		if (r.isEmpty() || !m_view->viewport()->rect().intersects(r))
			st.offscreen = true;
		if (r.height() <= 1 || !m_view->isVisible())
			st.invisible = true;
		return st;
	}

	void *interface_cast(QAccessible::InterfaceType t) override
	{
		if (t == QAccessible::ActionInterface)
			return static_cast<QAccessibleActionInterface *>(this);
		return nullptr;
	}

	// QAccessibleActionInterface
	QStringList actionNames() const override { return { pressAction(), setFocusAction() }; }
	QStringList keyBindingsForAction(const QString &) const override { return {}; }
	void doAction(const QString &actionName) override
	{
		if (!isValid())
			return;
		QPointer<QAbstractItemView> view = m_view;
		const QPersistentModelIndex index = this->index();
		if (actionName == setFocusAction()) {
			QMetaObject::invokeMethod(view, [view, index] {
				if (view && index.isValid()) {
					view->setFocus(Qt::OtherFocusReason);
					view->setCurrentIndex(index);
				}
			}, Qt::QueuedConnection);
			return;
		}
		if (actionName != pressAction())
			return;
		// LcListView dispatches viewport mouse events to the row's delegate, so a click at the
		// row's centre selects / opens it (opening a chat, folding a section).
		QMetaObject::invokeMethod(view, [view, index] {
			if (!view || !index.isValid())
				return;
			view->scrollTo(index);
			sendClick(view->viewport(), QRectF(view->visualRect(index)).center());
		}, Qt::QueuedConnection);
	}

	int row() const { return m_row; }

private:
	QString paintedText(QObject *data) const
	{
		auto *list = dynamic_cast<LineAccessibleList *>(parent());
		return list ? list->paintedRowText(m_row, data) : QString();
	}

	QPointer<QAbstractItemView> m_view;
	int m_row;
};

} // namespace

bool isLineListView(const QWidget *w)
{
	return w && qobject_cast<const QAbstractItemView *>(w) && inheritsClass(w, "LcListView");
}

// ---- LineAccessibleList --------------------------------------------------------------------------

LineAccessibleList::LineAccessibleList(QWidget *w)
	: QAccessibleWidget(w, QAccessible::List)
{
	connectView();
}

LineAccessibleList::~LineAccessibleList()
{
	for (const QMetaObject::Connection &c : std::as_const(m_connections))
		QObject::disconnect(c);
	for (QAccessible::Id id : std::as_const(m_items)) {
		if (QAccessible::accessibleInterface(id))
			QAccessible::deleteAccessibleInterface(id);
	}
}

QAbstractItemView *LineAccessibleList::view() const
{
	return qobject_cast<QAbstractItemView *>(object());
}

void LineAccessibleList::connectView()
{
	QAbstractItemView *v = view();
	if (!v)
		return;
	const QAccessible::Id listId = QAccessible::uniqueId(this);
	auto notify = [listId] {
		if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::accessibleInterface(listId)))
			list->notifyCurrentRow();
	};
	if (QItemSelectionModel *sm = v->selectionModel())
		m_connections << QObject::connect(sm, &QItemSelectionModel::currentChanged, v, notify);
	if (const int sig = signalIndex(v, kCurrentIndexSignal); sig >= 0)
		m_connections << connectRaw(v, sig, v, [notify](void **) { notify(); });
	if (QAbstractItemModel *model = v->model()) {
		auto prune = [listId] {
			if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::accessibleInterface(listId)))
				list->schedulePrune();
		};
		auto forget = [listId] {
			if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::accessibleInterface(listId)))
				list->m_painted.clear();
		};
		if (ancestorInheriting(v, "ChatMessageView")) {
			// New messages arrive as rows appended at the end; a chat being opened or scrolled
			// back loads many rows at once (or resets the model) and is not announced.
			auto loaded = std::make_shared<QElapsedTimer>();
			loaded->start();
			m_connections << QObject::connect(model, &QAbstractItemModel::modelReset, v, [loaded] { loaded->start(); });
			m_connections << QObject::connect(model, &QAbstractItemModel::rowsInserted, v,
				[listId, loaded](const QModelIndex &, int first, int last) {
					auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::accessibleInterface(listId));
					if (!list || last - first > 2 || loaded->elapsed() < 2000 || last != list->rowCount() - 1)
						return;
					QTimer::singleShot(400, list->view(), [listId] {
						auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::accessibleInterface(listId));
						QAbstractItemView *v = list ? list->view() : nullptr;
						if (!v || !v->isVisible())
							return;
						const int row = list->rowCount() - 1;
						QObject *data = v->model()->index(row, 0, v->rootIndex()).data(kItemDataRole).value<QObject *>();
						const QString text = list->paintedRowText(row, data);
						// The user's own messages were just typed by them.
						if (!text.isEmpty() && !text.startsWith(label("You") + QLatin1Char(',')))
							announce(v, text);
					});
				});
		}
		m_connections << QObject::connect(model, &QAbstractItemModel::rowsRemoved, v, prune)
		              << QObject::connect(model, &QAbstractItemModel::modelReset, v, prune)
		              << QObject::connect(model, &QAbstractItemModel::layoutChanged, v, prune)
		              << QObject::connect(model, &QAbstractItemModel::rowsRemoved, v, forget)
		              << QObject::connect(model, &QAbstractItemModel::modelReset, v, forget)
		              << QObject::connect(model, &QAbstractItemModel::layoutChanged, v, forget);
	}
}

void LineAccessibleList::schedulePrune() const
{
	QAbstractItemView *v = view();
	if (!v || m_prunePending)
		return;
	m_prunePending = true;
	const QAccessible::Id id = QAccessible::uniqueId(const_cast<LineAccessibleList *>(this));
	QTimer::singleShot(0, v, [id] {
		auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::accessibleInterface(id));
		if (!list)
			return;
		list->m_prunePending = false;
		// Items are row positions; only those past the new end go away.
		const int rows = list->rowCount();
		for (auto it = list->m_items.begin(); it != list->m_items.end();) {
			if (it.key() >= rows) {
				if (QAccessible::accessibleInterface(it.value()))
					QAccessible::deleteAccessibleInterface(it.value());
				it = list->m_items.erase(it);
			} else {
				++it;
			}
		}
	});
}

void LineAccessibleList::notifyCurrentRow()
{
	QAbstractItemView *v = view();
	if (!v || !v->isVisible() || !v->hasFocus())
		return;
	const QModelIndex current = v->currentIndex();
	if (!current.isValid())
		return;
	v->scrollTo(current);
	if (QAccessibleInterface *item = itemAt(current.row())) {
		QAccessibleEvent event(item, QAccessible::Focus);
		QAccessible::updateAccessibility(&event);
	}
}

QString LineAccessibleList::paintedRowText(int row, QObject *data) const
{
	QAbstractItemView *v = view();
	if (!v || !v->model())
		return QString();
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	const quintptr key = data ? quintptr(data) : quintptr(row) + 1;
	const qint64 now = clock().elapsed();
	const auto cached = m_painted.constFind(key);
	if (cached != m_painted.cend() && now - cached->at < kRowTextTtlMs)
		return cached->text;
	const QRect r = v->visualRect(idx);
	if (!v->isVisible() || r.isEmpty() || !v->viewport()->rect().intersects(r))
		return cached != m_painted.cend() ? cached->text : QString(); // last text seen on screen
	const bool messages = ancestorInheriting(v, "ChatMessageView") != nullptr || inheritsClass(data, "ChatMessageData");
	bool skipped = false;
	QString text = line_a11y::paintedRowText(v->viewport(), r, messages, &skipped);
	if (skipped) {
		// LINE was painting: answer with what we knew and read the row again shortly; NVDA
		// hears about the real text through a name change.
		const QAccessible::Id listId = QAccessible::uniqueId(const_cast<LineAccessibleList *>(this));
		QTimer::singleShot(300, v, [listId, row] {
			auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::accessibleInterface(listId));
			if (!list || row >= list->rowCount())
				return;
			QAccessibleInterface *item = list->itemAt(row);
			if (!item || item->text(QAccessible::Name).isEmpty())
				return;
			QAccessibleEvent event(item, QAccessible::NameChanged);
			QAccessible::updateAccessibility(&event);
		});
		return cached != m_painted.cend() ? cached->text : QString();
	}
	if (text.isEmpty() && cached != m_painted.cend())
		return cached->text; // row momentarily blank
	// A row partly scrolled out yields part of its text; keep the longer reading.
	if (cached != m_painted.cend() && !v->viewport()->rect().contains(r) && cached->text.size() > text.size())
		text = cached->text;
	if (m_painted.size() > 5000)
		m_painted.clear();
	m_painted.insert(key, { text, now });
	return text;
}

void LineAccessibleList::focusLastVisibleRow()
{
	QAbstractItemView *v = view();
	if (!v || !v->model())
		return;
	const QModelIndex current = v->currentIndex();
	if (current.isValid() && v->viewport()->rect().intersects(v->visualRect(current)))
		return;
	// The newest row that says something (the bottom rows can be blank spacers).
	const QRect area = v->viewport()->rect();
	int lastRow = -1;
	for (int y = area.bottom() - 2; y > area.top(); y -= 8) {
		const QModelIndex idx = v->indexAt(QPoint(area.center().x(), y));
		if (!idx.isValid() || idx.row() == lastRow)
			continue;
		lastRow = idx.row();
		QAccessibleInterface *item = itemAt(idx.row());
		if (item && !item->text(QAccessible::Name).isEmpty()) {
			v->setCurrentIndex(idx);
			return;
		}
	}
}

void LineAccessibleList::focusEdgeRow(bool last)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return;
	const int step = last ? -1 : 1;
	int target = last ? rows - 1 : 0;
	// The rows at the ends can be blank spacers: take the first one from that end with text.
	for (int row = target, tries = 0; row >= 0 && row < rows && tries < 10; row += step, ++tries) {
		v->scrollTo(v->model()->index(row, 0, v->rootIndex()));
		QAccessibleInterface *item = itemAt(row);
		if (item && !item->text(QAccessible::Name).isEmpty()) {
			target = row;
			break;
		}
	}
	const QModelIndex idx = v->model()->index(target, 0, v->rootIndex());
	if (idx == v->currentIndex())
		notifyCurrentRow(); // already there: read it again
	else
		v->setCurrentIndex(idx);
}

QString LineAccessibleList::text(QAccessible::Text t) const
{
	if (t == QAccessible::Name) {
		if (QString name = cleanText(widget()->accessibleName()); !name.isEmpty())
			return name;
		return listContextName(widget());
	}
	return QAccessibleWidget::text(t);
}

QAccessible::State LineAccessibleList::state() const
{
	QAccessible::State st = QAccessibleWidget::state();
	st.focusable = true;
	return st;
}

int LineAccessibleList::childCount() const
{
	return rowCount();
}

QAccessibleInterface *LineAccessibleList::child(int index) const
{
	return itemAt(index);
}

int LineAccessibleList::indexOfChild(const QAccessibleInterface *child) const
{
	const auto *item = dynamic_cast<const LineAccessibleListItem *>(child);
	if (!item || item->parent() != this)
		return -1;
	return item->row();
}

QAccessibleInterface *LineAccessibleList::focusChild() const
{
	QAbstractItemView *v = view();
	if (!v || !v->hasFocus() || !v->currentIndex().isValid())
		return nullptr;
	return itemAt(v->currentIndex().row());
}

QAccessibleInterface *LineAccessibleList::childAt(int x, int y) const
{
	QAbstractItemView *v = view();
	if (!v)
		return nullptr;
	const QModelIndex index = v->indexAt(v->viewport()->mapFromGlobal(QPoint(x, y)));
	return index.isValid() ? itemAt(index.row()) : nullptr;
}

QAccessibleInterface *LineAccessibleList::itemAt(int row) const
{
	QAbstractItemView *v = view();
	if (!v || row < 0 || row >= rowCount())
		return nullptr;
	if (const QAccessible::Id id = m_items.value(row)) {
		if (QAccessibleInterface *item = QAccessible::accessibleInterface(id))
			return item;
	}
	auto *item = new LineAccessibleListItem(v, row);
	m_items.insert(row, QAccessible::registerAccessibleInterface(item));
	return item;
}

int LineAccessibleList::rowCount() const
{
	QAbstractItemView *v = view();
	return v && v->model() ? v->model()->rowCount(v->rootIndex()) : 0;
}

bool LineAccessibleList::isRowSelected(int row) const
{
	QAbstractItemView *v = view();
	if (!v || !v->model())
		return false;
	QItemSelectionModel *sm = v->selectionModel();
	return sm && sm->isRowSelected(row, v->rootIndex());
}

} // namespace line_a11y

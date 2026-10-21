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

// The bridge interface of `view`, looked up whenever it is needed. Never keep an interface's id
// from its constructor: QAccessible::uniqueId() there registers the interface in Qt's cache before
// Qt does, Qt then registers it a second time, and when the view is destroyed Qt deletes it twice
// (LINE's heap-corruption crashes when a window with a list closed or a photo grid went away).
LineAccessibleList *listFor(const QPointer<QAbstractItemView> &view)
{
	return view ? dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(view.data())) : nullptr;
}

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

// "ขนาด 2.6MB" / "Size: 388 KB" under a file's name.
bool isFileSize(const QString &text)
{
	static const QRegularExpression pattern(QStringLiteral("^(ขนาด|Size:?)\\s*[\\d.,]+\\s*(B|KB|MB|GB)$"),
	                                        QRegularExpression::CaseInsensitiveOption);
	return pattern.match(text.trimmed()).hasMatch();
}

// What a message without (much) text is, from what LINE draws for it. ChatMessageData has no
// properties (nor dynamic properties or children), the model returns it for every role, and the row's
// delegate (ChatRecvStickerDelegateWidget, ...) paints without Qt events or signals (probed with
// type-probe), so the drawing is all there is (measured on LINE 26.5):
//  - a voice message has LINE's own play / pause icon in its bubble (recognised by its shape, see
//    knownIcon()) and its length ("00:04") drawn there too;
//  - text bubbles, date pills and notices are images with the text drawn inside them;
//  - a photo is an opaque image drawn under a clip (rounded corners), a video the same with a
//    small play icon at its centre; an album is a grid of equal squares;
//  - a sticker is a large transparent image without a clip (or an empty bubble), LINE emoji are
//    smaller images (~60 px);
//  - a call is a small bubble (~85x76) holding a 24 px phone icon and its length or status;
//  - pictures LINE has not loaded leave only their frame (a large empty rectangle); a video's
//    play button is drawn even then.
// Returns the English label ("Sticker", ...) or nullptr; `lengthBubble` receives the bubble of a
// call or voice message (its length is drawn inside).
const char *messageKind(const CaptureResult &cap, const QRect &rowRect, bool fileSize, QRectF *lengthBubble)
{
	if (fileSize)
		return "File";
	for (const PaintedImage &icon : cap.images) {
		if (icon.icon != QLatin1StringView("voice"))
			continue;
		for (const PaintedImage &bubble : cap.images) {
			if (bubble.rect.width() > icon.rect.width() * 2 && bubble.rect.contains(icon.rect)) {
				*lengthBubble = bubble.rect;
				break;
			}
		}
		return "Voice message";
	}
	auto textInside = [&cap](const QRectF &r) {
		const QRectF area = r.adjusted(-2, -2, 2, 2);
		for (const CapturedText &it : cap.items) {
			if (!it.text.trimmed().isEmpty() && area.contains(it.pos))
				return true;
		}
		return false;
	};
	// Pictures cut off at the top belong to the row above (or show only their lower part).
	auto inRow = [](const QRectF &r) { return r.top() >= -4; };
	auto isAvatar = [](const PaintedImage &pi) {
		return pi.rect.width() >= 30 && pi.rect.width() <= 45 && pi.rect.height() <= 45 && pi.rect.left() < 20;
	};
	auto centredIcon = [&cap](const QRectF &r, int minSide, int maxSide) {
		for (const PaintedImage &icon : cap.images) {
			const QRectF ir = icon.rect;
			if (icon.alpha && ir.width() >= minSide && ir.width() <= maxSide && qAbs(ir.width() - ir.height()) < 3
			    && (ir.center() - r.center()).manhattanLength() < 12)
				return true;
		}
		return false;
	};
	int photos = 0, emoji = 0;
	bool video = false, sticker = false;
	for (const PaintedImage &pi : cap.images) {
		const QRectF &r = pi.rect;
		if (isAvatar(pi) || !inRow(r))
			continue;
		// A call: a small bubble with a phone icon inside (and its length or status as text).
		if (r.height() >= 60 && r.height() <= 95 && r.width() <= 140) {
			for (const PaintedImage &icon : cap.images) {
				if (icon.alpha && icon.rect.width() >= 20 && icon.rect.width() <= 28 && r.contains(icon.rect)) {
					*lengthBubble = r;
					return "Call message";
				}
			}
		}
		if (textInside(r))
			continue;
		// A bubble with nothing written in it is a sticker LINE draws inside a bubble (seen at 150x38).
		if (r.width() >= 100 && r.height() >= 30 && r.height() < 60 && pi.alpha && pi.clearCorners && !pi.clipped) {
			sticker = true;
			continue;
		}
		if (r.width() < 48 || r.height() < 48)
			continue;
		const bool large = r.width() >= 100 || r.height() >= 100;
		if (large && (pi.clipped || !pi.alpha)) {
			++photos;
			video = video || centredIcon(r, 24, 48);
		} else if (large && pi.clearCorners) {
			sticker = true;
		} else if (pi.clearCorners && !pi.clipped) {
			++emoji;
		}
	}
	if (video)
		return "Video";
	if (photos > 1)
		return "Photos";
	if (photos == 1)
		return "Photo";
	if (sticker)
		return "Sticker";
	if (emoji > 0)
		return "LINE emoji";
	// Not loaded yet: empty frames. Several equal squares are an album; a video still has its play
	// button (a local icon) at the centre of the frame.
	int squares = 0;
	bool frame = false;
	bool playButton = false;
	for (const PaintedShape &s : cap.shapes) {
		const QRectF &r = s.rect;
		if (!inRow(r) || r.width() >= rowRect.width() - 20 || textInside(r))
			continue;
		if (r.width() >= 80 && r.width() <= 130 && qAbs(r.width() - r.height()) < 2) {
			++squares;
		} else if (r.width() >= 120 && r.height() >= 120) {
			frame = true;
			for (const PaintedShape &icon : cap.shapes) {
				const QRectF &ir = icon.rect;
				if (ir.width() >= 24 && ir.width() <= 48 && qAbs(ir.width() - ir.height()) < 3
				    && (ir.center() - r.center()).manhattanLength() < 12)
					playButton = true;
			}
			playButton = playButton || centredIcon(r, 24, 48);
		}
	}
	if (squares >= 4) // each tile is drawn twice
		return "Photos";
	if (playButton)
		return "Video";
	return frame ? "Photo" : nullptr;
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
		qreal right;
	};
	QList<Line> lines;
	for (const CapturedText &it : cap.items) {
		const QString t = it.text.trimmed();
		if (t.isEmpty())
			continue;
		// Fragments on one baseline that touch belong together (LINE draws runs separately).
		if (!lines.isEmpty() && qAbs(lines.last().y - it.pos.y()) < 1) {
			lines.last().text += it.text;
			lines.last().right = qMax(lines.last().right, it.pos.x() + it.width);
		} else {
			lines.append({ it.pos.y(), it.pos.x(), it.text, it.pos.x() + it.width });
		}
	}
	std::stable_sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) {
		return qAbs(a.y - b.y) > 4 ? a.y < b.y : a.x < b.x;
	});
	// Lines at the same indent are one bubble (joined with a space, or directly between Thai
	// characters, which LINE wraps anywhere); a change of indent (sender name above the bubble,
	// a reply quote) starts a new part.
	auto isThai = [](QChar c) { return c.unicode() >= 0x0E00 && c.unicode() <= 0x0E7F; };
	QStringList content, trailing;
	QList<qreal> trailingX;
	bool receipt = false;
	qreal minX = rowRect.width();
	qreal maxRight = 0;
	qreal lastX = -1000;
	bool photoMenu = false;
	bool fileSize = false;
	// Received group messages start with the sender's name, beside the avatar at the top.
	bool sender = false;
	for (const Line &l : std::as_const(lines)) {
		const QString t = cleanText(l.text);
		if (t.isEmpty())
			continue;
		fileSize = fileSize || isFileSize(t);
		if (isTimeOrReceipt(t)) {
			receipt = receipt || isReceipt(t);
			trailing << t;
			trailingX << l.x;
			continue;
		}
		if (isPhotoMenu(t)) {
			photoMenu = true;
			continue;
		}
		if (content.isEmpty())
			sender = l.y < 18 && l.x < rowRect.width() * 0.3;
		minX = qMin(minX, l.x);
		maxRight = qMax(maxRight, l.right);
		if (!content.isEmpty() && qAbs(l.x - lastX) <= 4) {
			content.last() += isThai(content.last().back()) && isThai(t.front()) ? t : QLatin1Char(' ') + t;
		} else {
			content << t;
		}
		lastX = l.x;
	}
	QRectF lengthBubble;
	const char *kind = messageKind(cap, rowRect, fileSize, &lengthBubble);
	if (content.isEmpty() && trailing.isEmpty()) {
		if (kind)
			return label(kind);
		return cap.pixmaps > 0 || photoMenu ? label("Photo or sticker") : QString();
	}
	// Date separators and notices are a single centred line without a time (as are tall pictures
	// whose time is out of view: those have a kind).
	if (trailing.isEmpty() && !kind)
		return content.join(QLatin1StringView(", "));
	// A call's or voice message's length ("06:26") is drawn inside its bubble; the time it was sent
	// beside it.
	QStringList lengthText;
	for (qsizetype i = trailing.size() - 1; i >= 0 && !lengthBubble.isEmpty(); --i) {
		if (trailingX[i] >= lengthBubble.left() && trailingX[i] < lengthBubble.right()) {
			lengthText.prepend(trailing.takeAt(i));
			trailingX.removeAt(i);
		}
	}
	// Sent messages are drawn on the right. Rows without text (photos, stickers) are placed by
	// their largest picture; small ones (avatars, icons) do not count.
	bool own = receipt;
	if (!own && !content.isEmpty()) {
		// Notices ("X left the group") are centred and have a time too; sent bubbles end at the right.
		const bool centred = qAbs((minX + maxRight) / 2 - rowRect.width() / 2.0) < rowRect.width() * 0.08;
		own = minX > rowRect.width() * 0.35 && !centred;
	} else if (!own) {
		qreal pictureX = -1;
		for (const QRectF &r : cap.pixmapRects) {
			if (r.width() >= 48 && r.height() >= 48)
				pictureX = pictureX < 0 ? r.left() : qMin(pictureX, r.left());
		}
		own = pictureX > rowRect.width() * 0.35;
	}
	// The type comes after the sender's name, before what the message shows (file name, length).
	if (kind) {
		const qsizetype at = sender && !own ? 1 : 0;
		content.insert(at, label(kind));
		for (qsizetype i = 0; i < lengthText.size(); ++i)
			content.insert(at + 1 + i, lengthText[i]);
	}
	else if (content.isEmpty() || (sender && content.size() == 1 && cap.pixmaps > 1))
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
		// Plain models (Settings' translation languages) give text even for rows off screen.
		if (QString text = cleanText(idx.data(Qt::DisplayRole).toString()); !text.isEmpty())
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
		const int cursor = list ? list->cursorRow() : m_view->currentIndex().row();
		// LINE moves the current row without selecting it; NVDA would say "not selected" on every row.
		if (list && list->hasCursor())
			st.selected = m_row == cursor;
		else
			st.selected = (list && list->isRowSelected(m_row)) || m_view->currentIndex() == idx;
		// NVDA ignores a focus event unless the element still has focus when it looks: count the
		// current row as focused while LINE briefly pulls focus into the chat being opened.
		st.focused = m_row == cursor && (m_view->hasFocus() || isBrowsingList(m_view));
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
	for (const QMetaObject::Connection &c : std::as_const(m_modelConnections))
		QObject::disconnect(c);
	// This destructor runs inside QAccessibleCache::deleteInterface() (the view was destroyed): do not
	// change Qt's cache from here. The row items are invalid once the view is gone; delete them on the
	// next turn of the event loop.
	const QList<QAccessible::Id> items = m_items.values();
	if (!items.isEmpty()) {
		QTimer::singleShot(0, QCoreApplication::instance(), [items] {
			for (QAccessible::Id id : items) {
				if (QAccessible::accessibleInterface(id))
					QAccessible::deleteAccessibleInterface(id);
			}
		});
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
	const QPointer<QAbstractItemView> listView = v;
	auto notify = [listView] {
		if (auto *list = listFor(listView))
			list->notifyCurrentRow();
	};
	if (const int sig = signalIndex(v, kCurrentIndexSignal); sig >= 0)
		m_connections << connectRaw(v, sig, v, [notify](void **) { notify(); });
	connectModel();
}

void LineAccessibleList::checkModel() const
{
	QAbstractItemView *v = view();
	if (v && v->model() != m_model.data())
		const_cast<LineAccessibleList *>(this)->connectModel();
}

// LINE gives the view a new model (and selection model) for each chat it opens: follow it, and
// forget what the old model's rows said.
void LineAccessibleList::connectModel()
{
	for (const QMetaObject::Connection &c : std::as_const(m_modelConnections))
		QObject::disconnect(c);
	m_modelConnections.clear();
	m_painted.clear();
	m_cursor = QPersistentModelIndex();
	QAbstractItemView *v = view();
	m_model = v ? v->model() : nullptr;
	if (!v)
		return;
	const QPointer<QAbstractItemView> listView = v;
	auto notify = [listView] {
		if (auto *list = listFor(listView))
			list->notifyCurrentRow();
	};
	if (QItemSelectionModel *sm = v->selectionModel())
		m_modelConnections << QObject::connect(sm, &QItemSelectionModel::currentChanged, v, notify);
	if (QAbstractItemModel *model = v->model()) {
		auto prune = [listView] {
			if (auto *list = listFor(listView))
				list->schedulePrune();
		};
		auto forget = [listView] {
			if (auto *list = listFor(listView))
				list->m_painted.clear();
		};
		if (ancestorInheriting(v, "ChatMessageView")) {
			// New messages arrive as rows appended at the end; a chat being opened or scrolled
			// back loads many rows at once (or resets the model) and is not announced.
			auto loaded = std::make_shared<QElapsedTimer>();
			loaded->start();
			m_modelConnections << QObject::connect(model, &QAbstractItemModel::modelReset, v, [loaded] { loaded->start(); });
			m_modelConnections << QObject::connect(model, &QAbstractItemModel::rowsInserted, v,
				[listView, loaded](const QModelIndex &, int first, int last) {
					auto *list = listFor(listView);
					if (!list || last - first > 2 || loaded->elapsed() < 2000 || last != list->rowCount() - 1)
						return;
					QTimer::singleShot(400, list->view(), [listView] {
						auto *list = listFor(listView);
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
		m_modelConnections << QObject::connect(model, &QAbstractItemModel::rowsRemoved, v, prune)
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
	const QPointer<QAbstractItemView> listView = v;
	QTimer::singleShot(0, v, [listView] {
		auto *list = listFor(listView);
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
	// While the chat list cursor is elsewhere, LINE moving its current row is not the user's doing.
	if (!v || !v->isVisible() || !v->hasFocus() || hasCursor())
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
	auto cached = m_painted.constFind(key);
	// A message object freed and another one created at the same address is not the same row.
	if (cached != m_painted.cend() && data && cached->object.data() != data) {
		m_painted.remove(key);
		cached = m_painted.cend();
	}
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
		const QPointer<QAbstractItemView> listView = v;
		QTimer::singleShot(300, v, [listView, row] {
			auto *list = listFor(listView);
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
	m_painted.insert(key, { text, now, data });
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
	// Only the text-less part of a tall row (a picture message) is on screen: scroll to the newest
	// row that says something instead of leaving the oldest row current.
	focusEdgeRow(true);
}

void LineAccessibleList::focusEdgeRow(bool last)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return;
	// Rows LINE just added have no position until the view's delayed layout has run.
	v->doItemsLayout();
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
	makeCurrent(target);
}

bool LineAccessibleList::stepRow(int key)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return false;
	const bool page = key == Qt::Key_PageUp || key == Qt::Key_PageDown;
	int step = 0;
	if (key == Qt::Key_Up || key == Qt::Key_PageUp)
		step = -1;
	else if (key == Qt::Key_Down || key == Qt::Key_PageDown)
		step = 1;
	else
		return false;
	const int current = v->currentIndex().isValid() ? v->currentIndex().row() : -1;
	if (current < 0) {
		focusEdgeRow(true);
		return true;
	}
	const int pageHeight = v->viewport()->height();
	int target = current;
	int travelled = 0;
	for (int row = current + step; row >= 0 && row < rows; row += step) {
		const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
		v->scrollTo(idx);
		// Blank rows (spacers) are skipped.
		if (QAccessibleInterface *item = itemAt(row); item && !item->text(QAccessible::Name).isEmpty()) {
			target = row;
			if (!page || travelled >= pageHeight)
				break;
		}
		travelled += v->visualRect(idx).height();
	}
	// At the top LINE loads older messages once the first row is in view; Up then goes on.
	if (target == current)
		v->scrollTo(v->model()->index(step < 0 ? 0 : rows - 1, 0, v->rootIndex()));
	makeCurrent(target);
	return true;
}

void LineAccessibleList::makeCurrent(int row)
{
	QAbstractItemView *v = view();
	if (!v)
		return;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	if (idx != v->currentIndex()) {
		v->setCurrentIndex(idx); // LINE reports the change (notifyCurrentRow)
		return;
	}
	v->scrollTo(idx);
	if (QAccessibleInterface *item = itemAt(row)) {
		QAccessibleEvent event(item, QAccessible::NameChanged);
		QAccessible::updateAccessibility(&event);
	}
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
	if (!v || !v->hasFocus())
		return nullptr;
	const int row = cursorRow();
	return row >= 0 ? itemAt(row) : nullptr;
}

bool LineAccessibleList::moveCursor(int key)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return false;
	int row = cursorRow();
	const int height = v->visualRect(v->model()->index(qMax(row, 0), 0, v->rootIndex())).height();
	const int page = height > 0 ? qMax(1, v->viewport()->height() / height - 1) : 5;
	switch (key) {
	case Qt::Key_Up:
		row = row < 0 ? 0 : row - 1;
		break;
	case Qt::Key_Down:
		row = row < 0 ? 0 : row + 1;
		break;
	case Qt::Key_PageUp:
		row -= page;
		break;
	case Qt::Key_PageDown:
		row = row < 0 ? page : row + page;
		break;
	case Qt::Key_Home:
		row = 0;
		break;
	case Qt::Key_End:
		row = rows - 1;
		break;
	default:
		return false;
	}
	row = qBound(0, row, rows - 1);
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	m_cursor = idx;
	// Scrolling does not change LINE's current row (so no chat is opened).
	v->scrollTo(idx);
	if (QAccessibleInterface *item = itemAt(row)) {
		QAccessibleEvent event(item, QAccessible::Focus);
		QAccessible::updateAccessibility(&event);
	}
	return true;
}

bool LineAccessibleList::openCursorRow()
{
	QAbstractItemView *v = view();
	const QPersistentModelIndex idx = m_cursor;
	m_cursor = QPersistentModelIndex();
	if (!v || !idx.isValid() || idx == QPersistentModelIndex(v->currentIndex()))
		return false;
	// A click opens the chat the way LINE expects (setting the current row alone may not).
	v->scrollTo(idx);
	sendClick(v->viewport(), QRectF(v->visualRect(idx)).center());
	return true;
}

void LineAccessibleList::resetCursor()
{
	m_cursor = QPersistentModelIndex();
}

int LineAccessibleList::cursorRow() const
{
	if (m_cursor.isValid() && m_cursor.model() == m_model.data())
		return m_cursor.row();
	QAbstractItemView *v = view();
	return v && v->currentIndex().isValid() ? v->currentIndex().row() : -1;
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
	checkModel();
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

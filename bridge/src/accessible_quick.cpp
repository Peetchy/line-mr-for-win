#include "accessible_quick.h"

#include "a11y_announce.h"
#include "a11y_log.h"
#include "line_introspect.h"
#include "qt_signal_utils.h"
#include "video_controls.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QMetaProperty>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTextStream>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QAccessibleObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QTextDocumentFragment>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickRenderControl>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <functional>

namespace line_a11y {
namespace {

// Marks items the bridge put into the Tab chain.
constexpr const char *kMadeFocusableProperty = "_lineA11yFocusable";
// The window's item list is rebuilt at most this often (NVDA asks for children repeatedly).
constexpr qint64 kModelTtlMs = 300;

enum class QuickKind { None, Text, Clickable };

// Qt's Text and LINE's own LcQuickText (both keep their text in a "text" property).
bool isTextItem(const QObject *o)
{
	return inheritsAnyClass(o, { "QQuickText", "LcQuickText" });
}

// LINE's own Qt Quick controls (LcQuickButton, clickable LcQuickImage, ...) signal clicked().
bool isLineQuickControl(const QQuickItem *item)
{
	return QLatin1StringView(item->metaObject()->className()).startsWith(QLatin1StringView("LcQuick"))
	    && item->isEnabled() && item->property("userInteractive").toBool() && isSignalConnected(item, "clicked()");
}

bool isInputItem(const QObject *o)
{
	return inheritsAnyClass(o, { "QQuickTextInput", "QQuickTextEdit" });
}

// QML signal handlers (onClicked: ...) count as connections.
bool isClickable(const QQuickItem *item)
{
	if (isLineQuickControl(item))
		return true;
	if (inheritsClass(item, "QQuickMouseArea")) {
		if (!item->isEnabled())
			return false;
		for (const char *signal : { "clicked(QQuickMouseEvent*)", "pressed(QQuickMouseEvent*)",
		                            "released(QQuickMouseEvent*)", "doubleClicked(QQuickMouseEvent*)" }) {
			if (isSignalConnected(item, signal))
				return true;
		}
		return false;
	}
	// A TapHandler is a QObject child of the item it makes clickable.
	for (QObject *child : item->children()) {
		if (inheritsClass(child, "QQuickTapHandler") && child->property("enabled").toBool()
		    && (isSignalConnected(child, "tapped(QEventPoint,Qt::MouseButton)")
		        || isSignalConnected(child, "singleTapped(QEventPoint,Qt::MouseButton)")))
			return true;
	}
	return false;
}

QuickKind kindOf(const QQuickItem *item)
{
	if (isTextItem(item))
		return QuickKind::Text;
	if (isClickable(item))
		return QuickKind::Clickable;
	return QuickKind::None;
}

QRectF sceneRect(const QQuickItem *item)
{
	return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// Visible, opaque, with a size, and not clipped away (e.g. scrolled out of a ListView).
bool isShown(const QQuickItem *item)
{
	if (!item->isVisible() || item->width() <= 0 || item->height() <= 0 || !item->window())
		return false;
	const QRectF rect = sceneRect(item);
	if (!rect.intersects(QRectF(0, 0, item->window()->width(), item->window()->height())))
		return false;
	for (const QQuickItem *p = item; p; p = p->parentItem()) {
		if (p->opacity() <= 0.01)
			return false;
		if (p != item && p->clip() && !rect.intersects(sceneRect(p)))
			return false;
	}
	return true;
}

QString itemText(const QQuickItem *item)
{
	QString text = item->property("text").toString();
	if (text.contains(u'<') && text.contains(u'>'))
		text = QTextDocumentFragment::fromHtml(text).toPlainText();
	return cleanText(text);
}

QString sourceOf(const QQuickItem *item)
{
	const QVariant source = item->property("source");
	return source.metaType().id() == QMetaType::QUrl ? source.toUrl().toString() : source.toString();
}

QString imageName(const QQuickItem *item)
{
	return humanizeIconName(sourceOf(item));
}

// Does `item` or anything inside it show an image whose source contains `part`?
bool showsImage(const QQuickItem *item, QLatin1StringView part)
{
	if (inheritsAnyClass(item, { "QQuickImage", "LcQuickImage", "LcQuickButton" }) && sourceOf(item).contains(part))
		return true;
	for (const QQuickItem *child : item->childItems()) {
		if (child->isVisible() && showsImage(child, part))
			return true;
	}
	return false;
}

// Qt Quick Controls buttons (Button, ToolButton, ...): Qt's own interface, pressed by click().
bool isQtButton(const QObject *o)
{
	return inheritsClass(o, "QQuickAbstractButton");
}

// The visible texts inside `item`, left to right.
QStringList textsInside(const QQuickItem *item)
{
	QList<const QQuickItem *> found;
	const std::function<void(const QQuickItem *)> visit = [&](const QQuickItem *i) {
		if (!i->isVisible() || i->opacity() <= 0.01)
			return;
		if (isTextItem(i) && !itemText(i).isEmpty())
			found << i;
		for (const QQuickItem *child : i->childItems())
			visit(child);
	};
	visit(item);
	std::stable_sort(found.begin(), found.end(), [](const QQuickItem *a, const QQuickItem *b) {
		return sceneRect(a).left() < sceneRect(b).left();
	});
	QStringList texts;
	for (const QQuickItem *t : std::as_const(found))
		if (const QString text = itemText(t); !texts.contains(text))
			texts << text;
	return texts;
}

// Interfaces Qt Quick itself provides for items it considers accessible (text fields, Controls).
bool isQtLeafRole(QAccessible::Role role)
{
	switch (role) {
	case QAccessible::Button:
	case QAccessible::CheckBox:
	case QAccessible::RadioButton:
	case QAccessible::EditableText:
	case QAccessible::ComboBox:
	case QAccessible::Slider:
	case QAccessible::SpinBox:
	case QAccessible::PageTab:
	case QAccessible::MenuItem:
	case QAccessible::Link:
	case QAccessible::StaticText:
	case QAccessible::ProgressBar:
		return true;
	default:
		return false;
	}
}

// ---- The window's presentable items --------------------------------------------------------------

struct QuickModel {
	QList<QPointer<QQuickItem>> order;       // presented items in reading order
	QHash<const QQuickItem *, QString> names; // names of clickable areas
	QHash<const QQuickItem *, QPointer<QQuickItem>> checks; // row -> its selection box (Create group)
	QElapsedTimer built;
};

QHash<const QQuickWindow *, QuickModel> &models()
{
	static QHash<const QQuickWindow *, QuickModel> m;
	return m;
}

bool hasShownContent(QQuickItem *item)
{
	if (!item->isVisible() || item->opacity() <= 0.01)
		return false;
	if (isShown(item) && ((isTextItem(item) && !itemText(item).isEmpty()) || isClickable(item)))
		return true;
	for (QQuickItem *child : item->childItems()) {
		if (hasShownContent(child))
			return true;
	}
	return false;
}

// LINE opens sub-pages (profile -> profile settings) in a window-sized Loader / StackView painted
// over the page it came from; the page underneath stays visible. Returns the last such loaded page
// (children painted before it and lying inside it are covered), or -1.
qsizetype coveringChild(const QList<QQuickItem *> &children)
{
	for (qsizetype i = children.size() - 1; i > 0; --i) {
		QQuickItem *c = children.at(i);
		if (!inheritsAnyClass(c, { "QQuickLoader", "QQuickStackView" }) || !isShown(c))
			continue;
		const QRectF r = sceneRect(c);
		const QQuickWindow *w = c->window();
		if (r.width() * r.height() < 0.9 * w->width() * w->height() || c->z() < 0)
			continue;
		if (hasShownContent(c))
			return i;
	}
	return -1;
}

void collect(QQuickItem *item, QList<QQuickItem *> &texts, QList<QQuickItem *> &clickables,
             QList<QQuickItem *> &images, QList<QQuickItem *> &qtItems)
{
	if (!item->isVisible() || item->opacity() <= 0.01)
		return;
	// A clickable without size is no control (the call footer's Stickers LcQuickButton is 0 px wide
	// beside its picture), but Tab stopped there and NVDA said "Button".
	if (item->activeFocusOnTab() && (item->width() <= 0 || item->height() <= 0) && isClickable(item))
		item->setActiveFocusOnTab(false);
	if (isShown(item)) {
		if (isTextItem(item)) {
			if (!itemText(item).isEmpty())
				texts << item;
			return;
		}
		if (isInputItem(item)) {
			qtItems << item;
			return;
		}
		if (isClickable(item)) {
			clickables << item;
		} else if (inheritsClass(item, "QQuickImage")) {
			images << item;
		} else if (QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
		           iface && isQtLeafRole(iface->role())) {
			qtItems << item;
			return;
		}
	}
	const QList<QQuickItem *> children = item->childItems();
	const qsizetype cover = coveringChild(children);
	const QRectF coverRect = cover >= 0 ? sceneRect(children.at(cover)).adjusted(-1, -1, 1, 1) : QRectF();
	for (qsizetype i = 0; i < children.size(); ++i) {
		if (i < cover && coverRect.contains(sceneRect(children.at(i))))
			continue;
		collect(children.at(i), texts, clickables, images, qtItems);
	}
}

bool isSelectionBox(const QQuickItem *item)
{
	if (!inheritsClass(item, "LcQuickButton") || item->width() > 32 || item->height() > 32)
		return false;
	const QVariant selected = item->property("selected");
	if (selected.metaType().id() != QMetaType::Bool)
		return false;
	for (const char *prop : { "text", "toolTip" }) {
		if (!item->property(prop).toString().isEmpty())
			return false;
	}
	// Empty while unticked, a check-box icon ("check-box-select-ic.svg") while ticked.
	const QString icon = sourceOf(item);
	return icon.isEmpty() || icon.contains(QLatin1StringView("check"));
}

bool readingOrder(const QQuickItem *a, const QQuickItem *b)
{
	const QRectF ra = sceneRect(a), rb = sceneRect(b);
	if (qAbs(ra.top() - rb.top()) > 6)
		return ra.top() < rb.top();
	return ra.left() < rb.left();
}

void nameInput(QQuickItem *item);
void nameQtButton(QQuickItem *item);

const QuickModel &modelFor(QQuickWindow *window)
{
	QuickModel &m = models()[window];
	if (m.built.isValid() && m.built.elapsed() < kModelTtlMs)
		return m;
	m.order.clear();
	m.names.clear();
	m.checks.clear();
	m.built.start();
	QList<QQuickItem *> texts, clickables, images, qtItems;
	if (QQuickItem *root = window->contentItem())
		collect(root, texts, clickables, images, qtItems);
	const qreal windowArea = qreal(window->width()) * window->height();
	// Smallest areas first: a button inside a clickable row claims its own label.
	std::stable_sort(clickables.begin(), clickables.end(), [](const QQuickItem *a, const QQuickItem *b) {
		return a->width() * a->height() < b->width() * b->height();
	});
	QSet<const QQuickItem *> claimed;
	QList<QQuickItem *> presented;
	for (QQuickItem *area : std::as_const(clickables)) {
		// Areas covering most of the window close it or drag it; they are not buttons.
		if (area->width() * area->height() > windowArea * 0.6)
			continue;
		const QRectF rect = sceneRect(area);
		// A bare MouseArea under a row of buttons tracks the mouse (the video call footer, 896x74):
		// not a button of its own, and it would take a label of the buttons.
		if (inheritsClass(area, "QQuickMouseArea")
		    && std::count_if(clickables.cbegin(), clickables.cend(), [area, &rect](const QQuickItem *other) {
			       return other != area && rect.contains(sceneRect(other)) && other->width() * other->height() > 0;
		       }) >= 3)
			continue;
		// A small unlabelled LcQuickButton with a "selected" state inside a clickable row is that
		// row's selection box (friend pickers): the row becomes a checkbox.
		if (isSelectionBox(area)) {
			QQuickItem *row = nullptr;
			for (QQuickItem *other : std::as_const(clickables)) {
				if (other != area && other->width() * other->height() <= windowArea * 0.6
				    && sceneRect(other).contains(rect) && other->width() * other->height() > area->width() * area->height())
					row = other;
				if (row)
					break;
			}
			if (row) {
				m.checks.insert(row, area);
				continue;
			}
		}
		QList<QQuickItem *> inside;
		for (QQuickItem *t : std::as_const(texts)) {
			// Mostly inside: a small edit icon next to a text does not take that text as its label.
			const QRectF tr = sceneRect(t);
			const QRectF common = rect.intersected(tr);
			if (!claimed.contains(t) && common.width() * common.height() >= 0.5 * tr.width() * tr.height())
				inside << t;
		}
		std::stable_sort(inside.begin(), inside.end(), readingOrder);
		QStringList parts;
		// LcQuickButton: its own text or tooltip (the call footer's End call: "buttonText"), else the
		// texts and icons inside it.
		for (const char *prop : { "text", "buttonText", "toolTip" }) {
			if (parts.isEmpty())
				if (const QString own = cleanText(area->property(prop).toString()); !own.isEmpty())
					parts << own;
		}
		for (QQuickItem *t : std::as_const(inside)) {
			claimed.insert(t);
			const QString text = itemText(t);
			if (std::none_of(parts.cbegin(), parts.cend(), [&text](const QString &part) { return part.contains(text); }))
				parts << text;
		}
		// The call footer's LabelButtons put their label under the clickable picture (Stickers).
		if (parts.isEmpty()) {
			for (QQuickItem *p = area->parentItem(); p && parts.isEmpty(); p = p->parentItem()) {
				if (!QLatin1StringView(p->metaObject()->className()).startsWith(QLatin1StringView("LabelButton")))
					continue;
				for (QQuickItem *child : p->childItems()) {
					if (isTextItem(child) && child->isVisible() && !itemText(child).isEmpty()) {
						parts = QStringList { itemText(child) };
						claimed.insert(child);
					}
				}
			}
		}
		// The call window's small picture of the user's own camera.
		if (parts.isEmpty()) {
			for (QQuickItem *p = area->parentItem(); p; p = p->parentItem()) {
				if (QLatin1StringView(p->metaObject()->className()).startsWith(QLatin1StringView("PipView"))) {
					parts << label("Your video");
					break;
				}
			}
		}
		if (parts.isEmpty()) {
			for (QQuickItem *image : std::as_const(images)) {
				if (rect.contains(sceneRect(image).center()))
					if (const QString name = imageName(image); !name.isEmpty() && !parts.contains(name))
						parts << name;
			}
		}
		if (parts.isEmpty())
			if (const QString icon = imageName(area); !icon.isEmpty())
				parts << icon;
		// A picture that is not an icon (profile picture, photo): a file or web image.
		if (parts.isEmpty()) {
			for (QQuickItem *image : std::as_const(images)) {
				if (rect.contains(sceneRect(image).center()) && !image->property("source").toString().isEmpty()) {
					parts << label("Photo");
					break;
				}
			}
		}
		// A MouseArea filling LINE's own picture item (the profile page's round picture): LcQuickImage
		// is no QQuickImage, so it is not among `images`. File or web sources only, not icons.
		if (QQuickItem *picture = area->parentItem(); parts.isEmpty() && picture && inheritsClass(picture, "LcQuickImage")) {
			const QString source = sourceOf(picture);
			if (!source.isEmpty() && !source.startsWith(QLatin1StringView("image://")) && !source.startsWith(QLatin1StringView("qrc:")))
				parts << label(picture->property("cacheCategory").toString().startsWith(QLatin1StringView("profile"))
				                   ? "Profile picture" : "Photo");
		}
		if (parts.isEmpty() && !area->objectName().isEmpty())
			parts << humanizeIdentifier(area->objectName());
		m.names.insert(area, parts.isEmpty() ? label("Button") : parts.join(QStringLiteral(", ")));
		presented << area;
		// Reachable with Tab (LINE's QML has no keyboard support of its own).
		if (!area->activeFocusOnTab()) {
			area->setActiveFocusOnTab(true);
			area->setProperty(kMadeFocusableProperty, true);
		}
	}
	// The small arrow beside a call button (microphone, camera, speaker) opens that device's menu:
	// "<button> options" rather than "Chevron".
	for (QQuickItem *arrow : std::as_const(presented)) {
		if (arrow->width() > 32 || arrow->height() > 32
		    || (!showsImage(arrow, QLatin1StringView("chevron")) && !showsImage(arrow, QLatin1StringView("ac-menu"))))
			continue;
		const QRectF near = sceneRect(arrow).adjusted(-4, -4, 4, 4);
		QQuickItem *owner = nullptr;
		for (QQuickItem *other : std::as_const(presented)) {
			if (other == arrow || !sceneRect(other).intersects(near)
			    || other->width() * other->height() <= arrow->width() * arrow->height())
				continue;
			if (!owner || other->width() * other->height() < owner->width() * owner->height())
				owner = other;
		}
		if (owner)
			m.names.insert(arrow, label("%1 options").arg(m.names.value(owner)));
	}
	for (QQuickItem *t : std::as_const(texts)) {
		if (!claimed.contains(t))
			presented << t;
	}
	for (QQuickItem *item : std::as_const(qtItems)) {
		if (isInputItem(item))
			nameInput(item);
		else if (isQtButton(item))
			nameQtButton(item);
	}
	presented << qtItems;
	// Buttons made focusable earlier on a page now covered by another one leave the Tab chain.
	const QSet<const QQuickItem *> shown(presented.cbegin(), presented.cend());
	const std::function<void(QQuickItem *)> unfocus = [&](QQuickItem *item) {
		if (item->property(kMadeFocusableProperty).toBool() && !shown.contains(item)) {
			item->setActiveFocusOnTab(false);
			item->setProperty(kMadeFocusableProperty, false);
		}
		for (QQuickItem *child : item->childItems())
			unfocus(child);
	};
	if (QQuickItem *root = window->contentItem())
		unfocus(root);
	std::stable_sort(presented.begin(), presented.end(), readingOrder);
	for (QQuickItem *item : std::as_const(presented))
		m.order << item;
	return m;
}

// Text fields are Qt Quick's own. Qt exposes one only once its QML Accessible attached object
// exists, which LINE's plain TextInputs (the friend's display name in a profile) never get: NVDA
// found no field. Give it the role of an editable text (which creates that object), a name from
// its hint (placeholderText) if it has one, and a place in the Tab order (TextInput has none).
void nameInput(QQuickItem *item)
{
	if (item->property("_lineA11yNamed").toBool())
		return;
	item->setProperty("_lineA11yNamed", true);
	if (!item->activeFocusOnTab())
		item->setActiveFocusOnTab(true);
	QQmlContext *context = qmlContext(item);
	if (!context)
		return;
	QQmlProperty role(item, QStringLiteral("Accessible.role"), context);
	if (role.isValid() && role.read().toInt() == int(QAccessible::NoRole))
		role.write(int(QAccessible::EditableText));
	const QString hint = cleanText(item->property("placeholderText").toString());
	QQmlProperty name(item, QStringLiteral("Accessible.name"), context);
	if (!hint.isEmpty() && name.isValid() && name.read().toString().isEmpty())
		name.write(hint);
}

// LINE builds some buttons from Qt Quick Controls with its own contents (the call window's End call
// confirmation: an icon and a text inside a Button), which Qt names from the Button's empty "text".
// Name them from the texts inside.
void nameQtButton(QQuickItem *item)
{
	QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
	if (!iface || !iface->text(QAccessible::Name).isEmpty())
		return;
	const QStringList texts = textsInside(item);
	QQmlContext *context = qmlContext(item);
	if (texts.isEmpty() || !context)
		return;
	QQmlProperty name(item, QStringLiteral("Accessible.name"), context);
	if (name.isValid())
		name.write(texts.join(QStringLiteral(", ")));
}

void tagCallButtons(const QuickModel &m);

// The call window's End call button, if one is shown (tagged by tagCallButtons).
QQuickItem *shownEndCallButton(QQuickWindow *window)
{
	for (const QPointer<QQuickItem> &item : modelFor(window).order) {
		if (item && item->objectName() == QLatin1StringView("lineMR_call-end") && isShown(item))
			return item;
	}
	return nullptr;
}

void clickItem(QQuickItem *item)
{
	QPointer<QQuickItem> guard(item);
	// Deferred: actions arrive inside synchronous UI Automation calls.
	QMetaObject::invokeMethod(item, [guard] {
		QQuickWindow *window = guard ? guard->window() : nullptr;
		if (!window)
			return;
		const QPointF scene = guard->mapToScene(QPointF(guard->width() / 2, guard->height() / 2));
		const QPointF global = window->mapToGlobal(scene);
		breadcrumb("quick click %s in %s", guard->metaObject()->className(), window->metaObject()->className());
		// Closing a call window or pressing its End call button shows a confirmation (End call /
		// Cancel) without moving focus: go to its End call button.
		if (window->property("mouseRollover").isValid()) {
			QPointer<QQuickWindow> callWindow(window);
			QTimer::singleShot(400, window, [callWindow] {
				if (!callWindow)
					return;
				models().remove(callWindow.data());
				tagCallButtons(modelFor(callWindow));
				if (QQuickItem *end = shownEndCallButton(callWindow); end && !end->hasActiveFocus())
					end->forceActiveFocus(Qt::OtherFocusReason);
			});
		}
		// LINE's own controls act on clicked(), like their widget counterparts.
		if (isLineQuickControl(guard)) {
			emitSignal(guard, "clicked()");
			return;
		}
		QMouseEvent press(QEvent::MouseButtonPress, scene, scene, global, Qt::LeftButton, Qt::LeftButton,
		                  Qt::NoModifier);
		QCoreApplication::sendEvent(window, &press);
		QMouseEvent release(QEvent::MouseButtonRelease, scene, scene, global, Qt::LeftButton, Qt::NoButton,
		                    Qt::NoModifier);
		QCoreApplication::sendEvent(window, &release);
		// A picker row: report the new state of its selection box.
		if (modelFor(window).checks.contains(guard.data())) {
			QTimer::singleShot(200, guard, [guard] {
				if (!guard)
					return;
				QAccessible::State changed;
				changed.checked = true;
				QAccessibleStateChangeEvent ev(guard.data(), changed);
				QAccessible::updateAccessibility(&ev);
			});
		}
	}, Qt::QueuedConnection);
}

// ---- Interfaces -----------------------------------------------------------------------------------

class LineQuickItem : public QAccessibleObject, public QAccessibleActionInterface {
public:
	LineQuickItem(QQuickItem *item, QuickKind kind)
		: QAccessibleObject(item), m_kind(kind)
	{
	}

	QQuickItem *item() const { return static_cast<QQuickItem *>(object()); }

	QAccessibleInterface *parent() const override
	{
		return item()->window() ? QAccessible::queryAccessibleInterface(item()->window()) : nullptr;
	}
	QAccessibleInterface *child(int) const override { return nullptr; }
	int childCount() const override { return 0; }
	int indexOfChild(const QAccessibleInterface *) const override { return -1; }
	QWindow *window() const override { return item()->window(); }

	QAccessible::Role role() const override
	{
		if (m_kind != QuickKind::Clickable)
			return QAccessible::StaticText;
		return selectionBox() ? QAccessible::CheckBox : QAccessible::Button;
	}

	// The row's selection box (see modelFor), if it has one.
	QQuickItem *selectionBox() const
	{
		QQuickWindow *w = item()->window();
		return w ? modelFor(w).checks.value(item()).data() : nullptr;
	}

	QString text(QAccessible::Text t) const override
	{
		if (t != QAccessible::Name)
			return QString();
		if (m_kind == QuickKind::Text)
			return itemText(item());
		if (QQuickWindow *w = item()->window())
			return modelFor(w).names.value(item(), label("Button"));
		return QString();
	}

	QRect rect() const override
	{
		QQuickItem *i = item();
		if (!i->window())
			return QRect();
		const QPointF topLeft = i->window()->mapToGlobal(i->mapToScene(QPointF(0, 0)));
		return QRect(topLeft.toPoint(), QSize(int(i->width()), int(i->height())));
	}

	QAccessible::State state() const override
	{
		QAccessible::State st;
		QQuickItem *i = item();
		if (!isShown(i))
			st.invisible = true;
		if (m_kind == QuickKind::Clickable) {
			st.focusable = true;
			st.focused = i->hasActiveFocus();
			if (QQuickItem *box = selectionBox()) {
				st.checkable = true;
				st.checked = box->property("selected").toBool();
			}
		}
		return st;
	}

	void *interface_cast(QAccessible::InterfaceType t) override
	{
		if (t == QAccessible::ActionInterface && m_kind == QuickKind::Clickable)
			return static_cast<QAccessibleActionInterface *>(this);
		return nullptr;
	}

	QStringList actionNames() const override
	{
		if (selectionBox())
			return { pressAction(), toggleAction(), setFocusAction() };
		return { pressAction(), setFocusAction() };
	}
	QStringList keyBindingsForAction(const QString &) const override { return {}; }
	void doAction(const QString &actionName) override
	{
		if (actionName == pressAction() || actionName == toggleAction())
			clickItem(item());
		else if (actionName == setFocusAction())
			item()->forceActiveFocus(Qt::OtherFocusReason);
	}

private:
	QuickKind m_kind;
};

QQuickItem *findNamedItem(QQuickItem *root, QLatin1StringView name)
{
	if (!root)
		return nullptr;
	if (root->objectName() == name)
		return root;
	for (QQuickItem *child : root->childItems()) {
		if (QQuickItem *found = findNamedItem(child, name))
			return found;
	}
	return nullptr;
}

// The first LcQuickButton under a footer LabelButton (#btMic, #btCamera) shows the state as its icon.
QString footerIcon(QQuickWindow *window, QLatin1StringView name)
{
	QQuickItem *labelButton = findNamedItem(window->contentItem(), name);
	if (!labelButton || !isShown(labelButton))
		return QString();
	for (QQuickItem *child : labelButton->childItems()) {
		if (inheritsClass(child, "LcQuickButton"))
			return sourceOf(child);
	}
	return QString();
}

// VideoCallWindow.qml: the other person's camera (#userItem's #imgState "thum-camera-off"), the
// user's microphone and camera (the footer icons "mic_mute_bt", "camera_off_bt").
QString callWindowStatus(QQuickWindow *window)
{
	if (!window->property("mouseRollover").isValid())
		return QString();
	QStringList status;
	QQuickItem *user = findNamedItem(window->contentItem(), QLatin1StringView("userItem"));
	QQuickItem *state = user ? findNamedItem(user, QLatin1StringView("imgState")) : nullptr;
	if (state && isShown(state) && sourceOf(state).contains(QLatin1StringView("camera-off")))
		status << label("%1's camera is off").arg(cleanText(window->title()));
	if (footerIcon(window, QLatin1StringView("btMic")).contains(QLatin1StringView("mic_mute")))
		status << label("Your microphone is off");
	if (footerIcon(window, QLatin1StringView("btCamera")).contains(QLatin1StringView("camera_off")))
		status << label("Your camera is off");
	return status.join(QStringLiteral(", "));
}

class LineQuickWindow : public QAccessibleObject {
public:
	explicit LineQuickWindow(QQuickWindow *window)
		: QAccessibleObject(window)
	{
	}

	QQuickWindow *quickWindow() const { return static_cast<QQuickWindow *>(object()); }

	QAccessibleInterface *parent() const override { return QAccessible::queryAccessibleInterface(qApp); }
	QWindow *window() const override { return quickWindow(); }
	QAccessible::Role role() const override { return QAccessible::Window; }

	int childCount() const override { return int(items().size()); }

	QAccessibleInterface *child(int index) const override
	{
		const auto &order = items();
		if (index < 0 || index >= order.size() || !order[index])
			return nullptr;
		return QAccessible::queryAccessibleInterface(order[index].data());
	}

	int indexOfChild(const QAccessibleInterface *child) const override
	{
		const auto &order = items();
		for (int i = 0; i < order.size(); ++i) {
			if (child && order[i] && child->object() == order[i].data())
				return i;
		}
		return -1;
	}

	QAccessibleInterface *focusChild() const override
	{
		QQuickItem *focus = quickWindow()->activeFocusItem();
		return focus ? QAccessible::queryAccessibleInterface(focus) : nullptr;
	}

	QAccessibleInterface *childAt(int x, int y) const override
	{
		for (int i = 0; i < childCount(); ++i) {
			QAccessibleInterface *c = child(i);
			if (c && c->rect().contains(x, y))
				return c;
		}
		return nullptr;
	}

	QString text(QAccessible::Text t) const override
	{
		// A call window shows its state only as icons; NVDA reads the description with the window.
		if (t == QAccessible::Description)
			return callWindowStatus(quickWindow());
		if (t != QAccessible::Name)
			return QString();
		// LINE titles every window "LINE"; the first text (the page heading) says more.
		const QString title = cleanText(quickWindow()->title());
		if (!title.isEmpty() && title != QLatin1String("LINE"))
			return title;
		// The first real text: a Text, or a button labelled with text (a profile's name), not an icon
		// and not a character counter ("1 / 20" under a name field).
		static const QRegularExpression counter(QStringLiteral("^\\d+\\s*/\\s*\\d+$"));
		for (const QPointer<QQuickItem> &item : items()) {
			if (item && isTextItem(item) && !counter.match(itemText(item)).hasMatch())
				return itemText(item);
			if (item && isLineQuickControl(item)) {
				if (const QString text = cleanText(item->property("text").toString()); !text.isEmpty())
					return text;
				// The profile's name button holds its text in a child.
				if (const QStringList inside = textsInside(item); !inside.isEmpty() && !counter.match(inside.first()).hasMatch())
					return inside.first();
			}
		}
		return title;
	}

	QRect rect() const override { return quickWindow()->frameGeometry(); }

	QAccessible::State state() const override
	{
		QAccessible::State st;
		if (!quickWindow()->isVisible())
			st.invisible = true;
		if (quickWindow()->isActive())
			st.active = true;
		return st;
	}

private:
	const QList<QPointer<QQuickItem>> &items() const { return modelFor(quickWindow()).order; }
};

// ---- Incoming calls --------------------------------------------------------------------------------

// The incoming-call notification (CallMessageToastWindow.qml, also used for chat messages during a
// call) has Decline (voip/notification/refuse_bt.svg) and Answer (accept_bt.svg, or
// video_accept_bt.svg for a video call) buttons. Returns the Answer button, or nullptr.
QQuickItem *incomingCallAnswerButton(const QuickModel &m)
{
	for (const QPointer<QQuickItem> &item : m.order) {
		if (item && m.names.contains(item) && sourceOf(item).contains(QLatin1StringView("voip/notification/"))
		    && sourceOf(item).contains(QLatin1StringView("accept_bt")))
			return item;
	}
	return nullptr;
}

// Tags for the add-on's answer / decline / hang-up commands (UIA AutomationId, see lineMR.py).
void tagCallButtons(const QuickModel &m)
{
	for (const QPointer<QQuickItem> &item : m.order) {
		if (!item || !item->objectName().isEmpty())
			continue;
		const QString source = sourceOf(item);
		if (source.contains(QLatin1StringView("voip/notification/accept_bt")))
			item->setObjectName(QStringLiteral("lineMR_call-answer"));
		else if (source.contains(QLatin1StringView("voip/notification/refuse_bt")))
			item->setObjectName(QStringLiteral("lineMR_call-decline"));
		// Group calls (CallEndButton.qml) and the call footer.
		else if (source.contains(QLatin1StringView("voip/end_button_ic_leave"))
		         || source.contains(QLatin1StringView("voip/footer/hangup"))
		         || source.contains(QLatin1StringView("call_end_bt"))
		         || (isQtButton(item) && showsImage(item, QLatin1StringView("end_button_ic_leave"))))
			item->setObjectName(QStringLiteral("lineMR_call-end"));
	}
}

// "Incoming call: <caller>, <call type>" from the notification's texts.
QString incomingCallText(const QuickModel &m)
{
	QStringList parts;
	for (const QPointer<QQuickItem> &item : m.order) {
		if (item && isTextItem(item) && !m.names.contains(item))
			if (const QString text = itemText(item); !text.isEmpty() && !parts.contains(text))
				parts << text;
	}
	return label("Incoming call") + QStringLiteral(": ") + parts.join(QStringLiteral(", "));
}

void writeCallDump()
{
	QFile f(bridgeDirectory() + QStringLiteral("call-window.txt"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return;
	QTextStream out(&f);
	out.setEncoding(QStringConverter::Utf8);
	dumpQuickWindows(out);
}

bool isOnScreenQuickWindow(QQuickWindow *window)
{
	// The offscreen window behind a QQuickWidget belongs to that widget's own interface.
	return window && !QQuickRenderControl::renderWindowFor(window);
}

void dumpItem(QTextStream &out, QQuickItem *item, int depth)
{
	const QRectF r = sceneRect(item);
	out << QString(depth * 2, QLatin1Char(' ')) << item->metaObject()->className();
	if (!item->objectName().isEmpty())
		out << " #" << item->objectName();
	out << QStringLiteral(" [%1,%2 %3x%4]").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
	if (!item->isVisible())
		out << " hidden";
	if (item->opacity() < 1)
		out << " opacity=" << item->opacity();
	if (isTextItem(item) || isInputItem(item))
		out << " text=\"" << itemText(item) << '"';
	if (inheritsClass(item, "QQuickImage"))
		out << " source=\"" << item->property("source").toString() << '"';
	if (isClickable(item))
		out << " CLICKABLE";
	if (item->activeFocusOnTab())
		out << " tab";
	if (item->hasActiveFocus())
		out << " FOCUS";
	// Properties and connected signals of LINE's own Qt Quick types (LcQuickText, LcQuickButton, ...).
	if (item->isVisible() && QLatin1StringView(item->metaObject()->className()).startsWith(QLatin1StringView("LcQuick"))) {
		for (const QMetaObject *mo = item->metaObject();
		     mo && !QLatin1StringView(mo->className()).startsWith(QLatin1StringView("QQuick")); mo = mo->superClass()) {
			// Only plain values are read: reading some of LINE's other properties crashed LINE.
			for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
				const QMetaProperty p = mo->property(i);
				const int type = p.metaType().id();
				out << ' ' << p.name();
				if (type == QMetaType::QString || type == QMetaType::QUrl || type == QMetaType::Bool
				    || type == QMetaType::Int)
					out << '=' << p.read(item).toString().left(80);
				else
					out << ':' << p.typeName();
			}
			for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
				const QMetaMethod m = mo->method(i);
				if (m.methodType() == QMetaMethod::Signal && isSignalConnected(item, m.methodSignature().constData()))
					out << " connected:" << m.methodSignature();
			}
		}
	}
	out << '\n';
	for (QQuickItem *child : item->childItems())
		dumpItem(out, child, depth + 1);
}

} // namespace

bool isQuickWindow(const QObject *o)
{
	return qobject_cast<const QQuickWindow *>(o) != nullptr;
}

QAccessibleInterface *quickAccessibleFactory(const QString &key, QObject *object)
{
	if (key != QLatin1StringView(object->metaObject()->className()))
		return nullptr;
	if (auto *window = qobject_cast<QQuickWindow *>(object))
		return isOnScreenQuickWindow(window) ? new LineQuickWindow(window) : nullptr;
	// The video player's Play / Pause and Position (in the viewer's QQuickWidget, see video_controls.h).
	if (QAccessibleInterface *control = videoControlAccessible(object))
		return control;
	auto *item = qobject_cast<QQuickItem *>(object);
	if (!item || !isOnScreenQuickWindow(item->window()))
		return nullptr;
	const QuickKind kind = kindOf(item);
	return kind == QuickKind::None ? nullptr : new LineQuickItem(item, kind);
}

void quickWindowShown(QQuickWindow *window)
{
	if (!isOnScreenQuickWindow(window))
		return;
	static QSet<QByteArray> loggedClasses;
	const QByteArray cls = window->metaObject()->className();
	if (!loggedClasses.contains(cls)) {
		loggedClasses.insert(cls);
		a11yLog("[quick] first %s shown (active=%d)", cls.constData(), int(window->isActive()));
	}
	breadcrumb("show quick window %s", cls.constData());
	if (!window->property("_lineA11yWatched").toBool()) {
		window->setProperty("_lineA11yWatched", true);
		// The video call window (VideoCallWindow.qml) shows its controls (microphone, camera, End call,
		// ..., the window buttons) only while the mouse is over it ("mouseRollover"): keyboard users
		// found nothing to Tab to. Keep them shown while the window is active and a screen reader runs.
		if (window->property("mouseRollover").isValid()) {
			QPointer<QQuickWindow> callWindow(window);
			auto *keepShown = new QTimer(window);
			QObject::connect(keepShown, &QTimer::timeout, window, [callWindow] {
				if (callWindow && callWindow->isActive() && QAccessible::isActive()
				    && !callWindow->property("mouseRollover").toBool())
					callWindow->setProperty("mouseRollover", true);
			});
			keepShown->start(250);
		}
		QObject::connect(window, &QObject::destroyed, [window] { models().remove(window); });
		// Qt Quick reports focus only for items it considers accessible.
		QObject::connect(window, &QQuickWindow::activeFocusItemChanged, window, [window] {
			QQuickItem *focus = window->activeFocusItem();
			if (!focus || focus == window->contentItem())
				return;
			// LINE focuses a page's text field itself (profile -> edit name): expose it first.
			if (isInputItem(focus))
				nameInput(focus);
			if (QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(focus)) {
				QAccessibleEvent event(iface, QAccessible::Focus);
				QAccessible::updateAccessibility(&event);
			}
		});
	}
	// LINE fills the page right after showing it. Then: a page opened from LINE comes to the
	// front, and focus starts on its first control (nothing has focus in LINE's QML pages).
	QPointer<QQuickWindow> guard(window);
	QTimer::singleShot(300, window, [guard] {
		if (!guard || !guard->isVisible())
			return;
		models().remove(guard.data());
		const QuickModel &m = modelFor(guard);
		// LINE shows the incoming-call notification without activating it: NVDA hears about it from
		// the announcement even while another application is in front.
		QQuickItem *answer = incomingCallAnswerButton(m);
		tagCallButtons(m);
		if (answer) {
			const QString text = incomingCallText(m);
			a11yLog("[call] incoming call notification %s (%d items)", guard->metaObject()->className(),
			        int(m.order.size()));
			announceIncomingCall(text);
			writeCallDump();
		} else if (QLatin1StringView(guard->metaObject()->className()).startsWith(QLatin1StringView("CallMessageToastWindow"))) {
			return; // a chat message shown during a call: it must not take focus
		}
		if (!guard->isActive() && QGuiApplication::focusWindow())
			guard->requestActivate();
		if (answer) {
			answer->forceActiveFocus(Qt::TabFocusReason);
			return;
		}
		QQuickItem *focus = guard->activeFocusItem();
		if (focus && focus != guard->contentItem() && isShown(focus))
			return;
		// Not on the window buttons along the top edge (close, ...) unless there is nothing else.
		QQuickItem *first = nullptr;
		for (const QPointer<QQuickItem> &item : m.order) {
			if (!item || !(isInputItem(item) || item->activeFocusOnTab()))
				continue;
			if (sceneRect(item).top() >= 40) {
				first = item;
				break;
			}
			if (!first)
				first = item;
		}
		if (first)
			first->forceActiveFocus(Qt::TabFocusReason);
		static int logged = 0;
		if (logged++ < 10)
			a11yLog("[quick] %s: %d items, focus %s", guard->metaObject()->className(), int(m.order.size()),
			        guard->activeFocusItem() ? guard->activeFocusItem()->metaObject()->className() : "-");
	});
}

bool handleQuickKey(QQuickWindow *window, int key)
{
	if (key != Qt::Key_Space && key != Qt::Key_Return && key != Qt::Key_Enter)
		return false;
	QQuickItem *focus = window->activeFocusItem();
	if (!focus)
		return false;
	// Qt Quick Controls buttons take Space themselves, not Enter.
	if (isQtButton(focus) && key != Qt::Key_Space && focus->isEnabled()) {
		if (!QMetaObject::invokeMethod(focus, "click"))
			clickItem(focus);
		return true;
	}
	if (!focus->property(kMadeFocusableProperty).toBool() || !isClickable(focus))
		return false;
	clickItem(focus);
	return true;
}

bool endQuickCall()
{
	for (QWindow *w : QGuiApplication::topLevelWindows()) {
		auto *window = qobject_cast<QQuickWindow *>(w);
		if (!window || !window->isVisible() || !window->property("mouseRollover").isValid())
			continue;
		// CallEndButton.qml: its endButtonClicked() handler disconnects the call. The window's two
		// (in the footer, and the one shown on closing the window) exist while hidden. Their QObject
		// parent is the window, not an item: walk the item tree.
		QList<QQuickItem *> items { window->contentItem() };
		for (qsizetype i = 0; i < items.size(); ++i) {
			QQuickItem *item = items.at(i);
			if (QLatin1StringView(item->metaObject()->className()).startsWith(QLatin1StringView("CallEndButton"))
			    && emitSignal(item, "endButtonClicked()")) {
				a11yLog("[call] ended the call in %s", window->metaObject()->className());
				return true;
			}
			items << item->childItems();
		}
	}
	return false;
}

void dumpQuickWindows(QTextStream &out)
{
	for (QWindow *w : QGuiApplication::topLevelWindows()) {
		auto *quick = qobject_cast<QQuickWindow *>(w);
		// A QQuickWidget's offscreen window is never "visible"; its widget is.
		if (!quick || (!quick->isVisible() && isOnScreenQuickWindow(quick)))
			continue;
		out << "=== quick window " << quick->metaObject()->className() << " \"" << quick->title() << "\""
		    << (quick->isActive() ? " active" : "") << (isOnScreenQuickWindow(quick) ? "" : " offscreen") << '\n';
		if (QQuickItem *root = quick->contentItem())
			dumpItem(out, root, 0);
		out << "--- accessible tree\n";
		if (QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(quick)) {
			out << "window name=\"" << iface->text(QAccessible::Name) << "\"\n";
			for (int i = 0; i < iface->childCount(); ++i) {
				if (QAccessibleInterface *c = iface->child(i))
					out << "  role=" << int(c->role()) << " name=\"" << c->text(QAccessible::Name) << "\" "
					    << (c->object() ? c->object()->metaObject()->className() : "") << '\n';
			}
		}
		out << '\n';
	}
}

} // namespace line_a11y

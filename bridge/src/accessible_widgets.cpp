#include "accessible_widgets.h"
#include "full_text_view.h"

#include "a11y_event_filter.h"
#include "a11y_log.h"
#include "accessible_list.h"
#include "accessible_quick.h"
#include "line_introspect.h"
#include "qt_signal_utils.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QFontMetrics>
#include <QtGui/QEnterEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

#include <typeinfo>

namespace line_a11y {
namespace {

// LcTextField paints its hint itself (placeholderText() stays empty); remember the hint seen
// while the field was empty so the name survives typing.
constexpr const char *kHintProperty = "_lineA11yHint";

constexpr QChar kMaskChar = QChar(0x25CF); // BLACK CIRCLE, as Windows password boxes show

bool firstTrueBoolProp(const QObject *o, std::initializer_list<const char *> names)
{
	for (const char *name : names) {
		const int value = boolProp(o, name);
		if (value >= 0)
			return value == 1;
	}
	return false;
}

QString explicitAccessibleName(const QWidget *w)
{
	return cleanText(w->accessibleName());
}

bool isClickableImage(const QWidget *w);

// From the icon's resource name, or, for a clickable LcImage drawn from a pixmap without a
// source (the call button in the chat header), from which known icon it paints.
QString iconDerivedName(const QWidget *w)
{
	if (const QString source = iconSource(w); !source.isEmpty())
		return humanizeIconName(source);
	if (isClickableImage(w))
		return humanizeIconName(paintedIcon(const_cast<QWidget *>(w)));
	return QString();
}

QString objectNameDerived(const QWidget *w)
{
	const QString on = w->objectName();
	if (on.isEmpty() || on.startsWith(QLatin1StringView("qt_")))
		return QString();
	return humanizeIdentifier(on);
}

bool contextContains(const QWidget *w, QLatin1StringView word, int levels)
{
	const QWidget *p = w;
	for (int i = 0; p && i <= levels; ++i, p = p->parentWidget()) {
		if (p->objectName().contains(word, Qt::CaseInsensitive))
			return true;
		if (QLatin1StringView(p->metaObject()->className()).contains(word, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

QStringList withBaseActions(QStringList actions, const QStringList &base)
{
	for (const QString &a : base) {
		if (!actions.contains(a))
			actions << a;
	}
	return actions;
}

// LcImages LINE uses as icon buttons (e.g. the call button in the chat header) emit clicked();
// decorative images have nobody listening.
bool isClickableImage(const QWidget *w)
{
	return inheritsClass(w, "LcImage")
	    && (isSignalConnected(w, "clicked()") || isSignalConnected(w, "clicked(LcImage*)"));
}

bool isInputClass(const QWidget *w)
{
	if (!qobject_cast<const QLineEdit *>(w) && !qobject_cast<const QTextEdit *>(w))
		return false;
	return inheritsAnyClass(w, { "LcTextField", "PasswordTextField", "LcTextArea", "ChatDetailInputTextEdit" });
}

// LINE's search boxes are plain LcTextFields with a magnifier image and a hidden
// "search" / clear button as children.
bool hasSearchAffordance(const QWidget *w)
{
	for (QObject *child : w->children()) {
		if (!child->isWidgetType() || !inheritsClass(child, "LcButton"))
			continue;
		auto *button = static_cast<QWidget *>(child);
		for (const QString &hint : { iconSource(button), button->objectName() }) {
			if (hint.contains(QLatin1StringView("search"), Qt::CaseInsensitive))
				return true;
		}
	}
	return false;
}

QString heuristicInputLabel(QWidget *w)
{
	auto *le = qobject_cast<QLineEdit *>(w);
	if (inheritsClass(w, "PasswordTextField") || (le && le->echoMode() == QLineEdit::Password))
		return label("Password");
	if (ancestorInheriting(w, "LoginEmailPhoneNumberPanel"))
		return label("Email address");
	if (ancestorInheriting(w, "LoginPasswordlessPanel"))
		return label("Phone number");
	if (inheritsClass(w, "ChatDetailInputTextEdit"))
		return label("Message input");
	// A search box nested inside the chat panel is still a search box, so check the close
	// context for "search"/"filter" before falling back to the chat-panel heuristic.
	const bool inChat = ancestorInheriting(w, "ChatMessageInputPanel") || ancestorInheriting(w, "ChatMessagePanel");
	if (!inChat && hasSearchAffordance(w)) {
		if (ancestorInheriting(w, "MainChatPanel"))
			return label("Search chats");
		if (ancestorInheriting(w, "MainFriendPanel"))
			return label("Search friends");
	}
	if (hasSearchAffordance(w) || contextContains(w, QLatin1StringView("search"), 3)
	    || contextContains(w, QLatin1StringView("filter"), 3))
		return label("Search");
	if (inChat)
		return label("Message input");
	return QString();
}

bool inputIsEmpty(const QWidget *w)
{
	if (auto *le = qobject_cast<const QLineEdit *>(w))
		return le->text().isEmpty();
	if (auto *te = qobject_cast<const QTextEdit *>(w))
		return te->document()->isEmpty();
	return false;
}

QString placeholderOf(QWidget *w)
{
	if (auto *le = qobject_cast<const QLineEdit *>(w); le && !le->placeholderText().isEmpty())
		return cleanText(le->placeholderText());
	if (auto *te = qobject_cast<const QTextEdit *>(w); te && !te->placeholderText().isEmpty())
		return cleanText(te->placeholderText());
	// LcTextField / LcTextArea draw the hint themselves while empty.
	if (inputIsEmpty(w)) {
		QString hint = paintedText(w);
		if (hint.isEmpty()) {
			for (QObject *child : w->children()) {
				if (child->isWidgetType() && inheritsClass(child, "LcText")
				    && !static_cast<QWidget *>(child)->isHidden()) {
					hint = extractLcTextString(static_cast<QWidget *>(child));
					if (!hint.isEmpty())
						break;
				}
			}
		}
		if (!hint.isEmpty() && hint != w->property(kHintProperty).toString())
			w->setProperty(kHintProperty, hint);
		if (!hint.isEmpty())
			return hint;
	}
	return w->property(kHintProperty).toString();
}

// Every widget below `w` (they are drawn into w's own element instead).
bool isDescendant(const QAccessibleInterface *iface, const QWidget *ancestor)
{
	const QObject *o = iface ? iface->object() : nullptr;
	for (; o; o = o->parent()) {
		if (o == ancestor)
			return true;
	}
	return false;
}

} // namespace

// ---- Classification / factory ------------------------------------------------------------------

WidgetKind classifyWidget(const QWidget *w)
{
	if (!w)
		return WidgetKind::None;
	if (isInputClass(w))
		return WidgetKind::Input;
	if (isLineListView(w))
		return WidgetKind::List;
	// The main window and chat windows already carry a useful title.
	if (w->isWindow() && inheritsClass(w, "LcWindow")
	    && !inheritsAnyClass(w, { "AllInOneWindow", "ChatWindow", "LcContextMenu" }))
		return WidgetKind::Window;
	if (inheritsAnyClass(w, { "LcContextMenu", "LcContextMenuItem", "LcCheckableContextMenuItem",
	                          "LcCheckboxContextMenuItem", "LcContextMenuSeperator" }))
		return WidgetKind::Menu;
	if (inheritsClass(w, "LcListDelegateWidget")
	    || (inheritsClass(w, "LcWidget")
	        && QLatin1StringView(w->metaObject()->className()).endsWith(QLatin1StringView("DelegateWidget"))))
		return WidgetKind::Delegate;
	if (inheritsAnyClass(w, { "LcButton", "LcIconTextButton", "LcRadioButton", "LcCheckBox" }) || isClickableImage(w)) {
		// A frame drawn as a button around a list (Settings > Chats: translation languages) is a
		// container: a button would hide the list from screen readers.
		if (w->findChild<QAbstractItemView *>())
			return WidgetKind::None;
		return WidgetKind::Button;
	}
	if (inheritsAnyClass(w, { "LcText", "HighlightableTextWidget", "LcLabel" }))
		return WidgetKind::Text;
	if (inheritsClass(w, "LcScrollArea"))
		return WidgetKind::ScrollArea;
	return WidgetKind::None;
}

QAccessibleInterface *lineAccessibleFactory(const QString &key, QObject *object)
{
	if (!object)
		return nullptr;
	if (!object->isWidgetType())
		return quickAccessibleFactory(key, object);
	// Qt calls the factory once per class in the hierarchy; decide on the most-derived one.
	if (key != QLatin1StringView(object->metaObject()->className()))
		return nullptr;
	auto *w = static_cast<QWidget *>(object);
	if (QAccessibleInterface *iface = fullTextAccessible(w))
		return iface;
	if (helperProcessName() == QLatin1StringView("linemediaplayer") && inheritsClass(w, "QQuickWidget"))
		return new LineAccessibleQuickWidget(w);
	const WidgetKind kind = classifyWidget(w);
	if (kind != WidgetKind::None) {
		static QSet<QByteArray> loggedClasses;
		const QByteArray cls = object->metaObject()->className();
		if (!loggedClasses.contains(cls)) {
			loggedClasses.insert(cls);
			a11yLog("[factory] first bridge interface for %s (kind %d)", cls.constData(), int(kind));
		}
	}
	switch (kind) {
	case WidgetKind::Input:
		return new LineAccessibleInput(w);
	case WidgetKind::Menu:
		return new LineAccessibleMenu(w);
	case WidgetKind::Window:
		return new LineAccessibleWindow(w);
	case WidgetKind::List:
		return new LineAccessibleList(w);
	case WidgetKind::Delegate:
		return new LineAccessibleDelegate(w);
	case WidgetKind::Button:
		return new LineAccessibleButton(w);
	case WidgetKind::Text:
		return new LineAccessibleText(w);
	case WidgetKind::ScrollArea:
		return new LineAccessibleScrollArea(w);
	case WidgetKind::None:
		break;
	}
	return nullptr;
}

void repairCachedInterface(QWidget *w)
{
	if (classifyWidget(w) == WidgetKind::None)
		return;
	QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(w);
	if (!iface || dynamic_cast<LineAccessibleWidget *>(iface) || dynamic_cast<LineAccessibleList *>(iface)
	    || dynamic_cast<LineAccessibleWindow *>(iface) || dynamic_cast<LineAccessibleScrollArea *>(iface))
		return;
	a11yLog("[factory] replacing stale %s interface for %s", typeid(*iface).name(), w->metaObject()->className());
	QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(iface));
}

// ---- Input labelling -----------------------------------------------------------------------------

bool isLineInputWidget(const QWidget *w)
{
	if (!w || (!qobject_cast<const QLineEdit *>(w) && !qobject_cast<const QTextEdit *>(w)))
		return false;
	return isInputClass(w) || ancestorInheriting(w, "LcWidget") != nullptr;
}

bool isMessageInput(const QWidget *w)
{
	if (!qobject_cast<const QTextEdit *>(w) || !isLineInputWidget(w))
		return false;
	return inheritsClass(w, "ChatDetailInputTextEdit") || ancestorInheriting(w, "ChatMessageInputPanel")
	    || ancestorInheriting(w, "ChatMessagePanel");
}

// Name of the chat `input` writes to: the first LcText in the ChatMessagePanel's header (the
// strip with the chatroom-* buttons). The input may sit outside the panel.
QString chatNameFor(const QWidget *input)
{
	QWidget *panel = ancestorInheriting(input, "ChatMessagePanel");
	if (!panel) {
		for (QWidget *candidate : input->window()->findChildren<QWidget *>()) {
			if (inheritsClass(candidate, "ChatMessagePanel") && candidate->isVisible()) {
				panel = candidate;
				break;
			}
		}
	}
	if (!panel)
		return QString();
	for (QObject *child : panel->children()) {
		if (!child->isWidgetType() || !static_cast<QWidget *>(child)->isVisible())
			continue;
		// The header is the first visible child.
		for (QObject *item : child->children()) {
			auto *text = qobject_cast<QWidget *>(item);
			if (text && text->isVisible() && inheritsClass(text, "LcText")) {
				if (const QString name = extractLcTextString(text); !name.isEmpty())
					return name;
			}
		}
		break;
	}
	return QString();
}

QString inputAccessibleName(QWidget *w)
{
	if (!w)
		return QString();
	if (const QString name = explicitAccessibleName(w); !name.isEmpty())
		return name;
	if (isMessageInput(w)) {
		if (const QString chat = chatNameFor(w); !chat.isEmpty())
			return label("Message to %1").arg(chat);
	}
	if (const QString placeholder = placeholderOf(w); !placeholder.isEmpty())
		return placeholder;
	if (const QString heuristic = heuristicInputLabel(w); !heuristic.isEmpty())
		return heuristic;
	if (const QString nearby = nearbyLabelText(w); !nearby.isEmpty())
		return nearby;
	if (const QString above = labelAboveText(w); !above.isEmpty())
		return above;
	if (const QString fromName = objectNameDerived(w); !fromName.isEmpty())
		return fromName;
	return qobject_cast<QTextEdit *>(w) ? label("Text area") : label("Text field");
}

QString inputAccessibleDescription(QWidget *w)
{
	if (!w)
		return QString();
	if (const QString desc = cleanText(w->accessibleDescription()); !desc.isEmpty())
		return desc;
	const QString name = inputAccessibleName(w);
	const QString tip = extractLcWidgetToolTip(w);
	return tip != name ? tip : QString();
}

// ---- LineAccessibleWidget ------------------------------------------------------------------------

LineAccessibleWidget::LineAccessibleWidget(QWidget *w, QAccessible::Role role)
	: QAccessibleWidget(w, role)
{
}

QString LineAccessibleWidget::text(QAccessible::Text t) const
{
	switch (t) {
	case QAccessible::Name:
		return computeName();
	case QAccessible::Description: {
		QString desc = cleanText(widget()->accessibleDescription());
		if (desc.isEmpty())
			desc = computeDescription();
		return desc == computeName() ? QString() : desc;
	}
	case QAccessible::Value:
		return computeValue();
	case QAccessible::Help: {
		const QString tip = toolTipText();
		return tip == computeName() ? QString() : tip;
	}
	default:
		return QAccessibleWidget::text(t);
	}
}

QStringList LineAccessibleWidget::actionNames() const
{
	QStringList actions;
	if (isPressable())
		actions << pressAction();
	return withBaseActions(actions, QAccessibleWidget::actionNames());
}

void LineAccessibleWidget::doAction(const QString &actionName)
{
	endListBrowsing();
	if ((actionName == pressAction() || actionName == toggleAction()) && isPressable()) {
		notePress(widget());
		press();
		return;
	}
	QAccessibleWidget::doAction(actionName);
}

QString LineAccessibleWidget::computeDescription() const
{
	return QString();
}

QString LineAccessibleWidget::computeValue() const
{
	return QString();
}

bool LineAccessibleWidget::isPressable() const
{
	return widget()->isEnabled();
}

void LineAccessibleWidget::press()
{
	QPointer<QWidget> guard(widget());
	// Deferred: doAction() runs inside a synchronous UI Automation call, and a click that opens
	// a modal dialog would otherwise block NVDA until the dialog closes.
	QMetaObject::invokeMethod(widget(), [guard] {
		if (!guard || !guard->isEnabled() || !guard->isVisible())
			return;
		emitSignal(guard, "clicked()");
	}, Qt::QueuedConnection);
}

QString LineAccessibleWidget::toolTipText() const
{
	return extractLcWidgetToolTip(widget());
}

namespace {

// A left click in the middle of `w` the way the mouse makes one: enter, move, press, release.
void sendMouseClick(QWidget *w)
{
	QPointer<QWidget> guard(w);
	const QPointF local = QRectF(w->rect()).center();
	const QPointF scene = w->mapTo(w->window(), local);
	const QPointF global = w->mapToGlobal(local);
	w->setAttribute(Qt::WA_UnderMouse, true);
	QEnterEvent enter(local, scene, global);
	QCoreApplication::sendEvent(w, &enter);
	QMouseEvent move(QEvent::MouseMove, local, scene, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	if (guard)
		QCoreApplication::sendEvent(guard, &move);
	QMouseEvent press(QEvent::MouseButtonPress, local, scene, global, Qt::LeftButton, Qt::LeftButton,
	                  Qt::NoModifier);
	if (guard)
		QCoreApplication::sendEvent(guard, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, local, scene, global, Qt::LeftButton, Qt::NoButton,
	                    Qt::NoModifier);
	if (guard)
		QCoreApplication::sendEvent(guard, &release);
}

} // namespace

// ---- LineAccessibleButton ------------------------------------------------------------------------

LineAccessibleButton::LineAccessibleButton(QWidget *w)
	: LineAccessibleWidget(w, roleFor(w))
{
}

namespace {

// LcCheckBoxes in an LcRadioGroup (sidebar tabs, chat-list filter chips) behave as radio buttons.
bool inRadioGroup(const QWidget *w)
{
	const QVariant group = readProperty(w, "radioGroup");
	return group.isValid() && group.value<QObject *>() != nullptr;
}

// Numeric badge (unread count) drawn by a sibling LcText over the button's corner.
QString overlappingBadge(const QWidget *w)
{
	QWidget *parent = w->parentWidget();
	if (!parent)
		return QString();
	const QRect g = w->geometry().adjusted(-4, -4, 4, 4);
	for (QObject *child : parent->children()) {
		if (child == w || !child->isWidgetType() || !inheritsClass(child, "LcText"))
			continue;
		auto *c = static_cast<QWidget *>(child);
		if (c->isHidden() || !g.intersects(c->geometry()))
			continue;
		const QString text = widgetText(c);
		if (isBadgeText(text))
			return text;
	}
	return QString();
}

// Another visible sibling button laid exactly over this one, later in paint order (so on top and
// receiving the clicks): LINE's ad banner stacks two full-size buttons.
bool coveredByTwin(const QWidget *w)
{
	QWidget *parent = w->parentWidget();
	if (!parent)
		return false;
	bool after = false;
	for (QObject *child : parent->children()) {
		if (child == w) {
			after = true;
			continue;
		}
		if (!after || !child->isWidgetType() || !inheritsClass(child, "LcButton"))
			continue;
		auto *c = static_cast<QWidget *>(child);
		if (!c->isHidden() && c->geometry() == w->geometry())
			return true;
	}
	return false;
}

// The call window: round icon buttons are labelled by an LcText right below them ("Mute",
// "Speaker", ...), and the small arrow laid over a button's corner opens its device menu.
QString callWindowButtonName(const QWidget *w)
{
	QWidget *parent = w->parentWidget();
	if (!parent)
		return QString();
	const QRect g = w->geometry();
	const QList<QObject *> siblings = parent->children();
	if (stringProp(w, "source").contains(QLatin1StringView("menu-up"))) {
		for (QObject *child : siblings) {
			if (child == w || !child->isWidgetType() || !inheritsClass(child, "LcButton"))
				continue;
			auto *c = static_cast<QWidget *>(child);
			if (c->isHidden() || !c->geometry().contains(g.center()) || c->geometry().width() <= g.width())
				continue;
			if (QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(c)) {
				if (QString owner = iface->text(QAccessible::Name); !owner.isEmpty())
					return label("%1 options").arg(owner);
			}
		}
		return QString();
	}
	for (QObject *child : siblings) {
		if (!child->isWidgetType() || !inheritsClass(child, "LcText"))
			continue;
		auto *c = static_cast<QWidget *>(child);
		const QRect t = c->geometry();
		if (c->isHidden() || t.top() < g.bottom() - 2 || t.top() > g.bottom() + 16 || t.left() > g.center().x()
		    || t.right() < g.center().x())
			continue;
		if (QString text = widgetText(c); !text.isEmpty())
			return text;
	}
	return QString();
}

} // namespace

QAccessible::Role LineAccessibleButton::roleFor(const QWidget *w)
{
	if (inheritsClass(w, "LcCheckBox"))
		return inRadioGroup(w) ? QAccessible::RadioButton : QAccessible::CheckBox;
	if (inheritsClass(w, "LcRadioButton"))
		return QAccessible::RadioButton;
	// LcButtons in a radio group switch pages (Settings' section list). Not PageTab: Qt 6.6's UIA
	// reports a tab as selected only while it has focus.
	if (inRadioGroup(w))
		return QAccessible::RadioButton;
	return QAccessible::Button;
}

bool LineAccessibleButton::isCheckable() const
{
	return role() == QAccessible::CheckBox || role() == QAccessible::RadioButton
	    || boolProp(widget(), "checkable") == 1;
}

bool LineAccessibleButton::isChecked() const
{
	return firstTrueBoolProp(widget(), { "checked", "isChecked", "selected" });
}

bool LineAccessibleButton::isSelected() const
{
	return boolProp(widget(), "selected") == 1;
}

QAccessible::State LineAccessibleButton::state() const
{
	QAccessible::State st = LineAccessibleWidget::state();
	if (coveredByTwin(widget()))
		st.invisible = true;
	if (isCheckable()) {
		st.checkable = true;
		st.checked = isChecked();
		if (role() == QAccessible::RadioButton) {
			st.selectable = true;
			st.selected = st.checked;
		}
	} else if (isSelected()) {
		st.selectable = true;
		st.selected = true;
	}
	return st;
}

int LineAccessibleButton::childCount() const
{
	return 0;
}

QAccessibleInterface *LineAccessibleButton::child(int index) const
{
	Q_UNUSED(index);
	return nullptr;
}

int LineAccessibleButton::indexOfChild(const QAccessibleInterface *child) const
{
	Q_UNUSED(child);
	return -1;
}

QStringList LineAccessibleButton::actionNames() const
{
	// UIA Invoke calls pressAction() and Toggle calls toggleAction(); checkables accept both.
	QStringList actions{ pressAction() };
	if (isCheckable())
		actions << toggleAction();
	return withBaseActions(actions, QAccessibleWidget::actionNames());
}

void LineAccessibleButton::doAction(const QString &actionName)
{
	endListBrowsing();
	if ((actionName == pressAction() || actionName == toggleAction()) && widget()->isEnabled()) {
		notePress(widget());
		press();
		return;
	}
	QAccessibleWidget::doAction(actionName);
}

QString LineAccessibleButton::computeName() const
{
	QWidget *w = widget();
	if (QString name = explicitAccessibleName(w); !name.isEmpty())
		return name;
	if (QString text = stringProp(w, "text"); !text.isEmpty())
		return text;
	if (QString children = collectVisibleChildTexts(w, QStringLiteral(" ")); !children.isEmpty())
		return children;
	if (isCheckable()) {
		if (QString lbl = nearbyLabelText(w); !lbl.isEmpty())
			return lbl;
	}
	if (QString tip = toolTipText(); !tip.isEmpty())
		return tip;
	if (ancestorInheriting(w, "CallWindow")) {
		if (QString name = callWindowButtonName(w); !name.isEmpty())
			return name;
	}
	if (QString icon = iconDerivedName(w); !icon.isEmpty())
		return icon;
	if (QString name = objectNameDerived(w); !name.isEmpty())
		return name;
	// A transparent button laid over labels drawn by its siblings or inside a sibling container
	// (the pinned announcement bar; the "+66" country-code picker of the phone login).
	if (QWidget *parent = w->parentWidget()) {
		QStringList covered;
		for (QObject *child : parent->children()) {
			if (child == w || !child->isWidgetType() || inheritsClass(child, "LcButton"))
				continue;
			auto *c = static_cast<QWidget *>(child);
			if (c->isHidden())
				continue;
			QList<QWidget *> texts;
			if (inheritsClass(c, "LcText"))
				texts << c;
			for (QWidget *nested : c->findChildren<QWidget *>()) {
				if (inheritsClass(nested, "LcText") && nested->isVisibleTo(parent))
					texts << nested;
			}
			for (QWidget *t : texts) {
				const QRect g(t->mapTo(parent, QPoint(0, 0)), t->size());
				if (!w->geometry().contains(g.center()))
					continue;
				if (QString text = widgetText(t); !text.isEmpty())
					covered << text;
			}
		}
		if (covered.size() == 1 && covered.first().startsWith(u'+') && isBadgeText(covered.first().mid(1)))
			return label("Country code %1").arg(covered.first());
		if (!covered.isEmpty())
			return covered.join(QLatin1StringView(", "));
	}
	// A bare container button whose icon is a nested LcButton / LcImage.
	for (QWidget *child : w->findChildren<QWidget *>()) {
		if (child->isHidden())
			continue;
		if (QString icon = iconDerivedName(child); !icon.isEmpty())
			return icon;
	}
	// LINE's ad banners: full-size buttons open the ad; the small one in the corner opens the
	// ad's menu (hide / report). None of them carries text, tooltip or icon name.
	if (ancestorInheriting(w, "AdvertisementPanel"))
		return w->width() <= 40 && w->height() <= 40 ? label("Ad options") : label("Advertisement");
	if (inheritsClass(w, "LcImage") && ancestorInheriting(w, "LoginQRCodePanel"))
		return label("QR code");
	// Last resort: whatever the button paints.
	return paintedText(w);
}

void LineAccessibleButton::press()
{
	QWidget *w = widget();
	QPointer<QWidget> guard(w);
	if (inheritsClass(w, "LcImage")) {
		// Like LcButton, LcImage ignores synthetic mouse events; emit its clicked() signals.
		QMetaObject::invokeMethod(w, [guard] {
			if (!guard || !guard->isEnabled() || !guard->isVisible())
				return;
			void *self = guard.data();
			void *args[] = { nullptr, &self };
			emitSignal(guard, "clicked()");
			if (guard)
				emitSignal(guard, "clicked(LcImage*)", args);
		}, Qt::QueuedConnection);
		return;
	}
	QMetaObject::invokeMethod(w, [guard] {
		QWidget *target = guard.data();
		if (!target || !target->isEnabled() || !target->isVisible())
			return;
		// The chat list's folder tabs (All / Friends / Groups / Official accounts) filter the list
		// only on a mouse click: setting `selected` and emitting clicked() ticked the tab but left the
		// list unfiltered.
		if (ancestorInheriting(target, "TabBar")) {
			sendMouseClick(target);
			QTimer::singleShot(150, target, [guard] {
				if (!guard)
					return;
				QAccessible::State changed;
				changed.checked = true;
				QAccessibleStateChangeEvent ev(guard.data(), changed);
				QAccessible::updateAccessibility(&ev);
			});
			return;
		}
		if (inheritsClass(target, "LcCheckBox") || inRadioGroup(target)) {
			const bool selected = boolProp(target, "selected") == 1;
			// A radio-group member stays selected when clicked again. The group deselects the
			// others, which is what hides the previous page of tabbed windows (Settings).
			if (!inRadioGroup(target) || !selected)
				target->setProperty("selected", !selected);
			if (!guard)
				return;
		}
		void *self = target;
		void *args[] = { nullptr, &self };
		emitSignal(target, "clicked()");
		if (guard)
			emitSignal(target, "clicked(LcButton*)", args);
		if (guard) {
			// LINE does not report the new state itself.
			QAccessibleStateChangeEvent ev(target, [] {
				QAccessible::State changed;
				changed.checked = true;
				return changed;
			}());
			QAccessible::updateAccessibility(&ev);
		}
	}, Qt::QueuedConnection);
}

QString LineAccessibleButton::computeDescription() const
{
	if (const QString badge = overlappingBadge(widget()); !badge.isEmpty())
		return label("%1 new").arg(badge);
	if (role() == QAccessible::Button && isSelected())
		return label("Selected");
	return QString();
}

// ---- LineAccessibleText --------------------------------------------------------------------------

LineAccessibleText::LineAccessibleText(QWidget *w)
	: LineAccessibleWidget(w, QAccessible::StaticText)
{
}

QString LineAccessibleText::computeName() const
{
	QWidget *w = widget();
	if (QString text = widgetText(w); !text.isEmpty())
		return text;
	return collectVisibleChildTexts(w, QStringLiteral(" "));
}

bool LineAccessibleText::isPressable() const
{
	QWidget *w = widget();
	if (!w->isEnabled())
		return false;
	if (isSignalConnected(w, "clicked()") || isSignalConnected(w, "linkActivated(QString)"))
		return true;
	return w->testAttribute(Qt::WA_SetCursor) && w->cursor().shape() == Qt::PointingHandCursor;
}

QAccessible::Role LineAccessibleText::role() const
{
	return isPressable() ? QAccessible::Link : QAccessible::StaticText;
}

QStringList LineAccessibleText::actionNames() const
{
	// Plain text must not advertise Invoke, or NVDA calls it "clickable".
	if (!isPressable())
		return {};
	return LineAccessibleWidget::actionNames();
}

QAccessible::State LineAccessibleText::state() const
{
	QAccessible::State st = LineAccessibleWidget::state();
	// Empty text widgets are layout placeholders; keep NVDA from stopping on them.
	if (computeName().isEmpty())
		st.invisible = true;
	if (isPressable())
		st.linked = true;
	return st;
}

// ---- LineAccessibleInput -------------------------------------------------------------------------

LineAccessibleInput::LineAccessibleInput(QWidget *w)
	: LineAccessibleWidget(w, QAccessible::EditableText)
{
}

QLineEdit *LineAccessibleInput::lineEdit() const
{
	return qobject_cast<QLineEdit *>(widget());
}

QTextEdit *LineAccessibleInput::textEdit() const
{
	return qobject_cast<QTextEdit *>(widget());
}

bool LineAccessibleInput::isMasked() const
{
	// PasswordTextField keeps echoMode Normal while empty and only switches once text is
	// entered, so the class alone must already mark it as a password field.
	if (inheritsClass(widget(), "PasswordTextField"))
		return true;
	QLineEdit *le = lineEdit();
	return le && le->echoMode() != QLineEdit::Normal;
}

QString LineAccessibleInput::plainText() const
{
	if (QLineEdit *le = lineEdit())
		return le->text();
	if (QTextEdit *te = textEdit()) {
		// toRawText() keeps one character per document position (needed for offsets).
		QString raw = te->document()->toRawText();
		raw.replace(QChar::ParagraphSeparator, u'\n');
		raw.replace(QChar::LineSeparator, u'\n');
		raw.replace(QChar::Nbsp, u' ');
		return raw;
	}
	return QString();
}

QString LineAccessibleInput::exposedText() const
{
	if (isMasked()) {
		QLineEdit *le = lineEdit();
		if (le && le->echoMode() == QLineEdit::NoEcho)
			return QString();
		return QString(plainText().size(), kMaskChar);
	}
	return plainText();
}

void *LineAccessibleInput::interface_cast(QAccessible::InterfaceType t)
{
	if (t == QAccessible::TextInterface)
		return static_cast<QAccessibleTextInterface *>(this);
	if (t == QAccessible::EditableTextInterface)
		return static_cast<QAccessibleEditableTextInterface *>(this);
	return LineAccessibleWidget::interface_cast(t);
}

QAccessible::State LineAccessibleInput::state() const
{
	QAccessible::State st = LineAccessibleWidget::state();
	bool readOnly = false;
	if (QLineEdit *le = lineEdit())
		readOnly = le->isReadOnly();
	else if (QTextEdit *te = textEdit()) {
		readOnly = te->isReadOnly();
		st.multiLine = true;
	}
	if (isMasked())
		st.passwordEdit = true;
	st.editable = !readOnly;
	st.readOnly = readOnly;
	st.focusable = true;
	st.selectableText = true;
	return st;
}

QStringList LineAccessibleInput::actionNames() const
{
	return { setFocusAction() };
}

void LineAccessibleInput::doAction(const QString &actionName)
{
	endListBrowsing();
	if (actionName == setFocusAction() || actionName == pressAction()) {
		widget()->setFocus(Qt::OtherFocusReason);
		return;
	}
	QAccessibleWidget::doAction(actionName);
}

int LineAccessibleInput::childCount() const
{
	// Keep real buttons (show password, clear search) reachable; drop hint texts and images.
	int n = 0;
	const int all = QAccessibleWidget::childCount();
	for (int i = 0; i < all; ++i) {
		QAccessibleInterface *c = QAccessibleWidget::child(i);
		if (c && c->role() == QAccessible::Button)
			++n;
	}
	return n;
}

QAccessibleInterface *LineAccessibleInput::child(int index) const
{
	const int all = QAccessibleWidget::childCount();
	for (int i = 0, n = 0; i < all; ++i) {
		QAccessibleInterface *c = QAccessibleWidget::child(i);
		if (c && c->role() == QAccessible::Button && n++ == index)
			return c;
	}
	return nullptr;
}

int LineAccessibleInput::indexOfChild(const QAccessibleInterface *childIface) const
{
	if (!isDescendant(childIface, widget()))
		return -1;
	for (int i = 0, n = childCount(); i < n; ++i) {
		if (child(i) == childIface)
			return i;
	}
	return -1;
}

QString LineAccessibleInput::computeName() const
{
	return inputAccessibleName(widget());
}

QString LineAccessibleInput::computeDescription() const
{
	return inputAccessibleDescription(widget());
}

QString LineAccessibleInput::computeValue() const
{
	return exposedText();
}

bool LineAccessibleInput::isPressable() const
{
	return false;
}

void LineAccessibleInput::selection(int selectionIndex, int *startOffset, int *endOffset) const
{
	*startOffset = *endOffset = 0;
	if (selectionIndex != 0)
		return;
	if (QLineEdit *le = lineEdit()) {
		if (le->hasSelectedText()) {
			*startOffset = le->selectionStart();
			*endOffset = le->selectionEnd();
		}
	} else if (QTextEdit *te = textEdit()) {
		const QTextCursor c = te->textCursor();
		if (c.hasSelection()) {
			*startOffset = c.selectionStart();
			*endOffset = c.selectionEnd();
		}
	}
}

int LineAccessibleInput::selectionCount() const
{
	if (QLineEdit *le = lineEdit())
		return le->hasSelectedText() ? 1 : 0;
	if (QTextEdit *te = textEdit())
		return te->textCursor().hasSelection() ? 1 : 0;
	return 0;
}

void LineAccessibleInput::addSelection(int startOffset, int endOffset)
{
	setSelection(0, startOffset, endOffset);
}

void LineAccessibleInput::removeSelection(int selectionIndex)
{
	if (selectionIndex != 0)
		return;
	if (QLineEdit *le = lineEdit()) {
		le->deselect();
	} else if (QTextEdit *te = textEdit()) {
		QTextCursor c = te->textCursor();
		c.clearSelection();
		te->setTextCursor(c);
	}
}

void LineAccessibleInput::setSelection(int selectionIndex, int startOffset, int endOffset)
{
	if (selectionIndex != 0)
		return;
	const int count = characterCount();
	startOffset = qBound(0, startOffset, count);
	endOffset = qBound(0, endOffset, count);
	if (QLineEdit *le = lineEdit()) {
		le->setSelection(startOffset, endOffset - startOffset);
	} else if (QTextEdit *te = textEdit()) {
		QTextCursor c(te->document());
		c.setPosition(startOffset);
		c.setPosition(endOffset, QTextCursor::KeepAnchor);
		te->setTextCursor(c);
	}
}

int LineAccessibleInput::cursorPosition() const
{
	if (QLineEdit *le = lineEdit())
		return le->cursorPosition();
	if (QTextEdit *te = textEdit())
		return te->textCursor().position();
	return 0;
}

void LineAccessibleInput::setCursorPosition(int position)
{
	position = qBound(0, position, characterCount());
	if (QLineEdit *le = lineEdit()) {
		le->setCursorPosition(position);
	} else if (QTextEdit *te = textEdit()) {
		QTextCursor c = te->textCursor();
		c.setPosition(position);
		te->setTextCursor(c);
	}
}

QString LineAccessibleInput::text(int startOffset, int endOffset) const
{
	const QString all = exposedText();
	startOffset = qBound(0, startOffset, int(all.size()));
	endOffset = qBound(startOffset, endOffset, int(all.size()));
	return all.mid(startOffset, endOffset - startOffset);
}

int LineAccessibleInput::characterCount() const
{
	if (QTextEdit *te = textEdit())
		return qMax(0, te->document()->characterCount() - 1);
	return int(exposedText().size());
}

QRect LineAccessibleInput::characterRect(int offset) const
{
	if (QLineEdit *le = lineEdit()) {
		// QLineEdit only exposes the caret rectangle publicly; measure from there so that
		// horizontal scrolling and text margins are accounted for.
		const QString display = le->displayText();
		offset = qBound(0, offset, int(display.size()));
		const QFontMetrics fm(le->font());
		const QRect caret = le->inputMethodQuery(Qt::ImCursorRectangle).toRect();
		const int cursor = le->cursorPosition();
		int x = caret.x();
		if (offset >= cursor)
			x += fm.horizontalAdvance(display.mid(cursor, offset - cursor));
		else
			x -= fm.horizontalAdvance(display.mid(offset, cursor - offset));
		const int width = offset < display.size() ? fm.horizontalAdvance(display.at(offset)) : 1;
		const QRect local(x, caret.y(), qMax(1, width), caret.height());
		return QRect(le->mapToGlobal(local.topLeft()), local.size());
	}
	if (QTextEdit *te = textEdit()) {
		QTextCursor c(te->document());
		c.setPosition(qBound(0, offset, characterCount()));
		const QRect caret = te->cursorRect(c);
		const QString all = plainText();
		const QFontMetrics fm(c.charFormat().font());
		const int width = offset >= 0 && offset < all.size() && all.at(offset) != u'\n'
			? fm.horizontalAdvance(all.at(offset)) : 1;
		const QRect local(caret.x(), caret.y(), qMax(1, width), caret.height());
		return QRect(te->viewport()->mapToGlobal(local.topLeft()), local.size());
	}
	return QRect();
}

int LineAccessibleInput::offsetAtPoint(const QPoint &point) const
{
	if (QLineEdit *le = lineEdit())
		return le->cursorPositionAt(le->mapFromGlobal(point));
	if (QTextEdit *te = textEdit())
		return te->cursorForPosition(te->viewport()->mapFromGlobal(point)).position();
	return -1;
}

void LineAccessibleInput::scrollToSubstring(int startIndex, int endIndex)
{
	if (QLineEdit *le = lineEdit()) {
		const int keep = le->cursorPosition();
		le->setCursorPosition(qBound(0, endIndex, int(le->text().size())));
		le->setCursorPosition(qBound(0, startIndex, int(le->text().size())));
		le->setCursorPosition(keep);
	} else if (QTextEdit *te = textEdit()) {
		const QTextCursor keep = te->textCursor();
		QTextCursor c(te->document());
		c.setPosition(qBound(0, endIndex, characterCount()));
		te->setTextCursor(c);
		te->ensureCursorVisible();
		c.setPosition(qBound(0, startIndex, characterCount()));
		te->setTextCursor(c);
		te->ensureCursorVisible();
		te->setTextCursor(keep);
	}
}

QString LineAccessibleInput::attributes(int offset, int *startOffset, int *endOffset) const
{
	Q_UNUSED(offset);
	*startOffset = 0;
	*endOffset = characterCount();
	return QString();
}

void LineAccessibleInput::deleteText(int startOffset, int endOffset)
{
	replaceText(startOffset, endOffset, QString());
}

void LineAccessibleInput::insertText(int offset, const QString &text)
{
	replaceText(offset, offset, text);
}

void LineAccessibleInput::replaceText(int startOffset, int endOffset, const QString &text)
{
	const int count = characterCount();
	startOffset = qBound(0, startOffset, count);
	endOffset = qBound(startOffset, endOffset, count);
	if (QLineEdit *le = lineEdit()) {
		if (le->isReadOnly())
			return;
		// Selection + insert() keeps the edit on QLineEdit's undo stack (setText() would not).
		le->setSelection(startOffset, endOffset - startOffset);
		if (text.isEmpty() && endOffset > startOffset)
			le->del();
		else
			le->insert(text);
	} else if (QTextEdit *te = textEdit()) {
		if (te->isReadOnly())
			return;
		QTextCursor c(te->document());
		c.setPosition(startOffset);
		c.setPosition(endOffset, QTextCursor::KeepAnchor);
		c.insertText(text);
		te->setTextCursor(c);
	}
}

// ---- LineAccessibleDelegate ----------------------------------------------------------------------

LineAccessibleDelegate::LineAccessibleDelegate(QWidget *w)
	: LineAccessibleWidget(w, QAccessible::ListItem)
{
}

QString LineAccessibleDelegate::computeName() const
{
	QWidget *w = widget();
	if (QString name = explicitAccessibleName(w); !name.isEmpty())
		return name;
	if (QString texts = collectVisibleChildTexts(w); !texts.isEmpty())
		return texts;
	return toolTipText();
}

QString LineAccessibleDelegate::computeDescription() const
{
	return QString();
}

QAccessible::State LineAccessibleDelegate::state() const
{
	QAccessible::State st = LineAccessibleWidget::state();
	st.selectable = true;
	st.selected = firstTrueBoolProp(widget(), { "selected", "isSelected", "current", "isCurrent", "highlighted" });
	return st;
}

// ---- LineAccessibleWindow ------------------------------------------------------------------------

LineAccessibleWindow::LineAccessibleWindow(QWidget *w)
	: QAccessibleWidget(w, QAccessible::Window)
{
}

QString LineAccessibleWindow::text(QAccessible::Text t) const
{
	if (t != QAccessible::Name)
		return QAccessibleWidget::text(t);
	const QString title = cleanText(widget()->windowTitle());
	if (!title.isEmpty() && title != QLatin1String("LINE"))
		return title;
	for (QWidget *child : widget()->findChildren<QWidget *>()) {
		if (inheritsClass(child, "LcTitleBar")) {
			if (const QString text = collectVisibleChildTexts(child, QStringLiteral(" ")); !text.isEmpty())
				return text;
			// SettingWindow: the title bar (or the window, titleVisible) paints the title itself.
			if (const QString text = paintedText(child); !text.isEmpty())
				return text;
			for (QWidget *part : child->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
				if (const QString text = paintedText(part); !text.isEmpty())
					return text;
			}
			if (const QString text = paintedText(widget()); !text.isEmpty())
				return text;
			break;
		}
	}
	// Windows without any title text.
	if (inheritsClass(widget(), "SettingWindow"))
		return label("Settings");
	// A heading right below the title bar (Albums / Notes: the chat's name).
	QWidget *heading = nullptr;
	int headingTop = 100;
	for (QWidget *child : widget()->findChildren<QWidget *>()) {
		if (!inheritsClass(child, "LcText") || !child->isVisible() || ancestorInheriting(child, "LcTitleBar"))
			continue;
		if (const int top = child->mapTo(widget(), QPoint()).y(); top < headingTop) {
			heading = child;
			headingTop = top;
		}
	}
	if (heading) {
		if (const QString text = extractLcTextString(heading); !text.isEmpty())
			return text;
	}
	return title.isEmpty() ? QStringLiteral("LINE") : title;
}

// ---- LineAccessibleScrollArea --------------------------------------------------------------------

LineAccessibleScrollArea::LineAccessibleScrollArea(QWidget *w)
	: QAccessibleWidget(w, QAccessible::Client)
{
}

// ---- LineAccessibleQuickWidget --------------------------------------------------------------------

LineAccessibleQuickWidget::LineAccessibleQuickWidget(QWidget *w)
	: QAccessibleWidget(w, QAccessible::Graphic)
{
}

QString LineAccessibleQuickWidget::text(QAccessible::Text t) const
{
	if (t != QAccessible::Name)
		return QAccessibleWidget::text(t);
	if (QString name = cleanText(widget()->accessibleName()); !name.isEmpty())
		return name;
	// The viewer stacks an image panel and a video panel; the one shown says what this is.
	const QWidget *window = widget()->window();
	if (!inheritsClass(window, "MediaViewerWindow"))
		return QString();
	for (const QWidget *panel : window->findChildren<QWidget *>()) {
		if (inheritsClass(panel, "ImageViewerPanel"))
			return label(panel->isVisible() ? "Photo" : "Video");
	}
	return label("Video");
}

// ---- LineAccessibleMenu --------------------------------------------------------------------------

LineAccessibleMenu::LineAccessibleMenu(QWidget *w)
	: LineAccessibleWidget(w, roleFor(w))
{
}

QAccessible::Role LineAccessibleMenu::roleFor(const QWidget *w)
{
	if (inheritsClass(w, "LcContextMenuSeperator"))
		return QAccessible::Separator;
	if (inheritsAnyClass(w, { "LcContextMenuItem", "LcCheckableContextMenuItem", "LcCheckboxContextMenuItem" }))
		return QAccessible::MenuItem;
	return QAccessible::PopupMenu;
}

QAccessible::State LineAccessibleMenu::state() const
{
	QAccessible::State st = LineAccessibleWidget::state();
	if (inheritsAnyClass(widget(), { "LcCheckableContextMenuItem", "LcCheckboxContextMenuItem" })) {
		st.checkable = true;
		st.checked = firstTrueBoolProp(widget(), { "checked", "isChecked", "selected" });
	}
	return st;
}

QStringList LineAccessibleMenu::actionNames() const
{
	QStringList actions;
	if (isPressable())
		actions << pressAction();
	return withBaseActions(actions, QAccessibleWidget::actionNames());
}

QString LineAccessibleMenu::computeName() const
{
	QWidget *w = widget();
	if (role() == QAccessible::Separator)
		return QString();
	if (QString name = explicitAccessibleName(w); !name.isEmpty())
		return name;
	if (QString text = stringProp(w, "text"); !text.isEmpty())
		return text;
	if (role() == QAccessible::MenuItem) {
		if (QString texts = collectVisibleChildTexts(w, QStringLiteral(" ")); !texts.isEmpty())
			return texts;
		return paintedText(w);
	}
	if (QString title = cleanText(w->windowTitle()); !title.isEmpty())
		return title;
	return label("Context menu");
}

bool LineAccessibleMenu::isPressable() const
{
	return role() == QAccessible::MenuItem && widget()->isEnabled();
}


void LineAccessibleMenu::press()
{
	QPointer<QWidget> guard(widget());
	QMetaObject::invokeMethod(widget(), [guard] {
		if (!guard || !guard->isEnabled() || !guard->isVisible())
			return;
		// Context menu items are clicked like with the mouse: emitting clicked() runs only some
		// commands (Reply did; Unsend, Delete, Translate did nothing). If the menu is still open
		// afterwards the click did nothing either: fall back to the signal below.
		if (QPointer<QWidget> menu(ancestorInheriting(guard, "LcContextMenu")); menu) {
			noteMenuPress(guard, "mouse click");
			sendMouseClick(guard);
			QTimer::singleShot(150, menu, [guard, menu] {
				if (!menu || !menu->isVisible())
					return;
				for (const char *signal : { "clicked()", "triggered()" }) {
					if (guard && isSignalConnected(guard, signal)) {
						noteMenuPress(guard, signal);
						emitSignal(guard, signal);
						break;
					}
				}
				QTimer::singleShot(100, menu, [menu] {
					if (menu && menu->isVisible())
						menu->hide();
				});
			});
			return;
		}
		// An item LINE connected a signal to is pressed through it; otherwise it is clicked.
		for (const char *signal : { "clicked()", "triggered()" }) {
			if (isSignalConnected(guard, signal)) {
				// clicked() runs the command (e.g. "Reply" opens the reply bar and focuses the message
				// input), but LINE closes the menu only on a real mouse click: close it here, or it
				// stays the active window and swallows what the user types.
				QPointer<QWidget> menu(ancestorInheriting(guard, "LcContextMenu"));
				if (menu)
					noteMenuPress(guard, signal);
				emitSignal(guard, signal);
				QTimer::singleShot(100, menu, [menu] {
					if (menu && menu->isVisible())
						menu->hide();
				});
				return;
			}
		}
		if (ancestorInheriting(guard, "LcContextMenu"))
			noteMenuPress(guard, "click");
		const QPointF local = QRectF(guard->rect()).center();
		const QPointF scene = guard->mapTo(guard->window(), local);
		const QPointF global = guard->mapToGlobal(local);
		QMouseEvent press(QEvent::MouseButtonPress, local, scene, global, Qt::LeftButton, Qt::LeftButton,
		                  Qt::NoModifier);
		QCoreApplication::sendEvent(guard, &press);
		if (!guard)
			return;
		QMouseEvent release(QEvent::MouseButtonRelease, local, scene, global, Qt::LeftButton, Qt::NoButton,
		                    Qt::NoModifier);
		QCoreApplication::sendEvent(guard, &release);
	}, Qt::QueuedConnection);
}

} // namespace line_a11y

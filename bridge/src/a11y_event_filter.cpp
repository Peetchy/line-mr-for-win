#include "a11y_event_filter.h"

#include "a11y_log.h"
#include "accessible_list.h"
#include "accessible_widgets.h"
#include "line_introspect.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QKeyEvent>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/qpa/qplatformaccessibility.h>
#include <QtGui/qpa/qplatformintegration.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace line_a11y {
namespace {

constexpr int kWatchdogIntervalMs = 1000;

// Marks widgets whose focus policy the bridge changed (so it only ever touches those).
constexpr const char *kMadeFocusableProperty = "_lineA11yFocusable";

// Controls LINE only lets the mouse reach: buttons, checkboxes, clickable images and texts.
bool wantsKeyboardFocus(QWidget *w)
{
	if (w->isWindow() || w->focusPolicy() != Qt::NoFocus)
		return false;
	if (classifyWidget(w) == WidgetKind::Button || classifyWidget(w) == WidgetKind::List)
		return true;
	if (classifyWidget(w) == WidgetKind::Text) {
		QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(w);
		QAccessibleActionInterface *actions = iface ? iface->actionInterface() : nullptr;
		return actions && actions->actionNames().contains(QAccessibleActionInterface::pressAction());
	}
	return false;
}

bool makeFocusable(QWidget *w)
{
	if (w->property(kMadeFocusableProperty).toBool() || !wantsKeyboardFocus(w))
		return false;
	// TabFocus only: clicking does not move focus, so mouse users see no difference.
	w->setFocusPolicy(Qt::TabFocus);
	w->setProperty(kMadeFocusableProperty, true);
	return true;
}

void collectTabStops(QWidget *w, QList<QWidget *> &content, QList<QWidget *> &titleBar, bool inTitleBar)
{
	inTitleBar = inTitleBar || inheritsClass(w, "LcTitleBar");
	for (QObject *child : w->children()) {
		if (!child->isWidgetType())
			continue;
		auto *c = static_cast<QWidget *>(child);
		if (c->isWindow())
			continue;
		if (c->focusPolicy() & Qt::TabFocus)
			(inTitleBar ? titleBar : content) << c;
		// A text field's own children (clear / show-password buttons) follow the field.
		collectTabStops(c, content, titleBar, inTitleBar);
	}
}

// Qt's focus chain follows widget creation order, which in LINE puts title-bar buttons in the
// middle of a window. Order Tab stops like the widget tree (LINE's panels are built in reading
// order), with the window's title bar last.
void orderTabStops(QWidget *window)
{
	QList<QWidget *> content, titleBar;
	collectTabStops(window, content, titleBar, false);
	const QList<QWidget *> order = content + titleBar;
	for (qsizetype i = 1; i < order.size(); ++i)
		QWidget::setTabOrder(order[i - 1], order[i]);
}

// Makes the mouse-only controls in `root` focusable; re-orders the window's Tab stops when
// anything changed.
void makeFocusableIn(QWidget *root)
{
	bool changed = makeFocusable(root);
	for (QWidget *w : root->findChildren<QWidget *>())
		changed = makeFocusable(w) || changed;
	if (changed && root->window())
		orderTabStops(root->window());
}

// Stable, language-independent tags for the NVDA add-on's commands. Qt's UIA AutomationId is the
// widget's objectName (LINE leaves these empty), e.g. "lineMR_gnb-chat", "lineMR_list_messages".
QString tagFor(QWidget *w)
{
	if (isLineListView(w)) {
		if (ancestorInheriting(w, "ChatMessageView"))
			return QStringLiteral("list_messages");
		if (ancestorInheriting(w, "MainChatPanel"))
			return QStringLiteral("list_chats");
		if (ancestorInheriting(w, "MainFriendPanel"))
			return QStringLiteral("list_friends");
		return QString();
	}
	if (isMessageInput(w))
		return QStringLiteral("input_message");
	if (inheritsClass(w, "LcButton")) {
		QString icon = iconSource(w);
		if (const qsizetype slash = icon.lastIndexOf(u'/'); slash >= 0)
			icon = icon.mid(slash + 1);
		if (const qsizetype dot = icon.lastIndexOf(u'.'); dot > 0)
			icon.truncate(dot);
		if (icon.startsWith(QLatin1String("gnb")) || icon.startsWith(QLatin1String("chatroom-")))
			return icon;
	}
	return QString();
}

void tagWidget(QWidget *w)
{
	if (!w->objectName().isEmpty())
		return;
	if (const QString tag = tagFor(w); !tag.isEmpty())
		w->setObjectName(QStringLiteral("lineMR_") + tag);
}

void tagWidgetsIn(QWidget *root)
{
	tagWidget(root);
	for (QWidget *w : root->findChildren<QWidget *>())
		tagWidget(w);
}

bool isActivationKey(const QKeyEvent *e)
{
	if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
		return false;
	return e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter;
}

// First Tab stop of `window`'s content (title bar excluded) that can take focus now.
QWidget *firstContentTabStop(QWidget *window)
{
	QList<QWidget *> content, titleBar;
	collectTabStops(window, content, titleBar, false);
	for (QWidget *w : content) {
		if (w->isVisible() && w->isEnabled())
			return w;
	}
	return nullptr;
}

void pressWithKeyboard(QWidget *w)
{
	QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(w);
	QAccessibleActionInterface *actions = iface ? iface->actionInterface() : nullptr;
	if (!actions)
		return;
	actions->doAction(QAccessibleActionInterface::pressAction());
	// When the press switches the view (e.g. "Log in with smartphone >" swaps the login panel),
	// the pressed control disappears and Qt hands focus to whatever follows it in the chain.
	// Start at the top of the new view instead.
	QPointer<QWidget> pressed(w);
	QPointer<QWidget> window(w->window());
	QTimer::singleShot(200, window, [pressed, window] {
		if (!window || !window->isActiveWindow() || (pressed && pressed->isVisible()))
			return;
		if (QWidget *first = firstContentTabStop(window))
			first->setFocus(Qt::TabFocusReason);
	});
}

bool isListNavigationKey(int key)
{
	switch (key) {
	case Qt::Key_Up:
	case Qt::Key_Down:
	case Qt::Key_PageUp:
	case Qt::Key_PageDown:
	case Qt::Key_Home:
	case Qt::Key_End:
		return true;
	default:
		return false;
	}
}

// The message input of the chat shown in `window`, if any.
QWidget *messageInputIn(QWidget *window)
{
	for (QWidget *w : window->findChildren<QWidget *>()) {
		if (isMessageInput(w) && w->isVisible())
			return w;
	}
	return nullptr;
}

// Inside the chat shown on the right: its message view, header or message input (which LINE
// places outside ChatMessagePanel in group chats).
bool isInOpenChat(const QWidget *w)
{
	return isMessageInput(w) || inheritsAnyClass(w, { "ChatMessageView", "ChatMessagePanel" })
	    || ancestorInheriting(w, "ChatMessagePanel") || ancestorInheriting(w, "ChatMessageView")
	    || ancestorInheriting(w, "BaseChatPanel");
}

class A11yEventFilter;
A11yEventFilter *g_filter = nullptr;

class A11yEventFilter : public QObject {
public:
	void endBrowsing() { m_browsingList = nullptr; }
	bool browsing(const QWidget *list) const
	{
		return list && m_browsingList.data() == list && m_listKeyTimer.isValid() && m_listKeyTimer.elapsed() < 5000;
	}

	// LINE is pulling focus into the open chat while the user browses a list (the bridge sends
	// it back right away): such focus changes are not announced.
	bool suppressFocusOn(const QWidget *w) const
	{
		return m_browsingList && w != m_browsingList.data() && m_listKeyTimer.isValid()
		    && m_listKeyTimer.elapsed() < 5000 && isInOpenChat(w);
	}

	explicit A11yEventFilter(QObject *parent)
		: QObject(parent)
	{
		auto *watchdog = new QTimer(this);
		connect(watchdog, &QTimer::timeout, this, [this] { watchdogTick(); });
		watchdog->start(kWatchdogIntervalMs);
		// LINE installs its no-op handler during start-up; catch it quickly.
		for (int delay : { 0, 100, 250, 500, 2000 })
			QTimer::singleShot(delay, this, [] { enforceAccessibilityHandler("startup"); });
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		switch (event->type()) {
		case QEvent::WindowActivate:
			enforceAccessibilityHandler("WindowActivate");
			break;
		case QEvent::FocusIn:
			enforceAccessibilityHandler("FocusIn");
			break;
		default:
			break;
		}
		if (!watched->isWidgetType())
			return false;
		auto *w = static_cast<QWidget *>(watched);
		switch (event->type()) {
		case QEvent::Polish:
			repairCachedInterface(w);
			break;
		case QEvent::Show:
			repairCachedInterface(w);
			makeFocusableIn(w);
			tagWidgetsIn(w);
			break;
		case QEvent::FocusIn:
			if (isLineListView(w) && ancestorInheriting(w, "ChatMessageView")) {
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w)))
					list->focusLastVisibleRow();
				// LINE hands focus on from the message list to the message input; reading
				// messages needs it to stay (Tab still moves on).
				m_browsingList = w;
				m_listKeyTimer.start();
			}
			// Arrowing through the chat list opens each chat, and once it has loaded (up to a few
			// seconds later) LINE moves focus into the chat: keep the user in the list instead.
			if (m_browsingList && m_browsingList->isVisible() && w != m_browsingList.data() && isInOpenChat(w)
			    && m_listKeyTimer.isValid() && m_listKeyTimer.elapsed() < 5000) {
				static int logged = 0;
				if (logged++ < 10)
					a11yLog("[keyboard] LINE moved focus to %s while browsing; back to the list",
					        w->metaObject()->className());
				QPointer<QWidget> list = m_browsingList;
				QTimer::singleShot(0, this, [list] {
					if (list && list->isVisible())
						list->setFocus(Qt::OtherFocusReason);
				});
			}
			break;
		case QEvent::MouseButtonPress:
			m_browsingList = nullptr; // the user went somewhere on purpose
			break;
		case QEvent::KeyPress: {
			auto *key = static_cast<QKeyEvent *>(event);
			if (!isLineListView(w) || !isListNavigationKey(key->key()))
				m_browsingList = nullptr;
			if (isLineListView(w) && isListNavigationKey(key->key())) {
				static int logged = 0;
				if (logged++ < 3)
					a11yLog("[keyboard] navigation key in %s", qPrintable(w->objectName()));
			}
			if (isLineListView(w) && ancestorInheriting(w, "ChatMessageView") && isListNavigationKey(key->key())) {
				m_browsingList = w;
				m_listKeyTimer.start();
			}
			if (isLineListView(w) && ancestorInheriting(w, "MainChatPanel")) {
				if (isListNavigationKey(key->key())) {
					m_browsingList = w;
					m_listKeyTimer.start();
				} else if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
				           && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
					// Enter: go to the open chat (opened by the selection) and start typing.
					if (QWidget *input = messageInputIn(w->window())) {
						input->setFocus(Qt::OtherFocusReason);
						return true;
					}
				}
			}
			if (w->property(kMadeFocusableProperty).toBool() && w->hasFocus() && isActivationKey(key)) {
				if (!key->isAutoRepeat())
					pressWithKeyboard(w);
				return true;
			}
			break;
		}
		default:
			break;
		}
		return false;
	}

private:
	void watchdogTick()
	{
		++m_ticks;
		// Qt calls the most recently installed application filter first, and LINE installs its
		// own filters (which consume keys such as the arrows in the chat list): stay in front.
		if (QCoreApplication *app = QCoreApplication::instance()) {
			app->removeEventFilter(this);
			app->installEventFilter(this);
		}
		enforceAccessibilityHandler("watchdog");
		// Keep the bridge factory last in Qt's list (factories are consulted last-first).
		QAccessible::removeFactory(lineAccessibleFactory);
		QAccessible::installFactory(lineAccessibleFactory);
		// Widgets LINE shows without a Show event reaching us (re-parented, re-laid out).
		if (m_ticks % 3 == 0) {
			for (QWidget *top : QApplication::topLevelWidgets()) {
				if (top->isVisible()) {
					makeFocusableIn(top);
					tagWidgetsIn(top);
				}
			}
		}
	}

	quint64 m_ticks = 0;
	QPointer<QWidget> m_browsingList;
	QElapsedTimer m_listKeyTimer;
};

} // namespace

namespace {

// Forwards to Qt's own handling (QPlatformAccessibility, i.e. UI Automation), except focus
// changes LINE makes while the user browses a list (see A11yEventFilter::suppressFocusOn()).
void bridgeUpdateHandler(QAccessibleEvent *event)
{
	if (event->type() == QAccessible::Focus && g_filter) {
		QObject *o = event->object();
		if (o && o->isWidgetType() && g_filter->suppressFocusOn(static_cast<QWidget *>(o)))
			return;
	}
	if (QPlatformIntegration *pi = QGuiApplicationPrivate::platformIntegration()) {
		if (QPlatformAccessibility *pa = pi->accessibility())
			pa->notifyAccessibilityUpdate(event);
	}
}

} // namespace

void enforceAccessibilityHandler(const char *reason)
{
	// Replaces LINE's no-op handler (which swallows every event) with the bridge's forwarder.
	const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(bridgeUpdateHandler);
	if (previous && previous != bridgeUpdateHandler) {
		static int logged = 0;
		if (logged++ < 20)
			a11yLog("[handler] removed foreign QAccessible update handler %p (%s)",
			        reinterpret_cast<void *>(previous), reason);
	}
	QAccessible::setActive(true);
}

void installEventFilter(QApplication *app)
{
	g_filter = new A11yEventFilter(app);
	app->installEventFilter(g_filter);
}

bool isBrowsingList(const QWidget *list)
{
	return g_filter && g_filter->browsing(list);
}

void endListBrowsing()
{
	if (g_filter)
		g_filter->endBrowsing();
}

} // namespace line_a11y

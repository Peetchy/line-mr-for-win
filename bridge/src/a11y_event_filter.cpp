#include "a11y_event_filter.h"

#include "a11y_announce.h"
#include "a11y_diagnostics.h"
#include "a11y_log.h"
#include "accessible_list.h"
#include "accessible_quick.h"
#include "accessible_widgets.h"
#include "full_text_view.h"
#include "line_introspect.h"
#include "qt_signal_utils.h"
#include "text_capture.h"

#include <algorithm>
#include <functional>
#include <memory>

#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QAccessible>
#include <QtGui/QKeyEvent>
#include <QtGui/QTextBlock>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/qpa/qplatformaccessibility.h>
#include <QtGui/qpa/qplatformintegration.h>
#include <QtQuick/QQuickWindow>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QWidget>

#include <windows.h>

namespace line_a11y {
namespace {

constexpr int kWatchdogIntervalMs = 1000;

// See setMenuRetry(): another try for the menu of a message, for a short while.
std::function<void()> g_menuRetry;
QElapsedTimer g_menuRetrySet;
int g_menusShown = 0;

// LINE's photo / video viewer runs in another process (LineMediaPlayer.exe). When it closes, Windows
// activates whatever window comes next, often another application, not the LINE window it was
// opened from: the user lost their place in the chat. The viewer's bridge remembers LINE's window in
// front when a viewer window appears and brings it back when that window hides.
HWND g_returnWindow = nullptr;

bool isLineWindow(HWND hwnd)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid || pid == GetCurrentProcessId())
		return false;
	const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return false;
	wchar_t buf[MAX_PATH];
	DWORD n = MAX_PATH;
	const bool ok = QueryFullProcessImageNameW(process, 0, buf, &n);
	CloseHandle(process);
	return ok && QString::fromWCharArray(buf, int(n)).endsWith(QLatin1StringView("\\LINE.exe"), Qt::CaseInsensitive);
}

// The chat and friend lists, where the bridge keeps its own cursor (see LineAccessibleList::moveCursor).
bool isCursorList(const QWidget *w)
{
	return isLineListView(w) && !ancestorInheriting(w, "LcListView")
	    && (ancestorInheriting(w, "MainChatPanel") || ancestorInheriting(w, "MainFriendPanel"));
}

bool isViewerWindow(const QWidget *w)
{
	return helperProcessName() == QLatin1StringView("linemediaplayer") && w->isWindow()
	    && (w->windowType() == Qt::Window || w->windowType() == Qt::Dialog);
}

void viewerShown(QWidget *w)
{
	const HWND front = GetForegroundWindow();
	if (front && isLineWindow(front)) {
		g_returnWindow = front;
		a11yLog("[viewer] %s shown over LINE window %p", w->metaObject()->className(), static_cast<void *>(front));
	}
}

void viewerHidden(QWidget *w)
{
	const HWND back = g_returnWindow;
	g_returnWindow = nullptr;
	if (!back || !IsWindow(back) || !IsWindowVisible(back))
		return;
	// Windows activates the next window once this one is hidden: give that a moment, then go back
	// to LINE unless the user is already there.
	QTimer::singleShot(100, [back, name = QByteArray(w->metaObject()->className())] {
		const HWND front = GetForegroundWindow();
		if (front == back || (front && isLineWindow(front)))
			return;
		const bool ok = SetForegroundWindow(back);
		a11yLog("[viewer] %s hidden: back to LINE window %p (%s)", name.constData(), static_cast<void *>(back),
		        ok ? "done" : "refused");
	});
}

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

// The same control twice: LINE nests a button in a button (the chat list's show / hide arrow) or lays
// equal buttons over each other (the chat list's ad). Only the outer / first one becomes a Tab stop.
bool duplicatesTabStop(QWidget *w)
{
	auto nameOf = [](QWidget *o) {
		QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(o);
		return iface ? iface->text(QAccessible::Name) : QString();
	};
	const QString name = nameOf(w);
	if (name.isEmpty())
		return false;
	for (QWidget *p = w->parentWidget(); p && !p->isWindow(); p = p->parentWidget()) {
		if (classifyWidget(p) == WidgetKind::Button && nameOf(p) == name)
			return true;
	}
	QWidget *parent = w->parentWidget();
	if (!parent)
		return false;
	for (QObject *o : parent->children()) {
		if (o == w)
			break;
		auto *sibling = qobject_cast<QWidget *>(o);
		if (sibling && !sibling->isWindow() && sibling->geometry() == w->geometry()
		    && (sibling->property(kMadeFocusableProperty).toBool() || wantsKeyboardFocus(sibling)) && nameOf(sibling) == name)
			return true;
	}
	return false;
}

bool makeFocusable(QWidget *w)
{
	if (w->property(kMadeFocusableProperty).toBool() || !wantsKeyboardFocus(w) || duplicatesTabStop(w))
		return false;
	// TabFocus only: clicking does not move focus, so mouse users see no difference.
	w->setFocusPolicy(Qt::TabFocus);
	w->setProperty(kMadeFocusableProperty, true);
	return true;
}

// `w`'s child widgets in reading order. A splitter's panes are in display order, which is not their
// creation order: the open chat's splitter creates the message input before the message list above it.
QList<QWidget *> childWidgetsInOrder(QWidget *w)
{
	QList<QWidget *> children;
	if (auto *splitter = qobject_cast<QSplitter *>(w)) {
		for (int i = 0; i < splitter->count(); ++i)
			children << splitter->widget(i);
	}
	for (QObject *child : w->children()) {
		if (child->isWidgetType() && !children.contains(static_cast<QWidget *>(child)))
			children << static_cast<QWidget *>(child);
	}
	return children;
}

void collectTabStops(QWidget *w, QList<QWidget *> &content, QList<QWidget *> &titleBar, bool inTitleBar)
{
	inTitleBar = inTitleBar || inheritsClass(w, "LcTitleBar");
	for (QWidget *c : childWidgetsInOrder(w)) {
		if (c->isWindow())
			continue;
		QList<QWidget *> &stops = inTitleBar ? titleBar : content;
		const qsizetype first = stops.size();
		if (c->focusPolicy() & Qt::TabFocus)
			stops << c;
		// A text field's own children (clear / show-password buttons) follow the field.
		collectTabStops(c, content, titleBar, inTitleBar);
		// The message input bar: from the message input (reached from the message list above it)
		// out through its buttons, nearest first (stickers ... attach file).
		if (inheritsClass(c, "MessageInputPanel"))
			std::reverse(stops.begin() + first, stops.end());
		// The open chat's note banner and the buttons over the messages come before the message
		// list, which leads on to the message input (user request).
		if (inheritsClass(c, "ChatMessageView"))
			std::stable_partition(stops.begin() + first, stops.end(), [](QWidget *stop) {
				return !isLineListView(stop) || ancestorInheriting(stop, "LcListView");
			});
		// The chat list panel: its buttons (sort, show / hide, create chat, the ad) first, then the
		// search field and the chat list, which leads on to the open chat (user request).
		if (inheritsClass(c, "MainChatPanel")) {
			auto rank = [](QWidget *stop) {
				if (isLineListView(stop) && !ancestorInheriting(stop, "LcListView"))
					return 2;
				return inheritsClass(stop, "QLineEdit") || ancestorInheriting(stop, "QLineEdit") ? 1 : 0;
			};
			std::stable_sort(stops.begin() + first, stops.end(),
			                 [&rank](QWidget *a, QWidget *b) { return rank(a) < rank(b); });
		}
	}
}

// Qt's focus chain follows widget creation order, which in LINE puts title-bar buttons in the
// middle of a window. Order Tab stops like the widget tree (LINE's panels are built in reading
// order), with the window's title bar last.
void orderTabStops(QWidget *window)
{
	QList<QWidget *> content, titleBar;
	collectTabStops(window, content, titleBar, false);
	// The chat folder tabs (All / Friends / Groups / ...) sit in the title bar but filter the chat
	// list: they go right before the chat list's search field (user request).
	const auto search = std::find_if(content.begin(), content.end(), [](QWidget *stop) {
		return inheritsClass(stop, "QLineEdit") && ancestorInheriting(stop, "MainChatPanel");
	});
	if (search != content.end()) {
		QList<QWidget *> folders;
		titleBar.removeIf([&folders](QWidget *stop) {
			if (!ancestorInheriting(stop, "TabBar"))
				return false;
			// The scroll area holding the tabs would be a nameless stop before them.
			if (inheritsClass(stop, "LcScrollArea"))
				stop->setFocusPolicy(Qt::NoFocus);
			else
				folders << stop;
			return true;
		});
		const qsizetype at = search - content.begin();
		for (qsizetype i = 0; i < folders.size(); ++i)
			content.insert(at + i, folders.at(i));
	}
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
		// Lists inside a row (photo grids in a message) are part of that row.
		if (ancestorInheriting(w, "LcListView"))
			return QString();
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
	// The search boxes above the chat and friend lists (the add-on's Alt+F).
	if (qobject_cast<const QLineEdit *>(w) && inheritsClass(w, "LcTextField")) {
		if (ancestorInheriting(w, "MainChatPanel"))
			return QStringLiteral("search_chats");
		if (ancestorInheriting(w, "MainFriendPanel"))
			return QStringLiteral("search_friends");
	}
	if (inheritsClass(w, "LcButton")) {
		QString icon = iconSource(w);
		if (const qsizetype slash = icon.lastIndexOf(u'/'); slash >= 0)
			icon = icon.mid(slash + 1);
		if (const qsizetype dot = icon.lastIndexOf(u'.'); dot > 0)
			icon.truncate(dot);
		if (icon.startsWith(QLatin1String("gnb")) || icon.startsWith(QLatin1String("chatroom-")))
			return icon;
		// Call windows (VoiceCallWindow): the add-on's answer / hang-up commands press these.
		if (icon == QLatin1String("call_end_bt") || icon == QLatin1String("call-end-icon") || icon == QLatin1String("hangup"))
			return QStringLiteral("call-end");
		if (icon == QLatin1String("btn_call_answer"))
			return QStringLiteral("call-answer");
	}
	return QString();
}

// LINE's combo boxes (Settings: language, font, lock delay) have no name; their label is the
// LcText right above them.
void labelComboBox(QWidget *box)
{
	if (!inheritsClass(box, "QComboBox") || !box->accessibleName().isEmpty() || !box->isVisible())
		return;
	if (const QString text = labelAboveText(box); !text.isEmpty())
		box->setAccessibleName(text);
	else if (const QString text = nearbyLabelText(box); !text.isEmpty())
		box->setAccessibleName(text);
}

void tagWidget(QWidget *w)
{
	labelComboBox(w);
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

// ---- Pages opened by a press ---------------------------------------------------------------------
//
// A press that opens a panel inside the window (profile, settings page, search) leaves focus on the
// pressed control, so NVDA says nothing about the new page. Panels shown shortly after a press are
// remembered, and focus moves to the first control of the outermost one.

QPointer<QWidget> g_pressed;
QElapsedTimer g_pressTimer;
QPointer<QWidget> g_newContent;

bool pressIsRecent()
{
	return g_pressed && g_pressTimer.isValid() && g_pressTimer.elapsed() < 1500;
}

// The last context menu item press, and the window it opened.
QElapsedTimer g_menuPressTimer;
QPointer<QWidget> g_windowAfterMenu;

bool menuPressIsRecent()
{
	return g_menuPressTimer.isValid() && g_menuPressTimer.elapsed() < 3000;
}

void noteShownAfterPress(QWidget *w)
{
	QWidget *pressed = g_pressed;
	if (w->isWindow() || w->window() != pressed->window() || w == pressed || w->isAncestorOf(pressed)
	    || pressed->isAncestorOf(w) || w->width() < 80 || w->height() < 40 || ancestorInheriting(w, "LcTitleBar"))
		return;
	if (g_newContent && (g_newContent == w || g_newContent->isAncestorOf(w)))
		return;
	if (!g_newContent || w->isAncestorOf(g_newContent))
		g_newContent = w;
}

void moveFocusToNewContent()
{
	QWidget *content = g_newContent;
	g_newContent = nullptr;
	if (!content || !content->isVisible() || !content->window()->isActiveWindow())
		return;
	QWidget *focus = QApplication::focusWidget();
	if (focus && focus->isVisible() && focus != g_pressed.data() && !focus->isAncestorOf(content))
		return; // LINE (or the user) already put focus somewhere on purpose
	if (focus && content->isAncestorOf(focus))
		return;
	QList<QWidget *> stops, titleBar;
	collectTabStops(content, stops, titleBar, false);
	// A page built around a list (friends, chats, search results) starts in the list.
	std::stable_partition(stops.begin(), stops.end(), [](QWidget *stop) { return isLineListView(stop); });
	for (QWidget *stop : std::as_const(stops) + titleBar) {
		if (stop->isVisible() && stop->isEnabled()) {
			static int logged = 0;
			if (logged++ < 10)
				a11yLog("[keyboard] new %s after a press: focus to %s", content->metaObject()->className(),
				        stop->metaObject()->className());
			stop->setFocus(Qt::TabFocusReason);
			return;
		}
	}
}

void pressWithKeyboard(QWidget *w)
{
	QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(w);
	QAccessibleActionInterface *actions = iface ? iface->actionInterface() : nullptr;
	if (!actions)
		return;
	actions->doAction(QAccessibleActionInterface::pressAction()); // also notes the press
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

// ---- Context menus (LcContextMenu) --------------------------------------------------------------
//
// LINE's context menus (Applications key / right click) are mouse-only: no item takes focus, so
// NVDA hears nothing and the arrows do nothing. The bridge focuses the first item when a menu
// opens; Up/Down/Home/End move between items, Enter/Space press, Escape closes.

bool isContextMenu(const QWidget *w)
{
	return inheritsClass(w, "LcContextMenu");
}

bool isContextMenuItem(const QWidget *w)
{
	return inheritsAnyClass(w, { "LcContextMenuItem", "LcCheckableContextMenuItem", "LcCheckboxContextMenuItem" });
}

QWidget *contextMenuOf(QWidget *w)
{
	if (!w)
		return nullptr;
	return isContextMenu(w) ? w : ancestorInheriting(w, "LcContextMenu");
}

// The menu's usable items, top to bottom (items of a sub-menu belong to that sub-menu).
QList<QWidget *> menuItems(QWidget *menu)
{
	QList<QWidget *> items;
	for (QWidget *w : menu->findChildren<QWidget *>()) {
		if (isContextMenuItem(w) && w->isVisible() && w->isEnabled() && contextMenuOf(w->parentWidget()) == menu)
			items << w;
	}
	std::stable_sort(items.begin(), items.end(), [menu](QWidget *a, QWidget *b) {
		return a->mapTo(menu, QPoint(0, 0)).y() < b->mapTo(menu, QPoint(0, 0)).y();
	});
	return items;
}

void logMenu(QWidget *menu)
{
	static int logged = 0;
	if (logged++ >= 5)
		return;
	QWidget *popup = QApplication::activePopupWidget();
	a11yLog("[menu] %s shown: window=%d flags=0x%x active=%d popup=%s", menu->metaObject()->className(),
	        int(menu->isWindow()), unsigned(menu->window()->windowFlags()), int(menu->window()->isActiveWindow()),
	        popup ? popup->metaObject()->className() : "-");
	for (QWidget *item : menuItems(menu)) {
		QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
		a11yLog("[menu]   %s \"%s\" clicked()=%d triggered()=%d", item->metaObject()->className(),
		        iface ? qUtf8Printable(iface->text(QAccessible::Name)) : "", int(isSignalConnected(item, "clicked()")),
		        int(isSignalConnected(item, "triggered()")));
	}
}

void focusMenuItem(QWidget *item)
{
	if (!item)
		return;
	if (item->focusPolicy() == Qt::NoFocus)
		item->setFocusPolicy(Qt::TabFocus);
	QWidget *window = item->window();
	// A popup gets the keyboard without being activated; other menu windows must be activated.
	if (!window->isActiveWindow() && window->windowType() != Qt::Popup)
		window->activateWindow();
	item->setFocus(Qt::PopupFocusReason);
}

// Moves focus within `menu` by key (Up/Down wrap around, Home/End go to the ends).
void moveInMenu(QWidget *menu, int key)
{
	const QList<QWidget *> items = menuItems(menu);
	if (items.isEmpty())
		return;
	qsizetype i = items.indexOf(QApplication::focusWidget());
	if (key == Qt::Key_Home || (i < 0 && key == Qt::Key_Down))
		i = 0;
	else if (key == Qt::Key_End || (i < 0 && key == Qt::Key_Up))
		i = items.size() - 1;
	else if (key == Qt::Key_Down)
		i = (i + 1) % items.size();
	else if (key == Qt::Key_Up)
		i = (i + items.size() - 1) % items.size();
	focusMenuItem(items[i]);
}

void pressMenuItem(QWidget *item)
{
	QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
	if (QAccessibleActionInterface *actions = iface ? iface->actionInterface() : nullptr)
		actions->doAction(QAccessibleActionInterface::pressAction()); // also closes the menu
}

// Is `menu` the chat background's menu (Select all / Set wallpaper), not a message's? LINE shows it
// when a right click misses the part of a message it reacts to.
bool isChatBackgroundMenu(QWidget *menu)
{
	// "Set wallpaper" in the languages LINE offers.
	static const QStringList kWallpaper = { QStringLiteral("วอลล์เปเปอร์"), QStringLiteral("wallpaper"),
		                                    QStringLiteral("壁紙"), QStringLiteral("壁纸"), QStringLiteral("桌布"),
		                                    QStringLiteral("背景") };
	// Select all / Set wallpaper. A photo's menu has "Use as wallpaper" too, among many other items.
	const QList<QWidget *> items = menu ? menuItems(menu) : QList<QWidget *>();
	if (items.isEmpty() || items.size() > 2)
		return false;
	for (QWidget *item : items) {
		QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
		const QString name = iface ? iface->text(QAccessible::Name) : QString();
		for (const QString &word : kWallpaper) {
			if (name.contains(word, Qt::CaseInsensitive))
				return true;
		}
	}
	return false;
}

// ---- Call windows --------------------------------------------------------------------------------

bool isCallWindow(const QWidget *w)
{
	const QLatin1StringView cls(w->metaObject()->className());
	return cls.contains(QLatin1StringView("Call"), Qt::CaseInsensitive)
	    || cls.contains(QLatin1StringView("Voip"), Qt::CaseInsensitive);
}

void windowShown(QWidget *w)
{
	static QSet<QByteArray> loggedClasses;
	const QByteArray cls = w->metaObject()->className();
	if (!loggedClasses.contains(cls)) {
		loggedClasses.insert(cls);
		a11yLog("[window] first %s shown: flags=0x%x active=%d", cls.constData(), unsigned(w->windowFlags()),
		        int(w->isActiveWindow()));
	}
	// A long message's full text ("See more"): one list row, read in one go. A text view laid over
	// it reads by line like Notepad (full_text_view.h); while LINE keeps painting, try again, then
	// start on the text row (the first row is a blank spacer).
	if (inheritsClass(w, "ChatDetailTextMessageWindow")) {
		QPointer<QWidget> window(w);
		auto attach = std::make_shared<std::function<void(int)>>();
		*attach = [window, attach](int tries) {
			if (!window || !window->isVisible() || attachFullTextView(window))
				return;
			if (tries > 0) {
				QTimer::singleShot(300, window, [attach, tries] { (*attach)(tries - 1); });
				return;
			}
			for (auto *view : window->findChildren<QAbstractItemView *>()) {
				auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(view));
				if (!list || !view->isVisible())
					continue;
				if (!window->isActiveWindow())
					window->activateWindow();
				view->setFocus(Qt::OtherFocusReason);
				list->focusEdgeRow(false);
				break;
			}
		};
		QTimer::singleShot(300, w, [attach] { (*attach)(5); });
	}
	if (!isCallWindow(w))
		return;
	// The call window is checked from this dump (an incoming call cannot be produced on demand).
	QPointer<QWidget> window(w);
	QTimer::singleShot(1500, w, [window] {
		if (!window || !window->isVisible())
			return;
		writeWindowDump(window, QStringLiteral("call-window.txt"));
		// LINE may show the incoming-call window without activating it, so focus never gets there.
		if (!window->isActiveWindow()) {
			const QString text = cleanText(capturedTextInReadingOrder(captureWidgetText(window, true)));
			a11yLog("[call] %s not active; announcing %d chars", window->metaObject()->className(), int(text.size()));
			announce(window, text, true);
		}
	});
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
		if (!watched->isWidgetType()) {
			if (isQuickWindow(watched)) {
				auto *window = static_cast<QQuickWindow *>(watched);
				if (event->type() == QEvent::Show)
					quickWindowShown(window);
				else if (event->type() == QEvent::KeyPress) {
					auto *key = static_cast<QKeyEvent *>(event);
					if (!key->isAutoRepeat() && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
					    && handleQuickKey(window, key->key()))
						return true;
				}
			}
			return false;
		}
		auto *w = static_cast<QWidget *>(watched);
		switch (event->type()) {
		case QEvent::Polish:
			repairCachedInterface(w);
			break;
		case QEvent::Show:
			if (w->isWindow())
				breadcrumb("show window %s", w->metaObject()->className());
			repairCachedInterface(w);
			makeFocusableIn(w);
			tagWidgetsIn(w);
			if (w->isWindow() && !isContextMenu(w) && menuPressIsRecent()) {
				a11yLog("[menu] after the press: %s shown, active=%d, title \"%s\"", w->metaObject()->className(),
				        int(w->isActiveWindow()), qUtf8Printable(w->windowTitle()));
				// A confirmation (AlertWindow, shown inactive): the user has to answer it.
				g_windowAfterMenu = w;
				QPointer<QWidget> window(w);
				QTimer::singleShot(150, w, [window] {
					if (!window || !window->isVisible() || window->isActiveWindow())
						return;
					window->raise();
					window->activateWindow();
				});
			}
			if (w->isWindow())
				windowShown(w);
			else if (pressIsRecent())
				noteShownAfterPress(w);
			if (isViewerWindow(w))
				viewerShown(w);
			if (isContextMenu(w))
				menuShown(w);
			break;
		case QEvent::WindowActivate:
			// A dialog window LINE opens with nothing focused, or (first activation) with focus on a
			// title-bar button such as Close: start at its first control.
			if (w->isWindow() && !isContextMenu(w)) {
				const auto unfocused = [](QWidget *window, bool first) {
					QWidget *f = window->focusWidget();
					return !f || (first && ancestorInheriting(f, "LcTitleBar")
					              && !inheritsAnyClass(window, { "AllInOneWindow", "ChatWindow" }));
				};
				const bool first = !w->property("_lineA11yActivated").toBool();
				w->setProperty("_lineA11yActivated", true);
				if (unfocused(w, first)) {
					QPointer<QWidget> window(w);
					QTimer::singleShot(150, w, [window, first, unfocused] {
						if (window && window->isActiveWindow() && unfocused(window, first))
							if (QWidget *stop = firstContentTabStop(window))
								stop->setFocus(Qt::TabFocusReason);
					});
				}
			}
			break;
		case QEvent::Hide:
			if (w->isWindow())
				breadcrumb("hide window %s", w->metaObject()->className());
			if (isContextMenu(w) && w == m_menu.data())
				menuHidden();
			if (isViewerWindow(w))
				viewerHidden(w);
			break;
		case QEvent::FocusIn:
			// Entering the chat or friend list starts at LINE's current row (unless the bridge is
			// sending focus back while the user browses).
			if (isLineListView(w) && w != m_browsingList.data()
			    && (ancestorInheriting(w, "MainChatPanel") || ancestorInheriting(w, "MainFriendPanel"))) {
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w)))
					list->resetCursor();
			}
			if (isLineListView(w) && ancestorInheriting(w, "ChatMessageView")) {
				// Entering the messages (Tab, Shift+Tab, Alt+M) starts at the newest one; coming back
				// (focus sent back by the bridge, menu closed, window activated, click) keeps the place.
				const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
				const bool returning = (w == m_browsingList.data() && m_listKeyTimer.isValid()
				                        && m_listKeyTimer.elapsed() < 5000)
				    || reason == Qt::PopupFocusReason || reason == Qt::ActiveWindowFocusReason
				    || reason == Qt::MouseFocusReason;
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w))) {
					// Tab / Shift+Tab back into the same chat: the message last read.
					const bool tab = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason;
					if (returning)
						list->focusLastVisibleRow();
					else if (!tab || !list->focusRememberedRow())
						list->focusEdgeRow(true);
				}
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
			// A click opens a chat itself: the chat list cursor follows LINE's current row again.
			if (QWidget *view = isLineListView(w) ? w : w->parentWidget(); view && isLineListView(view)) {
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(view)))
					list->resetCursor();
			}
			break;
		case QEvent::ContextMenu: {
			static int logged = 0;
			if (logged++ < 10)
				a11yLog("[menu] context menu event (reason %d) for %s", int(static_cast<QContextMenuEvent *>(event)->reason()),
				        w->metaObject()->className());
		}
			// Applications key / Shift+F10 on a message: open the message's menu, not the chat
			// background's (see LineAccessibleList::openRowMenu).
			if (static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Keyboard
			    && isLineListView(w) && ancestorInheriting(w, "ChatMessageView")) {
				if (m_cursorMenuKey.isValid() && m_cursorMenuKey.elapsed() < 1000)
					return true; // the Applications key already opened it
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w));
				    list && list->openRowMenu())
					return true;
			}
			// In the chat and friend lists: the menu of the row NVDA read (the bridge's cursor), not
			// of LINE's current row (the open chat). The Applications key already opened it on its
			// key press (below); Shift+F10 arrives only here.
			if (static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Keyboard && isCursorList(w)) {
				if (m_cursorMenuKey.isValid() && m_cursorMenuKey.elapsed() < 1000)
					return true;
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w));
				    list && list->openCursorMenu())
					return true;
			}
			break;
		case QEvent::KeyPress: {
			auto *key = static_cast<QKeyEvent *>(event);
			breadcrumb("key 0x%x in %s", key->key(), w->metaObject()->className());
			if (QWidget *menu = contextMenuOf(w); menu && menu->isVisible() && handleMenuKey(menu, key))
				return true;
			// LINE's own application event filters (after this one) switch to the previous / next
			// chat on Up/Down in the message input: the input gets them directly instead.
			if ((key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)
			    && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && w->hasFocus()
			    && isMessageInput(w)) {
				static int logged = 0;
				if (logged++ < 3)
					a11yLog("[keyboard] key 0x%x in the message input bypasses LINE's filters", key->key());
				static_cast<QObject *>(w)->event(event);
				return true;
			}
			// The full-text view (full_text_view.h): LINE's filters eat the arrows too. Tab and Escape
			// go the usual way (next control, LINE closes the window).
			// Enter there opens a web address on the caret's line (or one wrapped onto it).
			if (w->hasFocus() && w->objectName() == QLatin1StringView("lineMR_fulltext")
			    && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !key->isAutoRepeat()) {
				if (auto *edit = qobject_cast<QPlainTextEdit *>(w)) {
					const QTextBlock line = edit->textCursor().block();
					QStringList lines;
					for (QTextBlock b = line; b.isValid() && lines.size() < 4; b = b.next())
						lines << b.text();
					QString url = firstWebAddress(lines);
					if (url.isEmpty() && line.previous().isValid())
						url = firstWebAddress({ line.previous().text(), line.text() });
					if (!url.isEmpty() && openInBrowser(url)) {
						a11yLog("[fulltext] link (%s) opened in the browser", qUtf8Printable(QUrl(url).host()));
						return true;
					}
				}
			}
			if (w->hasFocus() && w->objectName() == QLatin1StringView("lineMR_fulltext")
			    && key->key() != Qt::Key_Tab && key->key() != Qt::Key_Backtab && key->key() != Qt::Key_Escape) {
				static_cast<QObject *>(w)->event(event);
				return true;
			}
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
				// Home / End: the oldest loaded / the newest message (LINE loads older messages when
				// the top is reached, so Home again goes further back).
				if ((key->key() == Qt::Key_Home || key->key() == Qt::Key_End)
				    && !(key->modifiers() & (Qt::AltModifier | Qt::ShiftModifier))) {
					if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w)))
						list->focusEdgeRow(key->key() == Qt::Key_End);
					return true;
				}
				// The arrows move from the bridge's row too (LINE keeps a position of its own).
				if (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))) {
					if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w));
					    list && list->stepRow(key->key()))
						return true;
				}
			}
			if (isLineListView(w)
			    && (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
			    && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))) {
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w));
				    list && !key->isAutoRepeat()
				    && (ancestorInheriting(w, "ChatMessageView") ? list->openSeeMore() || list->openLink() || list->openMessageContent()
				                                                 : list->toggleCursorRow()))
					return true;
			}
			// The Applications key on a message: its menu (see the context menu event above). Handled on
			// the key press, which also arrives for posted keys.
			if (isLineListView(w) && ancestorInheriting(w, "ChatMessageView") && key->key() == Qt::Key_Menu
			    && !key->isAutoRepeat()) {
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w));
				    list && list->openRowMenu()) {
					m_cursorMenuKey.start();
					return true;
				}
			}
			if (isCursorList(w) && key->key() == Qt::Key_Menu && !key->isAutoRepeat()) {
				if (auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w));
				    list && list->openCursorMenu()) {
					m_cursorMenuKey.start();
					return true;
				}
			}
			// Applications key: LINE opens the menu where the current row is; make sure it is on screen.
			if (isLineListView(w) && key->key() == Qt::Key_Menu) {
				auto *view = static_cast<QAbstractItemView *>(w);
				if (view->currentIndex().isValid())
					view->scrollTo(view->currentIndex());
			}
			// The friend list opens the friend's chat on every step too, and LINE then moves focus
			// into it (a second arrow went on to the title bar): it gets the same cursor.
			const bool friendList = isLineListView(w) && ancestorInheriting(w, "MainFriendPanel");
			if (isLineListView(w) && (friendList || ancestorInheriting(w, "MainChatPanel"))) {
				auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(w));
				const bool plain = !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
				if (isListNavigationKey(key->key())) {
					m_browsingList = w;
					m_listKeyTimer.start();
					// The bridge's cursor moves; LINE's current row (the open chat) stays.
					if (plain && list && list->moveCursor(key->key()))
						return true;
				} else if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
				           && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
					// Friends: click the row and let LINE decide (a section header folds, a friend's
					// chat opens and LINE moves focus there itself).
					if (friendList) {
						if (list && list->openCursorRow()) {
							m_browsingList = nullptr;
							return true;
						}
						break;
					}
					// Enter: open the chat under the cursor, go to it and start typing.
					if (list && list->openCursorRow()) {
						m_browsingList = nullptr;
						QPointer<QWidget> listView = w;
						QTimer::singleShot(0, this, [listView] {
							if (!listView || !listView->hasFocus())
								return;
							if (QWidget *input = messageInputIn(listView->window()))
								input->setFocus(Qt::OtherFocusReason);
						});
						return true;
					}
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
	void menuShown(QWidget *menu)
	{
		breadcrumb("menu shown %s", menu->metaObject()->className());
		++g_menusShown;
		logMenu(menu);
		QWidget *focus = QApplication::focusWidget();
		if (!contextMenuOf(focus))
			m_menuReturnFocus = focus;
		m_menu = menu;
		// LINE fills and lays out the menu right after showing it.
		QPointer<QWidget> guard(menu);
		QTimer::singleShot(50, menu, [guard] {
			if (!guard || !guard->isVisible())
				return;
			// The bridge's right click on a message missed: close the background's menu (which also ends
			// LINE's menu loop) and click the message's next part.
			std::function<void()> retry;
			if (g_menuRetrySet.isValid() && g_menuRetrySet.elapsed() < 1500)
				retry = std::move(g_menuRetry);
			g_menuRetry = nullptr;
			if (retry && isChatBackgroundMenu(guard)) {
				a11yLog("[menu] the chat background's menu: trying another point");
				guard->hide();
				QTimer::singleShot(50, qApp, retry);
				return;
			}
			const QList<QWidget *> items = menuItems(guard);
			if (!items.isEmpty())
				focusMenuItem(items.first());
			static int logged = 0;
			QWidget *now = QApplication::focusWidget();
			if (logged++ < 5)
				a11yLog("[menu] focused first of %d items; focus now %s", int(items.size()),
				        now ? now->metaObject()->className() : "-");
		});
	}

	void menuHidden()
	{
		m_menu = nullptr;
		QPointer<QWidget> back = m_menuReturnFocus;
		m_menuReturnFocus = nullptr;
		// The menu is a window of its own: give the keyboard back to the window it was opened from,
		// to the widget the chosen item focused there (e.g. "Reply" focuses the message input) or
		// else to where the menu was opened.
		QTimer::singleShot(0, this, [back] {
			if (!back || !back->isVisible())
				return;
			// The chosen item opened a window (a confirmation): it keeps the keyboard.
			if (g_windowAfterMenu && g_windowAfterMenu->isVisible() && g_windowAfterMenu->window() != back->window()) {
				a11yLog("[menu] closed; %s keeps the focus", g_windowAfterMenu->metaObject()->className());
				return;
			}
			QWidget *window = back->window();
			QWidget *target = window->focusWidget();
			if (!target || !target->isVisible() || contextMenuOf(target))
				target = back;
			if (!window->isActiveWindow())
				window->activateWindow();
			target->setFocus(Qt::PopupFocusReason);
			static int logged = 0;
			if (logged++ < 5)
				a11yLog("[menu] closed; focus to %s", target->metaObject()->className());
		});
	}

	bool handleMenuKey(QWidget *menu, const QKeyEvent *key)
	{
		if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
			return false;
		switch (key->key()) {
		case Qt::Key_Up:
		case Qt::Key_Down:
		case Qt::Key_Home:
		case Qt::Key_End:
			moveInMenu(menu, key->key());
			return true;
		case Qt::Key_Escape:
			menu->hide();
			return true;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: {
			QWidget *item = QApplication::focusWidget();
			if (!item || !isContextMenuItem(item) || contextMenuOf(item) != menu)
				return false;
			if (!key->isAutoRepeat())
				pressMenuItem(item);
			return true;
		}
		default:
			return false;
		}
	}

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
	// When the Applications key opened a chat / friend list row's menu (its context menu event follows).
	QElapsedTimer m_cursorMenuKey;
	QPointer<QWidget> m_menu;
	QPointer<QWidget> m_menuReturnFocus;
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

void installEventFilter(QCoreApplication *app)
{
	g_filter = new A11yEventFilter(app);
	app->installEventFilter(g_filter);
}

void notePress(QWidget *pressed)
{
	g_pressed = pressed;
	g_pressTimer.start();
	g_newContent = nullptr;
	if (pressed)
		QTimer::singleShot(450, pressed->window(), [] { moveFocusToNewContent(); });
}

void noteMenuPress(QWidget *item, const char *how)
{
	g_menuPressTimer.start();
	g_windowAfterMenu = nullptr;
	QAccessibleInterface *iface = item ? QAccessible::queryAccessibleInterface(item) : nullptr;
	a11yLog("[menu] pressed \"%s\" (%s)", iface ? qUtf8Printable(iface->text(QAccessible::Name)) : "", how);
}

void setMenuRetry(std::function<void()> retry)
{
	g_menuRetry = std::move(retry);
	g_menuRetrySet.start();
}

int contextMenusShown()
{
	return g_menusShown;
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

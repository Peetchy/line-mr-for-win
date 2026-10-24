#include "video_controls.h"

#include "a11y_log.h"
#include "line_introspect.h"
#include "qt_signal_utils.h"

#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QAccessibleObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QWindow>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickRenderControl>
#include <QtQuick/QQuickWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <windows.h>

namespace line_a11y {
namespace {

// The controls, in Tab order, by their QML objectName.
enum Control { PlayButton, PositionSlider, SpeedBox, ControlCount };
constexpr const char *kControlNames[ControlCount] = { "playBtn", "timeSlider", "speedComboBox" };

// Which control the bridge's focus is on, kept on the video widget.
constexpr const char *kFocusProperty = "_lineA11yVideoFocus";
constexpr double kStepSeconds = 5;
constexpr double kPageSeconds = 30;

// Set while the bridge reports a value change itself (the player moves the slider every second).
bool g_reporting = false;

// LINE's viewer takes the arrows (previous / next media) before Qt delivers them while its window is
// active; Space reached the bridge, the arrows never did. A native filter installed after LINE's
// sees the keys first: on the video widget it plays them itself.
class VideoKeyFilter : public QAbstractNativeEventFilter {
public:
	bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
	{
		if (type != "windows_generic_MSG")
			return false;
		const MSG *msg = static_cast<const MSG *>(message);
		if (msg->message != WM_KEYDOWN && msg->message != WM_KEYUP)
			return false;
		int key = 0;
		switch (msg->wParam) {
		case VK_LEFT: key = Qt::Key_Left; break;
		case VK_RIGHT: key = Qt::Key_Right; break;
		case VK_UP: key = Qt::Key_Up; break;
		case VK_DOWN: key = Qt::Key_Down; break;
		case VK_HOME: key = Qt::Key_Home; break;
		case VK_END: key = Qt::Key_End; break;
		case VK_PRIOR: key = Qt::Key_PageUp; break;
		case VK_NEXT: key = Qt::Key_PageDown; break;
		default: return false;
		}
		if ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) | GetKeyState(VK_SHIFT)) & 0x8000)
			return false;
		QWidget *focus = QApplication::focusWidget();
		if (!focus || !inheritsClass(focus, "QQuickWidget") || !showsVideoWidget(focus))
			return false;
		if (msg->message == WM_KEYDOWN) {
			QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(), (msg->lParam & (1 << 30)) != 0);
			static int logged = 0;
			if (logged++ < 5)
				a11yLog("[video] native key 0x%x", unsigned(key));
			handleVideoKey(focus, &press);
		}
		return true;
	}
	static bool showsVideoWidget(const QWidget *w);
};

VideoKeyFilter *g_keyFilter = nullptr;

// The native filter never saw real arrows either (posted ones it did): LINE takes them with a
// low-level keyboard hook while the viewer is in front. Hooks are called newest first, so the
// bridge installs its own (re-installed every second, only while a video shows) and plays the keys.
HHOOK g_keyHook = nullptr;

int qtKeyFor(DWORD vk)
{
	switch (vk) {
	case VK_LEFT: return Qt::Key_Left;
	case VK_RIGHT: return Qt::Key_Right;
	case VK_UP: return Qt::Key_Up;
	case VK_DOWN: return Qt::Key_Down;
	case VK_HOME: return Qt::Key_Home;
	case VK_END: return Qt::Key_End;
	case VK_PRIOR: return Qt::Key_PageUp;
	case VK_NEXT: return Qt::Key_PageDown;
	// LINE's hook takes Space as well (it paused, then the bridge saw a paused video and resumed it).
	case VK_SPACE: return Qt::Key_Space;
	case VK_RETURN: return Qt::Key_Return;
	default: return 0;
	}
}

// Keys held down (the hook sees each repeat as another key down).
QSet<DWORD> g_keysDown;

// The focused video widget while this viewer is the foreground window, else nullptr.
QWidget *focusedVideoInFront()
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	if (pid != GetCurrentProcessId())
		return nullptr;
	QWidget *focus = QApplication::focusWidget();
	return focus && inheritsClass(focus, "QQuickWidget") && VideoKeyFilter::showsVideoWidget(focus) ? focus : nullptr;
}

LRESULT CALLBACK lowLevelKey(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_KEYUP)) {
		const auto *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
		const int key = qtKeyFor(k->vkCode);
		// Modifiers, and NVDA's own key (Insert / Caps Lock) for its commands, pass.
		const bool modifiers = (GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_SHIFT)
		                        | GetAsyncKeyState(VK_INSERT) | GetAsyncKeyState(VK_CAPITAL) | GetAsyncKeyState(VK_LWIN)
		                        | GetAsyncKeyState(VK_RWIN)) & 0x8000;
		if (key && !modifiers) {
			if (QWidget *video = focusedVideoInFront()) {
				const bool repeat = wParam == WM_KEYDOWN && g_keysDown.contains(k->vkCode);
				if (wParam == WM_KEYDOWN)
					g_keysDown.insert(k->vkCode);
				else
					g_keysDown.remove(k->vkCode);
				// Holding Space / Enter must not toggle again and again; held arrows keep seeking.
				if (wParam == WM_KEYDOWN && !(repeat && (key == Qt::Key_Space || key == Qt::Key_Return))) {
					static int logged = 0;
					if (logged++ < 5)
						a11yLog("[video] hook key 0x%x", unsigned(key));
					// Out of the hook (it must return quickly): the seek runs on the next event.
					QPointer<QWidget> guard(video);
					QTimer::singleShot(0, video, [guard, key] {
						if (!guard)
							return;
						QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
						handleVideoKey(guard, &press);
					});
				}
				return 1;
			}
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

QQuickItem *findItem(QQuickItem *root, QLatin1StringView name)
{
	if (!root)
		return nullptr;
	if (root->objectName() == name)
		return root;
	for (QQuickItem *child : root->childItems()) {
		if (QQuickItem *found = findItem(child, name))
			return found;
	}
	return nullptr;
}

// The scene behind a QQuickWidget: its offscreen window, rendered into the widget's top-level window.
QQuickItem *videoItem(const QWidget *quickWidget, QLatin1StringView name)
{
	const QWindow *host = quickWidget->window()->windowHandle();
	QQuickItem *hidden = nullptr;
	for (QWindow *w : QGuiApplication::topLevelWindows()) {
		auto *scene = qobject_cast<QQuickWindow *>(w);
		if (!scene || QQuickRenderControl::renderWindowFor(scene) != host)
			continue;
		// After a switch to another video an old scene may linger: the shown one counts.
		if (QQuickItem *found = findItem(scene->contentItem(), name)) {
			if (found->isVisible())
				return found;
			if (!hidden)
				hidden = found;
		}
	}
	return hidden;
}

QQuickItem *controlItem(const QWidget *quickWidget, int control)
{
	return control >= 0 && control < ControlCount ? videoItem(quickWidget, QLatin1StringView(kControlNames[control])) : nullptr;
}

int controlOf(const QQuickItem *item)
{
	for (int c = 0; c < ControlCount; ++c) {
		if (item->objectName() == QLatin1StringView(kControlNames[c]))
			return c;
	}
	return -1;
}

// The video widget an item is shown in.
QWidget *hostOf(const QQuickItem *item)
{
	const QWindow *host = item->window() ? QQuickRenderControl::renderWindowFor(item->window()) : nullptr;
	if (!host)
		return nullptr;
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (top->windowHandle() != host)
			continue;
		for (QWidget *w : top->findChildren<QWidget *>()) {
			const QQuickItem *slider = inheritsClass(w, "QQuickWidget") ? videoItem(w, QLatin1StringView("timeSlider")) : nullptr;
			if (slider && slider->window() == item->window())
				return w;
		}
	}
	return nullptr;
}

// The viewer stacks an image panel and a video panel; the controls exist only for a shown video.
bool showsVideo(const QWidget *quickWidget)
{
	const QWidget *panel = ancestorInheriting(quickWidget, "VideoPlayerPanel");
	return panel && panel->isVisible() && quickWidget->isVisible()
	    && videoItem(quickWidget, QLatin1StringView("timeSlider"));
}

bool VideoKeyFilter::showsVideoWidget(const QWidget *w)
{
	return showsVideo(w);
}

QQuickItem *panelOf(QQuickItem *item)
{
	for (QQuickItem *i = item; i; i = i->parentItem()) {
		if (QLatin1StringView(i->metaObject()->className()).startsWith(QLatin1StringView("VideoRenderPanel")))
			return i;
	}
	return nullptr;
}

double parseTime(const QString &text)
{
	double seconds = 0;
	bool any = false;
	for (const QString &part : cleanText(text).split(QLatin1Char(':'))) {
		bool ok = false;
		const int n = part.trimmed().toInt(&ok);
		if (!ok)
			return 0;
		seconds = seconds * 60 + n;
		any = true;
	}
	return any ? seconds : 0;
}

QString formatTime(double seconds)
{
	const int s = qMax(0, qRound(seconds));
	return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
	                 : QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// The video's length, from the footer's "00:38".
double totalSeconds(QQuickItem *anyControl)
{
	QQuickItem *panel = panelOf(anyControl);
	QQuickItem *total = panel ? findItem(panel, QLatin1StringView("totalTimeLabel")) : nullptr;
	return total ? parseTime(total->property("text").toString()) : 0;
}

QString sourceOf(const QObject *o)
{
	const QVariant source = o->property("source");
	return source.metaType().id() == QMetaType::QUrl ? source.toUrl().toString() : source.toString();
}

// The speed box's choices ("2.0x" ... "0.2x", fastest first).
QStringList speeds(const QQuickItem *box)
{
	return box->property("model").toStringList();
}

void seekTo(QQuickItem *slider, double fraction)
{
	QQuickItem *panel = panelOf(slider);
	if (!panel)
		return;
	fraction = qBound(0.0, fraction, 1.0);
	// What the slider's MouseArea does on press and release.
	slider->setProperty("value", fraction);
	emitSignal(panel, "timeSliderPressed()");
	slider->setProperty("value", fraction);
	emitSignal(panel, "timeSliderReleased()");
	a11yLog("[video] seek to %.0f%%", fraction * 100);
}

// Pause: the footer button's signal. Play: it did nothing once paused (the player resumed only from
// the centre): what the centre button (replay after the end) or a click on the video sends.
void togglePlay(QQuickItem *button)
{
	QQuickItem *panel = panelOf(button);
	if (!panel)
		return;
	if (!sourceOf(button).contains(QLatin1StringView("player-play"))) {
		emitSignal(panel, "playBtnClicked()");
		a11yLog("[video] pause");
		return;
	}
	QQuickItem *replay = findItem(panel, QLatin1StringView("replayAtCenterBtn"));
	QQuickItem *center = findItem(panel, QLatin1StringView("playAtCenterBtn"));
	const char *signal = replay && replay->isVisible() ? "replayAtCenterBtnClicked()"
	    : center && center->isVisible()                ? "playAtCenterBtnClicked()"
	                                                   : "playClicked()";
	emitSignal(panel, signal);
	a11yLog("[video] play (%s)", signal);
}

// One step faster (-1) or slower (+1): the box's handler tells the player (speedChanged).
void changeSpeed(QQuickItem *box, int step)
{
	const int count = int(speeds(box).size());
	const int index = box->property("currentIndex").toInt();
	const int next = qBound(0, index + step, count - 1);
	if (count == 0 || next == index)
		return;
	box->setProperty("currentIndex", next);
	a11yLog("[video] speed %s", qUtf8Printable(speeds(box).value(next)));
}

class LineVideoControl : public QAccessibleObject, public QAccessibleActionInterface, public QAccessibleValueInterface {
public:
	explicit LineVideoControl(QQuickItem *item)
		: QAccessibleObject(item)
	{
	}

	QQuickItem *item() const { return static_cast<QQuickItem *>(object()); }
	int control() const { return controlOf(item()); }
	QWidget *host() const { return hostOf(item()); }

	QAccessibleInterface *parent() const override
	{
		QWidget *h = host();
		return h ? QAccessible::queryAccessibleInterface(h) : nullptr;
	}
	QAccessibleInterface *child(int) const override { return nullptr; }
	int childCount() const override { return 0; }
	int indexOfChild(const QAccessibleInterface *) const override { return -1; }
	QWindow *window() const override
	{
		QWidget *h = host();
		return h ? h->window()->windowHandle() : nullptr;
	}

	QAccessible::Role role() const override { return control() == PlayButton ? QAccessible::Button : QAccessible::Slider; }

	QString text(QAccessible::Text t) const override
	{
		if (t == QAccessible::Name) {
			switch (control()) {
			case PositionSlider:
				return label("Position");
			case SpeedBox:
				return label("Playback speed");
			default:
				return sourceOf(item()).contains(QLatin1StringView("player-play")) ? label("Play") : label("Pause");
			}
		}
		if (t == QAccessible::Value && control() == PositionSlider) {
			const double total = totalSeconds(item());
			return formatTime(item()->property("value").toDouble() * total) + QStringLiteral(" / ") + formatTime(total);
		}
		if (t == QAccessible::Value && control() == SpeedBox)
			return speeds(item()).value(item()->property("currentIndex").toInt());
		return QString();
	}

	QRect rect() const override
	{
		QWidget *h = host();
		if (!h)
			return QRect();
		const QPoint topLeft = h->mapToGlobal(item()->mapToScene(QPointF(0, 0)).toPoint());
		return QRect(topLeft, QSize(int(item()->width()), int(item()->height())));
	}

	QAccessible::State state() const override
	{
		QAccessible::State st;
		QWidget *h = host();
		if (!h || !h->isVisible()) {
			st.invisible = true;
			return st;
		}
		st.focusable = true;
		st.focused = h->hasFocus() && h->property(kFocusProperty).toInt() == control();
		return st;
	}

	void *interface_cast(QAccessible::InterfaceType t) override
	{
		if (t == QAccessible::ActionInterface)
			return static_cast<QAccessibleActionInterface *>(this);
		if (t == QAccessible::ValueInterface && control() != PlayButton)
			return static_cast<QAccessibleValueInterface *>(this);
		return nullptr;
	}

	QStringList actionNames() const override
	{
		if (control() != PlayButton)
			return { increaseAction(), decreaseAction(), setFocusAction() };
		return { pressAction(), setFocusAction() };
	}
	QStringList keyBindingsForAction(const QString &) const override { return {}; }
	void doAction(const QString &actionName) override
	{
		const bool up = actionName == increaseAction();
		if (actionName == pressAction() && control() == PlayButton) {
			togglePlay(item());
		} else if ((up || actionName == decreaseAction()) && control() == SpeedBox) {
			changeSpeed(item(), up ? -1 : 1);
		} else if (up || actionName == decreaseAction()) {
			const double total = totalSeconds(item());
			if (total > 0)
				seekTo(item(), item()->property("value").toDouble() + (up ? kStepSeconds : -kStepSeconds) / total);
		} else if (actionName == setFocusAction()) {
			if (QWidget *h = host()) {
				h->setProperty(kFocusProperty, control());
				h->setFocus(Qt::OtherFocusReason);
			}
		}
	}

	// Position: seconds. Speed: the choice's number (2.0 ... 0.2).
	QVariant currentValue() const override
	{
		if (control() == SpeedBox)
			return speeds(item()).value(item()->property("currentIndex").toInt()).chopped(1).toDouble();
		return item()->property("value").toDouble() * totalSeconds(item());
	}
	void setCurrentValue(const QVariant &value) override
	{
		if (control() == SpeedBox) {
			const QStringList choices = speeds(item());
			for (int i = 0; i < choices.size(); ++i) {
				if (qFuzzyCompare(choices.at(i).chopped(1).toDouble(), value.toDouble()))
					item()->setProperty("currentIndex", i);
			}
			return;
		}
		const double total = totalSeconds(item());
		if (total > 0)
			seekTo(item(), value.toDouble() / total);
	}
	QVariant maximumValue() const override
	{
		if (control() == SpeedBox)
			return speeds(item()).value(0).chopped(1).toDouble();
		return totalSeconds(item());
	}
	QVariant minimumValue() const override
	{
		if (control() == SpeedBox)
			return speeds(item()).isEmpty() ? 0.0 : speeds(item()).last().chopped(1).toDouble();
		return 0.0;
	}
	QVariant minimumStepSize() const override { return control() == SpeedBox ? 0.1 : kStepSeconds; }
};

void report(QWidget *quickWidget, QAccessible::Event type)
{
	QAccessibleInterface *control = videoControl(quickWidget, quickWidget->property(kFocusProperty).toInt());
	if (!control)
		return;
	g_reporting = true;
	if (type == QAccessible::ValueChanged) {
		QAccessibleValueChangeEvent event(control, control->valueInterface() ? control->valueInterface()->currentValue() : QVariant());
		QAccessible::updateAccessibility(&event);
	} else {
		QAccessibleEvent event(control, type);
		QAccessible::updateAccessibility(&event);
	}
	g_reporting = false;
}

// Tab out of the player: on to the viewer's next (or previous) control. Tab inside the QQuickWidget
// would otherwise move Qt Quick's focus to LINE's own speed box and volume slider, unnamed for NVDA.
void leavePlayer(QWidget *quickWidget, bool forward)
{
	QWidget *w = quickWidget;
	for (int guard = 0; guard < 200; ++guard) {
		w = forward ? w->nextInFocusChain() : w->previousInFocusChain();
		if (!w || w == quickWidget)
			return;
		if (w->window() == quickWidget->window() && w->isVisible() && w->isEnabled() && (w->focusPolicy() & Qt::TabFocus)) {
			w->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
			return;
		}
	}
}

// The keys the player takes (also before LINE's shortcuts: Right = next media, Space = LINE's own
// play / pause, which only work while the viewer window is active).
bool wantsKey(const QKeyEvent *key)
{
	if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
		return false;
	switch (key->key()) {
	case Qt::Key_Tab:
	case Qt::Key_Backtab:
	case Qt::Key_Space:
	case Qt::Key_Return:
	case Qt::Key_Enter:
	case Qt::Key_Left:
	case Qt::Key_Right:
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

} // namespace

QAccessibleInterface *videoControlAccessible(QObject *object)
{
	auto *item = qobject_cast<QQuickItem *>(object);
	if (!item || helperProcessName() != QLatin1StringView("linemediaplayer") || controlOf(item) < 0)
		return nullptr;
	return new LineVideoControl(item);
}

int videoControlCount(QWidget *quickWidget)
{
	return showsVideo(quickWidget) ? ControlCount : 0;
}

QAccessibleInterface *videoControl(QWidget *quickWidget, int index)
{
	if (index < 0 || index >= videoControlCount(quickWidget))
		return nullptr;
	QQuickItem *item = controlItem(quickWidget, index);
	return item ? QAccessible::queryAccessibleInterface(item) : nullptr;
}

int indexOfVideoControl(QWidget *quickWidget, const QAccessibleInterface *child)
{
	for (int i = 0; i < videoControlCount(quickWidget); ++i) {
		if (child && child->object() && child->object() == controlItem(quickWidget, i))
			return i;
	}
	return -1;
}

QAccessibleInterface *focusedVideoControl(QWidget *quickWidget)
{
	if (!quickWidget->hasFocus())
		return nullptr;
	return videoControl(quickWidget, quickWidget->property(kFocusProperty).toInt());
}

bool videoWantsShortcut(QWidget *quickWidget, QKeyEvent *key)
{
	return showsVideo(quickWidget) && wantsKey(key);
}

bool isPlayerValueEvent(QAccessibleEvent *event)
{
	if (g_reporting || event->type() != QAccessible::ValueChanged)
		return false;
	auto *item = qobject_cast<QQuickItem *>(event->object());
	return item && helperProcessName() == QLatin1StringView("linemediaplayer") && controlOf(item) == PositionSlider;
}

bool handleVideoKey(QWidget *quickWidget, QKeyEvent *key)
{
	if (!showsVideo(quickWidget) || !wantsKey(key))
		return false;
	const int focus = quickWidget->property(kFocusProperty).toInt();
	switch (key->key()) {
	case Qt::Key_Tab:
	case Qt::Key_Backtab: {
		const bool forward = key->key() == Qt::Key_Tab && !(key->modifiers() & Qt::ShiftModifier);
		const int next = focus + (forward ? 1 : -1);
		if (next < 0 || next >= ControlCount) {
			leavePlayer(quickWidget, forward);
			return true;
		}
		quickWidget->setProperty(kFocusProperty, next);
		report(quickWidget, QAccessible::Focus);
		return true;
	}
	case Qt::Key_Space:
	case Qt::Key_Return:
	case Qt::Key_Enter: {
		if (key->isAutoRepeat())
			return true;
		QQuickItem *button = controlItem(quickWidget, PlayButton);
		if (!button)
			return true;
		togglePlay(button);
		// The button's icon (and name) follows the player.
		QPointer<QWidget> guard(quickWidget);
		QTimer::singleShot(300, quickWidget, [guard] {
			if (guard && guard->property(kFocusProperty).toInt() == PlayButton)
				report(guard, QAccessible::NameChanged);
		});
		return true;
	}
	default:
		break;
	}
	// The speed: Up / Right faster, Down / Left slower, Home / End fastest / slowest.
	if (focus == SpeedBox) {
		QQuickItem *box = controlItem(quickWidget, SpeedBox);
		if (!box)
			return true;
		switch (key->key()) {
		case Qt::Key_Up:
		case Qt::Key_Right:
		case Qt::Key_PageUp:
			changeSpeed(box, -1);
			break;
		case Qt::Key_Down:
		case Qt::Key_Left:
		case Qt::Key_PageDown:
			changeSpeed(box, 1);
			break;
		case Qt::Key_Home:
			changeSpeed(box, -int(speeds(box).size()));
			break;
		case Qt::Key_End:
			changeSpeed(box, int(speeds(box).size()));
			break;
		default:
			break;
		}
		report(quickWidget, QAccessible::ValueChanged);
		return true;
	}
	QQuickItem *slider = controlItem(quickWidget, PositionSlider);
	const double total = slider ? totalSeconds(slider) : 0;
	if (!slider || total <= 0)
		return true;
	const double now = slider->property("value").toDouble();
	double target = now;
	switch (key->key()) {
	case Qt::Key_Right:
	case Qt::Key_Up:
		target = now + kStepSeconds / total;
		break;
	case Qt::Key_Left:
	case Qt::Key_Down:
		target = now - kStepSeconds / total;
		break;
	case Qt::Key_PageUp:
		target = now + kPageSeconds / total;
		break;
	case Qt::Key_PageDown:
		target = now - kPageSeconds / total;
		break;
	case Qt::Key_Home:
		target = 0;
		break;
	case Qt::Key_End:
		target = 1;
		break;
	default:
		break;
	}
	seekTo(slider, target);
	// Seeking from the button moves the bridge's focus to the slider, which then reads its value.
	if (focus != PositionSlider) {
		quickWidget->setProperty(kFocusProperty, int(PositionSlider));
		report(quickWidget, QAccessible::Focus);
	} else {
		report(quickWidget, QAccessible::ValueChanged);
	}
	return true;
}

void keepVideoKeyFilterFirst()
{
	if (helperProcessName() != QLatin1StringView("linemediaplayer"))
		return;
	if (!g_keyFilter)
		g_keyFilter = new VideoKeyFilter;
	// The filter installed last is asked first: stay after any LINE installs later.
	QCoreApplication::instance()->removeNativeEventFilter(g_keyFilter);
	QCoreApplication::instance()->installNativeEventFilter(g_keyFilter);
	// The keyboard hook: only while a video shows, newest again each second.
	bool video = false;
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (!top->isVisible() || !inheritsClass(top, "MediaViewerWindow"))
			continue;
		for (QWidget *w : top->findChildren<QWidget *>()) {
			if (inheritsClass(w, "QQuickWidget") && showsVideo(w))
				video = true;
		}
	}
	if (g_keyHook) {
		UnhookWindowsHookEx(g_keyHook);
		g_keyHook = nullptr;
	}
	if (video) {
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   reinterpret_cast<LPCWSTR>(&lowLevelKey), &self);
		g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKey, self, 0);
		static bool logged = false;
		if (!logged) {
			logged = true;
			a11yLog("[video] keyboard hook %s", g_keyHook ? "installed" : "failed");
		}
	}
}

void videoWidgetFocused(QWidget *quickWidget)
{
	if (!showsVideo(quickWidget))
		return;
	QPointer<QWidget> guard(quickWidget);
	QTimer::singleShot(0, quickWidget, [guard] {
		if (guard && guard->hasFocus())
			report(guard, QAccessible::Focus);
	});
}

} // namespace line_a11y

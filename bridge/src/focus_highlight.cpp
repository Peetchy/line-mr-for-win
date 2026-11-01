#include "focus_highlight.h"

#include "a11y_log.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <windows.h>

namespace line_a11y {
namespace {

const wchar_t kWindowClass[] = L"lineMR_focus_highlight";
// Pixels of this colour are transparent (LWA_COLORKEY); not one of the add-on's colours.
const COLORREF kTransparent = RGB(255, 0, 255);
const COLORREF kOutline = RGB(0, 0, 0);

struct Settings {
	bool enabled = true;
	COLORREF color = RGB(255, 214, 0);
	int width = 4; // of the coloured band, at 100 % scale
};

Settings g_settings;
// Width of the coloured band of the frame shown, in native pixels.
int g_band = 4;

HMODULE thisModule()
{
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCWSTR>(&thisModule), &module);
	return module;
}

// Black outline, coloured band, black outline: visible on light and dark backgrounds alike.
void paintFrame(HWND hwnd, HDC dc)
{
	RECT r;
	GetClientRect(hwnd, &r);
	HBRUSH clear = CreateSolidBrush(kTransparent);
	HBRUSH outline = CreateSolidBrush(kOutline);
	HBRUSH color = CreateSolidBrush(g_settings.color);
	FillRect(dc, &r, outline);
	RECT inner = r;
	InflateRect(&inner, -1, -1);
	FillRect(dc, &inner, color);
	InflateRect(&inner, -g_band, -g_band);
	FillRect(dc, &inner, outline);
	InflateRect(&inner, -1, -1);
	if (inner.right > inner.left && inner.bottom > inner.top)
		FillRect(dc, &inner, clear);
	DeleteObject(clear);
	DeleteObject(outline);
	DeleteObject(color);
}

LRESULT CALLBACK frameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		paintFrame(hwnd, dc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

QWindow *windowOf(QAccessibleInterface *iface)
{
	for (int depth = 0; iface && depth < 64; ++depth, iface = iface->parent()) {
		if (QWindow *w = iface->window())
			return w;
	}
	return nullptr;
}

class FocusHighlight : public QObject {
public:
	explicit FocusHighlight(QObject *parent)
	    : QObject(parent)
	{
		loadSettings();
		connect(&m_timer, &QTimer::timeout, this, [this] {
			if (++m_ticks % 10 == 0)
				loadSettings();
			update();
		});
		m_timer.start(100);
		connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this] { update(); });
	}

	~FocusHighlight() override
	{
		if (m_hwnd)
			DestroyWindow(m_hwnd);
	}

	void setTarget(QAccessibleInterface *iface)
	{
		m_target = iface ? QAccessible::uniqueId(iface) : 0;
		update();
		// Lists scroll the focused row into view, windows finish their layout.
		QTimer::singleShot(50, this, [this] { update(); });
	}

private:
	void loadSettings()
	{
		const QString path = bridgeDirectory() + QStringLiteral("focus-highlight.ini");
		const QDateTime modified = QFileInfo(path).lastModified();
		if (m_settingsLoaded && modified == m_settingsModified)
			return;
		m_settingsLoaded = true;
		m_settingsModified = modified;
		Settings s;
		if (modified.isValid()) {
			QSettings ini(path, QSettings::IniFormat);
			s.enabled = ini.value(QStringLiteral("highlight/enabled"), true).toBool();
			// RGB in hex, without '#' (QSettings could take it for a comment).
			const QColor c(QLatin1Char('#') + ini.value(QStringLiteral("highlight/color")).toString());
			if (c.isValid())
				s.color = RGB(c.red(), c.green(), c.blue());
			s.width = qBound(1, ini.value(QStringLiteral("highlight/width"), s.width).toInt(), 12);
		}
		g_settings = s;
		a11yLog("[highlight] %s, colour #%02X%02X%02X, width %d", s.enabled ? "on" : "off",
		        GetRValue(s.color), GetGValue(s.color), GetBValue(s.color), s.width);
		if (m_hwnd)
			InvalidateRect(m_hwnd, nullptr, TRUE);
	}

	HWND frameWindow(HWND owner)
	{
		if (!m_hwnd) {
			static bool registered = false;
			if (!registered) {
				WNDCLASSEXW wc = { sizeof(wc) };
				wc.style = CS_HREDRAW | CS_VREDRAW;
				wc.lpfnWndProc = frameProc;
				wc.hInstance = thisModule();
				wc.lpszClassName = kWindowClass;
				registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
			}
			m_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			                         kWindowClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, thisModule(), nullptr);
			if (!m_hwnd) {
				a11yLog("[highlight] could not create the frame window (error %lu)", GetLastError());
				return nullptr;
			}
			SetLayeredWindowAttributes(m_hwnd, kTransparent, 0, LWA_COLORKEY);
			m_owner = owner;
		}
		if (owner != m_owner) {
			// An owned window stays above its owner (also a popup menu), and hides with it.
			SetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
			m_owner = owner;
		}
		return m_hwnd;
	}

	void hide()
	{
		if (m_hwnd && IsWindowVisible(m_hwnd))
			ShowWindow(m_hwnd, SW_HIDE);
		m_shown = {};
	}

	void update()
	{
		if (!g_settings.enabled || QGuiApplication::applicationState() != Qt::ApplicationActive)
			return hide();
		QAccessibleInterface *iface = m_target ? QAccessible::accessibleInterface(m_target) : nullptr;
		if (!iface || !iface->isValid() || iface->state().invisible || iface->state().offscreen)
			return hide();
		const QRect r = iface->rect();
		QWindow *w = windowOf(iface);
		if (r.isEmpty() || !w || !w->isVisible() || !w->handle())
			return hide();
		const HWND owner = reinterpret_cast<HWND>(w->winId());
		// Logical position inside the window, then native pixels on the window's screen.
		const qreal dpr = w->devicePixelRatio();
		const QPoint local = w->mapFromGlobal(r.topLeft());
		POINT origin = { 0, 0 };
		ClientToScreen(owner, &origin);
		RECT target = { origin.x + qRound(local.x() * dpr), origin.y + qRound(local.y() * dpr), 0, 0 };
		target.right = target.left + qRound(r.width() * dpr);
		target.bottom = target.top + qRound(r.height() * dpr);
		const int band = qMax(2, qRound(g_settings.width * dpr));
		RECT frame = target;
		InflateRect(&frame, band + 2, band + 2);
		// Rows scrolled partly out of view: only the part inside the window.
		RECT windowRect;
		GetWindowRect(owner, &windowRect);
		if (!IntersectRect(&frame, &frame, &windowRect))
			return hide();
		const bool ownerChanged = owner != m_owner;
		HWND hwnd = frameWindow(owner);
		if (!hwnd)
			return;
		if (!ownerChanged && EqualRect(&frame, &m_shown) && band == g_band && IsWindowVisible(hwnd))
			return;
		g_band = band;
		// Qt's popups (menus) are topmost, and owning the frame does not lift it above them.
		const bool topmost = GetWindowLongPtrW(owner, GWL_EXSTYLE) & WS_EX_TOPMOST;
		if (!topmost && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
			SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_TOP, frame.left, frame.top, frame.right - frame.left,
		             frame.bottom - frame.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect(hwnd, nullptr, TRUE);
		m_shown = frame;
	}

	QTimer m_timer;
	quint64 m_ticks = 0;
	bool m_settingsLoaded = false;
	QDateTime m_settingsModified;
	QAccessible::Id m_target = 0;
	HWND m_hwnd = nullptr;
	HWND m_owner = nullptr;
	RECT m_shown = {};
};

QPointer<FocusHighlight> g_highlight;

} // namespace

void startFocusHighlight(QCoreApplication *app)
{
	if (!g_highlight && qGuiApp)
		g_highlight = new FocusHighlight(app);
}

void noteAccessibleEvent(QAccessibleEvent *event)
{
	if (!g_highlight || event->type() != QAccessible::Focus)
		return;
	QAccessibleInterface *iface = event->accessibleInterface();
	// UI Automation reports a table's or list's focused item, not the view (as Qt's UIA does).
	if (iface && (iface->tableInterface() || iface->role() == QAccessible::List || iface->role() == QAccessible::Tree)) {
		if (QAccessibleInterface *child = iface->focusChild())
			iface = child;
	}
	g_highlight->setTarget(iface);
}

} // namespace line_a11y

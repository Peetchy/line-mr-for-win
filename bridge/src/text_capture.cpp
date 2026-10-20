#include "text_capture.h"

#include "a11y_log.h"

#include <QtCore/QFile>

#include <QtGui/QPaintDevice>
#include <QtGui/QPaintEngine>
#include <QtGui/QTextItem>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace line_a11y {
namespace {

class TextCaptureEngine final : public QPaintEngine {
public:
	explicit TextCaptureEngine(CaptureResult *out)
		: QPaintEngine(QPaintEngine::AllFeatures), m_out(out)
	{
	}

	bool begin(QPaintDevice *) override { return true; }
	bool end() override { return true; }
	Type type() const override { return QPaintEngine::User; }

	void updateState(const QPaintEngineState &newState) override
	{
		const QPaintEngine::DirtyFlags dirty = newState.state();
		if (dirty & QPaintEngine::DirtyTransform)
			m_transform = newState.transform();
		if (dirty & QPaintEngine::DirtyOpacity)
			m_opacity = newState.opacity();
		if (dirty & QPaintEngine::DirtyPen)
			m_penAlpha = newState.pen().color().alpha();
	}

	void drawTextItem(const QPointF &p, const QTextItem &ti) override
	{
		// Invisible text (faded-out hover menus over photos, hidden labels) is not on screen.
		if (m_opacity <= 0.01 || m_penAlpha == 0)
			return;
		const QString text = ti.text();
		if (text.isEmpty()) {
			++m_out->glyphRunsWithoutText;
			return;
		}
		m_out->items.push_back({ m_transform.map(p), text });
	}

	void drawPixmap(const QRectF &r, const QPixmap &, const QRectF &) override { recordPixmap(r); }
	void drawImage(const QRectF &r, const QImage &, const QRectF &, Qt::ImageConversionFlags) override
	{
		recordPixmap(r);
	}
	void drawTiledPixmap(const QRectF &, const QPixmap &, const QPointF &) override { ++m_out->pixmaps; }
	void drawPolygon(const QPointF *, int, PolygonDrawMode) override {}
	void drawPolygon(const QPoint *, int, PolygonDrawMode) override {}
	void drawRects(const QRectF *, int) override {}
	void drawRects(const QRect *, int) override {}
	void drawLines(const QLineF *, int) override {}
	void drawLines(const QLine *, int) override {}
	void drawEllipse(const QRectF &) override {}
	void drawEllipse(const QRect &) override {}
	void drawPath(const QPainterPath &) override {}
	void drawPoints(const QPointF *, int) override {}
	void drawPoints(const QPoint *, int) override {}

private:
	void recordPixmap(const QRectF &r)
	{
		++m_out->pixmaps;
		m_out->pixmapRects << m_transform.mapRect(r);
	}

	CaptureResult *m_out;
	QTransform m_transform;
	qreal m_opacity = 1;
	int m_penAlpha = 255;
};

// Spare room right of and below the rendered area, for painting that runs a little past it.
constexpr int kMargin = 64;

// A real QImage (so code in LINE that inspects or casts the painter's device to QImage, or
// writes pixels into it, stays within valid memory) whose paint engine records text instead of
// rasterising. LINE crashed with heap corruption while message rows with images were rendered
// into the previous, bare QPaintDevice. (Rendering at an offset inside a larger image loses the
// text of tall message rows: LcListView paints its rows relative to the device origin.)
class TextCaptureDevice final : public QImage {
public:
	TextCaptureDevice(const QSize &size, int dpi, CaptureResult *out)
		: QImage(size.expandedTo(QSize(1, 1)) + QSize(kMargin, kMargin), QImage::Format_ARGB32_Premultiplied),
		  m_engine(out)
	{
		const int dotsPerMeter = int(std::lround(dpi / 0.0254));
		setDotsPerMeterX(dotsPerMeter);
		setDotsPerMeterY(dotsPerMeter);
		fill(Qt::white);
	}

	QPaintEngine *paintEngine() const override { return &m_engine; }

private:
	mutable TextCaptureEngine m_engine;
};

bool g_capturing = false;
bool g_safeMode = false;

// Diagnostics (%LOCALAPPDATA%\LineMR\heap-check exists at start-up): validate the whole heap
// around every render and log the first corruption found, to tell whether rendering causes it.
bool heapCheckEnabled()
{
	static const bool on = QFile::exists(bridgeDirectory() + QStringLiteral("heap-check"));
	return on;
}

void checkHeap(const char *when, const QWidget *w)
{
	static bool reported = false;
	if (reported || !heapCheckEnabled() || HeapValidate(GetProcessHeap(), 0, nullptr))
		return;
	reported = true;
	a11yLog("[heap] corrupt %s rendering %s", when, w->metaObject()->className());
}


// Rendering re-enters LINE's paint code: never while LINE itself is painting (a screen-reader
// query can arrive from a message loop LINE runs inside a paint event) or while another capture
// is running.
bool captureAllowed()
{
	if (g_capturing)
		return false;
	const QWidgetList widgets = QApplication::allWidgets();
	for (QWidget *w : widgets) {
		if (w->paintingActive())
			return false;
	}
	return true;
}

struct CaptureScope {
	CaptureScope() { g_capturing = true; }
	~CaptureScope() { g_capturing = false; }
};

} // namespace

CaptureResult captureWidgetText(QWidget *w, bool children)
{
	CaptureResult result;
	if (!w || w->width() <= 0 || w->height() <= 0)
		return result;
	if (!captureAllowed()) {
		result.skipped = true;
		return result;
	}
	CaptureScope scope;
	TextCaptureDevice dev(w->size(), w->logicalDpiX() > 0 ? w->logicalDpiX() : 96, &result);
	QWidget::RenderFlags flags = QWidget::DrawWindowBackground;
	if (children)
		flags |= QWidget::DrawChildren;
	breadcrumb("render %s %dx%d%s", w->metaObject()->className(), w->width(), w->height(), children ? " +children" : "");
	checkHeap("before", w);
	w->render(&dev, QPoint(), QRegion(), flags);
	checkHeap("after", w);
	breadcrumb("render done");
	return result;
}

void setSafeMode(bool on)
{
	g_safeMode = on;
}

CaptureResult captureRegionText(QWidget *w, const QRect &rect)
{
	CaptureResult result;
	if (g_safeMode) {
		result.skipped = true;
		return result;
	}
	const QRect area = rect & (w ? w->rect() : QRect());
	if (area.isEmpty())
		return result;
	if (!captureAllowed()) {
		result.skipped = true;
		return result;
	}
	CaptureScope scope;
	TextCaptureDevice dev(area.size(), w->logicalDpiX() > 0 ? w->logicalDpiX() : 96, &result);
	// render() maps the region's top-left to the target offset; the engine does not clip, so
	// neighbouring rows may still draw: keep what lies inside the area.
	breadcrumb("render region of %s at %d,%d %dx%d", w->metaObject()->className(), area.x(), area.y(), area.width(),
	           area.height());
	checkHeap("before", w);
	w->render(&dev, QPoint(), QRegion(area), QWidget::DrawChildren);
	checkHeap("after", w);
	breadcrumb("render done");
	QList<CapturedText> inside;
	for (const CapturedText &it : std::as_const(result.items)) {
		if (it.pos.y() > 0 && it.pos.y() <= area.height() + 2)
			inside << it;
	}
	result.items = inside;
	QList<QRectF> pixmapsInside;
	for (const QRectF &r : std::as_const(result.pixmapRects)) {
		if (r.bottom() > 0 && r.top() < area.height())
			pixmapsInside << r;
	}
	result.pixmapRects = pixmapsInside;
	result.pixmaps = int(pixmapsInside.size());
	return result;
}

QString capturedTextInReadingOrder(const CaptureResult &r)
{
	QList<CapturedText> items = r.items;
	// Items on (roughly) the same baseline are one line; 4 px tolerates mixed fonts.
	std::stable_sort(items.begin(), items.end(), [](const CapturedText &a, const CapturedText &b) {
		if (std::abs(a.pos.y() - b.pos.y()) > 4)
			return a.pos.y() < b.pos.y();
		return a.pos.x() < b.pos.x();
	});
	// Thai is written without spaces between words and LINE wraps it anywhere, so a line break
	// between two Thai characters is not a word boundary.
	auto isThai = [](QChar c) { return c.unicode() >= 0x0E00 && c.unicode() <= 0x0E7F; };
	QString out;
	qreal lastY = -1e9;
	for (const CapturedText &it : items) {
		if (!out.isEmpty() && !it.text.isEmpty() && std::abs(it.pos.y() - lastY) > 4)
			out += isThai(out.back()) && isThai(it.text.front()) ? QString() : QStringLiteral("\n");
		out += it.text;
		lastY = it.pos.y();
	}
	return out;
}

} // namespace line_a11y

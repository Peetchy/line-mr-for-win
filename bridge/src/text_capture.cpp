#include "text_capture.h"

#include "a11y_log.h"

#include <QtCore/QFile>
#include <QtCore/QHash>

#include <QtGui/QPaintDevice>
#include <QtGui/QPaintEngine>
#include <QtGui/QTextItem>
#include <QtGui/QFontMetricsF>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace line_a11y {
namespace {

// ---- Known icons -----------------------------------------------------------------------------------
//
// Some message types are drawn with an icon of their own from LINE's resources (default.dat, mounted
// at :/resource): the play / pause button of a voice message, ... The images LINE draws are compared
// with those icons by shape: both are scaled to a small alpha mask (sizes differ with the display
// scale and @2x variants). Button sprites hold their states side by side.

constexpr int kMaskSide = 12;
constexpr int kMaxMaskDifference = 28; // mean alpha difference (0-255) still taken as the same icon

struct KnownIcon {
	const char *path;
	int slices;
	const char *name;
	// Compared by colour (over black) instead of shape: LINE's reaction faces are all round.
	bool color = false;
};

const KnownIcon kKnownIcons[] = {
	{ ":/resource/img/basic/chat/chat_word_send_voice_play_icon.png", 4, "voice" },
	{ ":/resource/img/basic/chat/chat_word_send_voice_pause_icon.png", 4, "voice" },
	{ ":/resource/img/basic/chat/chat_word_receive_voice_play_icon.png", 4, "voice" },
	{ ":/resource/img/basic/chat/chat_word_receive_voice_pause_icon.png", 4, "voice" },
	// The play button over a video's thumbnail (34x34; a white triangle like the voice play icon,
	// so messageKind() also looks at what it is drawn on).
	{ ":/resource/img/basic/chat/chat_video_icon.svg", 1, "video" },
	{ ":/resource/img/basic/chat/chatcontents/chat_video_icon.svg", 1, "video" },
	{ ":/resource/img/basic/chat/chatcontents/video_icon.svg", 1, "video" },
	{ ":/resource/img/basic/chat/flex_video_play.svg", 1, "video" },
	{ ":/resource/img/basic/chat/chatcontents/no_mov_icon.svg", 1, "video" },
	// Notes (posts): the play button on a video, the reaction and comment buttons (the numbers
	// beside them say how many).
	{ ":/resource/img/basic/album_note/play_icon.svg", 1, "video" },
	{ ":/resource/img/basic/album_note/smile_off_26.svg", 1, "note-reactions" },
	{ ":/resource/img/basic/album_note/smile_on_26.svg", 1, "note-reactions" },
	{ ":/resource/img/basic/album_note/comment_icon.svg", 1, "note-comments" },
	{ ":/resource/img/basic/chat/chatcontents/no_img_icon.svg", 1, "photo" },
	// File type icons beside a file's name (45x45).
	{ ":/resource/img/basic/chat/chatcontents/ai_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/docx_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/mic_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/mov_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/mp3_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/pdf_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/ppt_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/psd_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/txt_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/xlsx_ic.svg", 1, "file" },
	{ ":/resource/img/basic/chat/chatcontents/zip_ic.svg", 1, "file" },
	// The phone in a call's bubble (messageKind(): a small bubble with any 20-28 px icon was taken for
	// a call, which made link previews read "Call").
	{ ":/resource/img/basic/chat/call.svg", 1, "call" },
	{ ":/resource/img/basic/chat/calling.svg", 1, "call" },
	{ ":/resource/img/basic/chat/call-join.svg", 1, "call" },
	{ ":/resource/img/basic/chat/video-call.svg", 1, "call" },
	{ ":/resource/img/basic/chat/video-call-join.svg", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_send_linecall_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_receive_linecall_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_send_linecallsucess_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_receive_linecallsucess_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_send_linecallfail_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_receive_linecallfail_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_send_freecallsucess_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_word_send_freecallfail_icon.png", 1, "call" },
	{ ":/resource/img/basic/chat/chat_groupcall_video_icon.png", 1, "call" },
	// The red flag in a location's bubble (14x16).
	{ ":/resource/img/basic/chat/chat_word_flag.png", 1, "location" },
	// Sound stickers and stickers that could not be loaded.
	{ ":/resource/img/basic/chat/sticker_icon_sound.png", 1, "sticker-sound" },
	// The speaker button beside a sound sticker in a chat (27x30, sprites of 4 states): plays it again.
	{ ":/resource/img/basic/chat/sound_02.png", 4, "sticker-sound" },
	{ ":/resource/img/basic/chat/chat_popup_sound.png", 4, "sticker-sound" },
	{ ":/resource/img/basic/chat/chat_effect_sound.png", 4, "sticker-sound" },
	{ ":/resource/img/basic/chat/chat_word_sticker_error_icon.svg", 1, "sticker" },
	{ ":/resource/img/basic/chat/chat_word_send_sticker_error_icon.png", 1, "sticker" },
	{ ":/resource/img/basic/chat/chat_word_receive_sticker_error_icon.png", 1, "sticker" },
	// Chat header buttons LINE makes from an LcImage with a pixmap and no source (paintedIcon();
	// the names are iconLabels() keys).
	{ ":/resource/img/basic/chat/chatroom-call.png", 1, "chatroom-call" },
	{ ":/resource/img/basic/chat/chatroom-call-solid.svg", 1, "chatroom-call" },
	{ ":/resource/img/basic/chat/chatroom-video-call-solid.svg", 1, "chatroom-video-call" },
	// The reaction button LINE draws beside a message while the mouse is over it.
	{ ":/resource/img/basic/chat/reaction/ic-reaction-off.svg", 1, "reaction-button" },
	// Drawn instead when the user has reacted to the message.
	{ ":/resource/img/basic/chat/reaction/ic-reaction-on.svg", 1, "reaction-button-on" },
	// The reactions in LINE's reaction picker (ReactionDockWidget; names are iconLabels() keys).
	{ ":/resource/img/basic/chat/reaction/1001_icon.png", 1, "reaction-love", true },
	{ ":/resource/img/basic/chat/reaction/1002_icon.png", 1, "reaction-fun", true },
	{ ":/resource/img/basic/chat/reaction/1003_icon.png", 1, "reaction-like", true },
	{ ":/resource/img/basic/chat/reaction/1004_icon.png", 1, "reaction-envy", true },
	{ ":/resource/img/basic/chat/reaction/1005_icon.png", 1, "reaction-surprise", true },
	{ ":/resource/img/basic/chat/reaction/1006_icon.png", 1, "reaction-sad", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-more.png", 1, "reaction-more", true },
	// The button on LINE's preview of a sticker (sticker window, mouse over a sticker): the kind of
	// sticker, a click plays its sound; a message sticker has an edit button instead.
	{ ":/resource/img/basic/sticker/sound_type_icon.svg", 1, "stype-sound" },
	{ ":/resource/img/basic/sticker/animation_sound_type_icon.svg", 1, "stype-animation-sound" },
	{ ":/resource/img/basic/sticker/popup_sound_type_icon.svg", 1, "stype-popup-sound" },
	{ ":/resource/img/basic/sticker/effect_sound_type_icon.svg", 1, "stype-effect-sound" },
	{ ":/resource/img/basic/sticker/popup_type_icon.svg", 1, "stype-popup" },
	{ ":/resource/img/basic/sticker/effect_type_icon.svg", 1, "stype-effect" },
	{ ":/resource/img/basic/sticker/text_type_icon.svg", 1, "stype-text" },
	{ ":/resource/img/basic/sticker/message-sticker-edit-ic.svg", 1, "stype-edit" },
	{ ":/resource/img/basic/sticker/message-sticker-edit-ic-dark.svg", 1, "stype-edit" },
	// The reactions drawn under a message (18 px at 150 %; matched in colour, recordImage()).
	{ ":/resource/img/basic/chat/reaction/ic-reaction-like.png", 1, "badge-like", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-like-20.png", 1, "badge-like", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-like-24.png", 1, "badge-like", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-like-40.png", 1, "badge-like", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-love.png", 1, "badge-love", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-love-20.png", 1, "badge-love", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-love-24.png", 1, "badge-love", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-love-40.png", 1, "badge-love", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-fun.png", 1, "badge-fun", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-fun-20.png", 1, "badge-fun", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-fun-24.png", 1, "badge-fun", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-fun-40.png", 1, "badge-fun", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-envy.png", 1, "badge-envy", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-envy-20.png", 1, "badge-envy", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-envy-24.png", 1, "badge-envy", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-envy-40.png", 1, "badge-envy", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-sad.png", 1, "badge-sad", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-sad-20.png", 1, "badge-sad", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-sad-24.png", 1, "badge-sad", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-sad-40.png", 1, "badge-sad", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-surprise.png", 1, "badge-surprise", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-surprise-20.png", 1, "badge-surprise", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-surprise-24.png", 1, "badge-surprise", true },
	{ ":/resource/img/basic/chat/reaction/ic-reaction-surprise-40.png", 1, "badge-surprise", true },
};

struct IconMask {
	QByteArray alpha; // kMaskSide x kMaskSide; RGB (3 bytes a pixel, over black) for colour icons
	qreal aspect = 1;
	const char *name = nullptr;
	bool color = false;
};

QByteArray alphaMask(const QImage &image)
{
	const QImage small = image.scaled(kMaskSide, kMaskSide, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
	                         .convertToFormat(QImage::Format_Alpha8);
	QByteArray mask;
	mask.reserve(kMaskSide * kMaskSide);
	for (int y = 0; y < kMaskSide; ++y)
		mask.append(reinterpret_cast<const char *>(small.constScanLine(y)), kMaskSide);
	return mask;
}

QByteArray colorMask(const QImage &image)
{
	// Over black: the faces are white with dark outlines, and LINE's pixmaps may come pre-composited
	// on black.
	QImage flat(image.size(), QImage::Format_RGB32);
	flat.fill(Qt::black);
	{
		// Pixel for pixel: a grab at 150 % has device pixel ratio 1.5 and would be drawn smaller.
		QImage source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
		source.setDevicePixelRatio(1);
		QPainter p(&flat);
		p.drawImage(0, 0, source);
	}
	const QImage small = flat.scaled(kMaskSide, kMaskSide, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
	                         .convertToFormat(QImage::Format_RGB888);
	QByteArray mask;
	mask.reserve(kMaskSide * kMaskSide * 3);
	for (int y = 0; y < kMaskSide; ++y)
		mask.append(reinterpret_cast<const char *>(small.constScanLine(y)), kMaskSide * 3);
	return mask;
}

const QList<IconMask> &iconMasks()
{
	static const QList<IconMask> masks = [] {
		QList<IconMask> list;
		for (const KnownIcon &icon : kKnownIcons) {
			const QImage sprite(QString::fromLatin1(icon.path));
			if (sprite.isNull()) {
				a11yLog("[icons] %s not found", icon.path);
				continue;
			}
			const int w = sprite.width() / icon.slices;
			for (int i = 0; i < icon.slices; ++i) {
				const QImage slice = sprite.copy(i * w, 0, w, sprite.height());
				list.append({ icon.color ? colorMask(slice) : alphaMask(slice), qreal(w) / sprite.height(), icon.name, icon.color });
			}
		}
		return list;
	}();
	return masks;
}

// The known icon `image` shows, or nullptr. Results are kept per image (LINE draws the same
// pixmaps again and again).
const char *knownIcon(qint64 key, const QImage &image)
{
	static QHash<qint64, const char *> cache;
	if (const auto it = cache.constFind(key); key && it != cache.constEnd())
		return it.value();
	const char *found = nullptr;
	if (!image.isNull()) {
		const QByteArray mask = alphaMask(image);
		const qreal aspect = qreal(image.width()) / image.height();
		int best = kMaxMaskDifference + 1;
		for (const IconMask &known : iconMasks()) {
			if (qAbs(aspect - known.aspect) > known.aspect * 0.15)
				continue;
			// Colour icons are matched on rendered widgets only (reactionIcon()): LINE hands the
			// picker's images to the painter in another form than the screen shows.
			if (known.color)
				continue;
			int sum = 0;
			for (int i = 0; i < mask.size(); ++i)
				sum += qAbs(int(quint8(mask[i])) - int(quint8(known.alpha[i])));
			const int mean = sum / int(mask.size());
			if (mean < best) {
				best = mean;
				found = known.name;
			}
		}
	}
	if (cache.size() > 4000)
		cache.clear();
	if (key)
		cache.insert(key, found);
	return found;
}

} // namespace

const char *colorIconName(const QImage &rendered, const char *prefix);

namespace {

// Which reaction face a small round picture is ("badge-like", ...), or nullptr; kept per image.
const char *reactionBadge(qint64 key, const QImage &image)
{
	static QHash<qint64, const char *> cache;
	if (const auto it = cache.constFind(key); key && it != cache.constEnd())
		return it.value();
	const char *found = colorIconName(image, "badge-");
	if (cache.size() > 4000)
		cache.clear();
	if (key)
		cache.insert(key, found);
	return found;
}

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
		if (dirty & QPaintEngine::DirtyBrush)
			m_brush = newState.brush();
		if (dirty & QPaintEngine::DirtyPen)
			m_penAlpha = newState.pen().color().alpha();
		if (dirty & (QPaintEngine::DirtyClipEnabled | QPaintEngine::DirtyClipPath | QPaintEngine::DirtyClipRegion))
			m_clipped = newState.isClipEnabled() && newState.clipOperation() != Qt::NoClip;
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
		const qreal width = QFontMetricsF(ti.font()).horizontalAdvance(text) * m_transform.m11();
		m_out->items.push_back({ m_transform.map(p), text, width, ti.font(), m_transform.m11() });
	}

	void drawPixmap(const QRectF &r, const QPixmap &pm, const QRectF &) override
	{
		recordPixmap(r);
		recordImage(r, pm.size(), pm.hasAlphaChannel(), pm.cacheKey(), [&pm] { return pm.toImage(); });
	}
	void drawImage(const QRectF &r, const QImage &image, const QRectF &, Qt::ImageConversionFlags) override
	{
		recordPixmap(r);
		recordImage(r, image.size(), image.hasAlphaChannel(), image.cacheKey(), [&image] { return image; });
	}
	void drawTiledPixmap(const QRectF &, const QPixmap &, const QPointF &) override { ++m_out->pixmaps; }
	void drawPolygon(const QPointF *, int, PolygonDrawMode) override {}
	void drawPolygon(const QPoint *, int, PolygonDrawMode) override {}
	void drawRects(const QRectF *rects, int n) override
	{
		for (int i = 0; i < n; ++i)
			recordShape(rects[i]);
	}
	void drawRects(const QRect *rects, int n) override
	{
		for (int i = 0; i < n; ++i)
			recordShape(rects[i]);
	}
	void drawLines(const QLineF *, int) override {}
	void drawLines(const QLine *, int) override {}
	void drawEllipse(const QRectF &r) override { recordShape(r); }
	void drawEllipse(const QRect &r) override { recordShape(r); }
	void drawPath(const QPainterPath &path) override { recordShape(path.boundingRect()); }
	void drawPoints(const QPointF *, int) override {}
	void drawPoints(const QPoint *, int) override {}

private:
	void recordPixmap(const QRectF &r)
	{
		++m_out->pixmaps;
		m_out->pixmapRects << m_transform.mapRect(r);
	}

	template <typename ToImage>
	void recordImage(const QRectF &r, const QSize &size, bool alpha, qint64 key, ToImage toImage)
	{
		if (m_opacity <= 0.01)
			return;
		PaintedImage pi;
		pi.rect = m_transform.mapRect(r);
		pi.size = size;
		pi.alpha = alpha;
		pi.clipped = m_clipped;
		if (alpha && size.width() > 2 && size.height() > 2) {
			const QImage image = toImage();
			const int w = image.width() - 1, h = image.height() - 1;
			pi.clearCorners = qAlpha(image.pixel(0, 0)) < 16 && qAlpha(image.pixel(w, 0)) < 16
			               && qAlpha(image.pixel(0, h)) < 16 && qAlpha(image.pixel(w, h)) < 16;
			// Icons only: pictures are larger.
			if (size.width() <= 96 && size.height() <= 96 && size.width() >= 8 && size.height() >= 8)
				if (const char *icon = knownIcon(key, image))
					pi.icon = QLatin1String(icon);
			// Badges are told apart by colour (chat list: blue pin vs green OpenChat badge).
			if (size.width() <= 48 && size.height() <= 48) {
				long r = 0, g = 0, b = 0, n = 0;
				for (int y = 0; y < image.height(); ++y) {
					for (int x = 0; x < image.width(); ++x) {
						const QRgb p = image.pixel(x, y);
						if (qAlpha(p) > 200) {
							r += qRed(p);
							g += qGreen(p);
							b += qBlue(p);
							++n;
						}
					}
				}
				if (n)
					pi.average = QColor(int(r / n), int(g / n), int(b / n));
			}
			// A reaction under a message: a small round face, told apart by its colours only.
			if (size.width() >= 12 && size.width() <= 64 && qAbs(size.width() - size.height()) <= 2)
				if (const char *badge = reactionBadge(key, image))
					pi.icon = QLatin1String(badge);
		}
		m_out->images << pi;
	}

	void recordShape(const QRectF &r)
	{
		if (m_opacity <= 0.01)
			return;
		PaintedShape s;
		s.rect = m_transform.mapRect(r);
		s.brush = int(m_brush.style());
		if (m_brush.style() == Qt::TexturePattern)
			s.texture = m_brush.textureImage().isNull() ? m_brush.texture().size() : m_brush.textureImage().size();
		m_out->shapes << s;
	}

	CaptureResult *m_out;
	QTransform m_transform;
	qreal m_opacity = 1;
	int m_penAlpha = 255;
	bool m_clipped = false;
	QBrush m_brush;
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
	// neighbouring rows may still draw: keep what lies inside `rect`. LINE draws a row whole even when
	// part of it is scrolled out (a message taller than the list), so that part is kept too.
	breadcrumb("render region of %s at %d,%d %dx%d", w->metaObject()->className(), area.x(), area.y(), area.width(),
	           area.height());
	checkHeap("before", w);
	w->render(&dev, QPoint(), QRegion(area), QWidget::DrawChildren);
	checkHeap("after", w);
	breadcrumb("render done");
	// From the rendered area's coordinates to rect's.
	const QPointF offset = area.topLeft() - rect.topLeft();
	auto inside = [&rect](const QRectF &r) { return r.bottom() > 0 && r.top() < rect.height(); };
	QList<CapturedText> itemsInside;
	for (CapturedText it : std::as_const(result.items)) {
		it.pos += offset;
		if (it.pos.y() > 0 && it.pos.y() <= rect.height() + 2)
			itemsInside << it;
	}
	result.items = itemsInside;
	QList<QRectF> pixmapsInside;
	for (const QRectF &r : std::as_const(result.pixmapRects)) {
		if (inside(r.translated(offset)))
			pixmapsInside << r.translated(offset);
	}
	result.pixmapRects = pixmapsInside;
	result.pixmaps = int(pixmapsInside.size());
	QList<PaintedImage> imagesInside;
	for (PaintedImage pi : std::as_const(result.images)) {
		pi.rect.translate(offset);
		if (inside(pi.rect))
			imagesInside << pi;
	}
	result.images = imagesInside;
	QList<PaintedShape> shapesInside;
	for (PaintedShape s : std::as_const(result.shapes)) {
		s.rect.translate(offset);
		if (inside(s.rect))
			shapesInside << s;
	}
	result.shapes = shapesInside;
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

QString reactionIcon(const QImage &rendered)
{
	return colorIcon(rendered, "reaction-");
}

QString colorIcon(const QImage &rendered, const char *prefix)
{
	const char *found = colorIconName(rendered, prefix);
	return found ? QLatin1String(found) : QString();
}

const char *colorIconName(const QImage &rendered, const char *prefix)
{
	if (rendered.isNull())
		return nullptr;
	const QByteArray mask = colorMask(rendered);
	const qreal aspect = qreal(rendered.width()) / rendered.height();
	const char *found = nullptr;
	int best = kMaxMaskDifference + 1;
	for (const IconMask &known : iconMasks()) {
		if (!known.color || qAbs(aspect - known.aspect) > known.aspect * 0.15
		    || !QLatin1StringView(known.name).startsWith(QLatin1StringView(prefix)))
			continue;
		int sum = 0;
		for (int i = 0; i < mask.size(); ++i)
			sum += qAbs(int(quint8(mask[i])) - int(quint8(known.alpha[i])));
		const int mean = sum / int(mask.size());
		if (mean < best) {
			best = mean;
			found = known.name;
		}
	}
	return found;
}

} // namespace line_a11y

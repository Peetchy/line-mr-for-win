// Recovers the text a widget draws by rendering it into a paint device whose engine records
// every text item instead of rasterising it.
//
// LINE's LcText has no text property; on macOS its text was read through reverse-engineered
// offsets. Recording QPainter text calls needs no offsets and survives LINE updates, as long
// as LINE draws text through QPainter (and not from pre-rendered pixmaps).

#pragma once

#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QRect>
#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtCore/QString>

class QImage;
class QWidget;

namespace line_a11y {

struct CapturedText {
	QPointF pos; // baseline origin, in the rendered widget's coordinates
	QString text;
	qreal width = 0; // advance of the text, in the same coordinates
};

// An image or pixmap the widget drew.
struct PaintedImage {
	QRectF rect;          // where (device coordinates)
	QSize size;           // the image's own size
	bool alpha = false;   // has an alpha channel
	bool clearCorners = false; // its four corner pixels are transparent (stickers, round icons)
	bool clipped = false; // drawn with a clip set (LINE rounds photo corners that way)
	QString icon;         // which of LINE's known icons it is ("voice", see text_capture.cpp), or empty
};

// A path, rectangle or ellipse the widget drew.
struct PaintedShape {
	QRectF rect;   // bounding rectangle (device coordinates)
	int brush = 0; // Qt::BrushStyle it was filled with
	QSize texture; // size of the brush's image (pictures drawn as a filled shape)
};

struct CaptureResult {
	QList<CapturedText> items;
	int glyphRunsWithoutText = 0; // text drawn from glyph indexes only (not recoverable here)
	int pixmaps = 0;              // pixmaps/images drawn (text may be baked into them)
	QList<QRectF> pixmapRects;    // where they were drawn (device coordinates)
	QList<PaintedImage> images;   // details of the same images, in paint order
	QList<PaintedShape> shapes;   // paths / rectangles / ellipses drawn (bubbles, badges)
	bool skipped = false;         // not rendered (LINE was painting); use an earlier result
};

// Renders `w` (and, if `children`, its children) and returns the text items in paint order.
CaptureResult captureWidgetText(QWidget *w, bool children);

// Renders only `rect` (in w's coordinates; e.g. one row of a list viewport) and returns the text
// items whose baseline lies inside it, with positions relative to rect.topLeft().
CaptureResult captureRegionText(QWidget *w, const QRect &rect);

// Safe mode (set after repeated crashes): captureRegionText() renders nothing.
void setSafeMode(bool on);

// The captured items joined in reading order (top-to-bottom, then left-to-right).
QString capturedTextInReadingOrder(const CaptureResult &r);

// Which of LINE's reaction faces a rendered picture (QWidget::grab() of a reaction picker choice)
// shows: "reaction-love", "reaction-fun", ..., or "" (compared by colour, see text_capture.cpp).
QString reactionIcon(const QImage &rendered);

// The same for any colour icon whose name starts with `prefix` ("reaction-", "badge-").
QString colorIcon(const QImage &rendered, const char *prefix);

} // namespace line_a11y

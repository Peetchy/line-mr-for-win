#pragma once

#include "text_capture.h"
#include <QtCore/QRegularExpression>

namespace line_a11y {

// Some LINE paint paths draw the bubble as nine adjacent image slices. No single
// slice then contains both the play icon and the duration. Reconstruct only small,
// connected, transparent background regions; avatars/icons and photo clips are excluded.
inline QRectF tiledVoiceBubble(const CaptureResult &cap, const QRectF &play, qreal rowWidth)
{
	QList<QRectF> tiles;
	for (const PaintedImage &image : cap.images) {
		if (image.icon.isEmpty() && image.alpha && !image.clipped && image.rect.top() >= -4
		    && image.rect.width() > 0 && image.rect.width() < rowWidth - 20
		    && image.rect.height() > 0 && image.rect.height() < 80)
			tiles << image.rect;
	}
	static const QRegularExpression duration(QStringLiteral("^\\d{1,2}:\\d{2}$"));
	while (!tiles.isEmpty()) {
		QList<QRectF> component { tiles.takeFirst() };
		QRectF bounds = component.first();
		for (qsizetype i = 0; i < component.size(); ++i) {
			for (qsizetype j = tiles.size(); j-- > 0;) {
				if (component[i].adjusted(-1, -1, 1, 1).intersects(tiles[j])) {
					bounds = bounds.united(tiles[j]);
					component << tiles.takeAt(j);
				}
			}
		}
		if (component.size() < 2 || bounds.height() >= 80 || bounds.width() <= play.width() * 2
		    || bounds.width() >= rowWidth - 20 || !bounds.contains(play))
			continue;
		for (const CapturedText &text : cap.items) {
			if (bounds.adjusted(-2, -2, 2, 2).contains(text.pos)
			    && duration.match(text.text.trimmed()).hasMatch())
				return bounds;
		}
	}
	return {};
}

} // namespace line_a11y

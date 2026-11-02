#pragma once

#include "text_capture.h"
#include <cmath>

namespace line_a11y {

enum class MessageDirection { Unknown, Incoming, Outgoing };

// LINE's nine-slice bubble has its wider corner on the tail side. Unlike text indent,
// this distinguishes a wide sent bubble even when its time/receipt is not painted.
inline MessageDirection bubbleDirection(const CaptureResult &cap)
{
	auto near = [](qreal a, qreal b) { return std::abs(a - b) <= 1; };
	auto tile = [&](qreal x, qreal y, qreal width, qreal height) {
		for (const PaintedImage &image : cap.images)
			if (image.alpha && near(image.rect.x(), x) && near(image.rect.y(), y)
			    && near(image.rect.width(), width) && near(image.rect.height(), height))
				return true;
		return false;
	};
	MessageDirection result = MessageDirection::Unknown;
	for (const PaintedImage &left : cap.images) {
		const QRectF a = left.rect;
		if (!left.alpha || a.width() < 10 || a.width() > 26 || a.height() < 10 || a.height() > 24)
			continue;
		for (const PaintedImage &middle : cap.images) {
			const QRectF b = middle.rect;
			if (!middle.alpha || b.width() < 16 || !near(b.x(), a.right())
			    || !near(b.y(), a.y()) || !near(b.height(), a.height()))
				continue;
			for (const PaintedImage &right : cap.images) {
				const QRectF c = right.rect;
				if (!right.alpha || c.width() < 10 || c.width() > 26 || !near(c.x(), b.right())
				    || !near(c.y(), a.y()) || !near(c.height(), a.height()) || std::abs(a.width() - c.width()) < 4)
					continue;
				// Require all nine tiles, not an arbitrary run of icons or a quoted avatar.
				for (const PaintedImage &centre : cap.images) {
					const QRectF d = centre.rect;
					if (!centre.alpha || d.height() <= 0 || !near(d.x(), b.x()) || !near(d.y(), b.bottom())
					    || !near(d.width(), b.width()))
						continue;
					if (!tile(a.x(), d.y(), a.width(), d.height()) || !tile(c.x(), d.y(), c.width(), d.height())
					    || !tile(a.x(), d.bottom(), a.width(), a.height())
					    || !tile(b.x(), d.bottom(), b.width(), b.height())
					    || !tile(c.x(), d.bottom(), c.width(), c.height()))
						continue;
					const auto direction = a.width() > c.width() ? MessageDirection::Incoming : MessageDirection::Outgoing;
					if (result != MessageDirection::Unknown && result != direction)
						return MessageDirection::Unknown;
					result = direction;
				}
			}
		}
	}
	return result;
}

} // namespace line_a11y

#include "voice_bubble.h"
#include <cstdio>

int main()
{
	using namespace line_a11y;
	CaptureResult cap;
	// Coordinates from the failing 00:15 voice message, without private chat text.
	for (int y : { 5, 20, 40 }) {
		const int h = y == 20 ? 20 : 15;
		for (const QRectF &r : { QRectF(52, y, 22, h), QRectF(74, y, 96, h), QRectF(170, y, 13, h) }) {
			PaintedImage tile;
			tile.rect = r;
			tile.alpha = true;
			cap.images << tile;
		}
	}
	CapturedText duration;
	duration.pos = QPointF(143, 35);
	duration.text = QStringLiteral("00:15");
	cap.items << duration;
	const QRectF play(72, 21, 19, 18);
	auto check = [](bool condition, const char *description) {
		if (!condition)
			std::fprintf(stderr, "Failed: %s\n", description);
		return condition ? 0 : 1;
	};
	int failures = check(tiledVoiceBubble(cap, play, 368) == QRectF(52, 5, 131, 50), "nine-slice bubble");
	CaptureResult noDuration = cap;
	noDuration.items.clear();
	failures += check(tiledVoiceBubble(noDuration, play, 368).isEmpty(), "no duration");
	CaptureResult outside = cap;
	outside.items[0].pos = QPointF(190, 51);
	failures += check(tiledVoiceBubble(outside, play, 368).isEmpty(), "timestamp outside bubble");
	CaptureResult clipped = cap;
	for (auto &image : clipped.images)
		image.clipped = true;
	failures += check(tiledVoiceBubble(clipped, play, 368).isEmpty(), "clipped video thumbnail");
	CaptureResult opaque = cap;
	for (auto &image : opaque.images)
		image.alpha = false;
	failures += check(tiledVoiceBubble(opaque, play, 368).isEmpty(), "opaque photo");
	failures += check(tiledVoiceBubble(cap, QRectF(200, 21, 19, 18), 368).isEmpty(), "unrelated play icon");
	failures += check(tiledVoiceBubble(cap, play, 140).isEmpty(), "row-wide background");
	return failures ? 1 : 0;
}

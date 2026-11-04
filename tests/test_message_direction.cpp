#include "message_direction.h"
#include <cstdio>

using namespace line_a11y;

CaptureResult bubble(int x, int y, int left, int centre, int right, int middleHeight)
{
	CaptureResult cap;
	int py = y;
	for (int height : {15, middleHeight, 15}) {
		int px = x;
		for (int width : {left, centre, right}) {
			PaintedImage image;
			image.rect = QRectF(px, py, width, height);
			image.alpha = true;
			cap.images << image;
			px += width;
		}
		py += height;
	}
	return cap;
}

int main()
{
	int failures = 0;
	auto check = [&](bool condition, const char *description) {
		if (!condition) { ++failures; std::fprintf(stderr, "Failed: %s\n", description); }
	};
	// Measured from the failing long sent message: no time/receipt and text starts at x=93
	// in a 368-wide row. The old indent rule called this incoming.
	check(bubbleDirection(bubble(81, 0, 12, 245, 22, 20)) == MessageDirection::Outgoing, "long sent without time");
	check(bubbleDirection(bubble(276, 0, 12, 50, 22, 2)) == MessageDirection::Outgoing, "short sent without time");
	check(bubbleDirection(bubble(55, 5, 22, 47, 13, 2)) == MessageDirection::Incoming, "received bubble");
	check(bubbleDirection(bubble(55, 0, 22, 245, 13, 20)) == MessageDirection::Incoming, "long received without avatar");
	check(bubbleDirection(bubble(100, 0, 15, 80, 15, 2)) == MessageDirection::Unknown, "symmetric notice");
	auto partial = bubble(81, 0, 12, 245, 22, 20);
	partial.images.removeLast();
	check(bubbleDirection(partial) == MessageDirection::Unknown, "incomplete bubble");
	auto conflicting = bubble(81, 0, 12, 245, 22, 20);
	conflicting.images.append(bubble(55, 70, 22, 47, 13, 2).images);
	check(bubbleDirection(conflicting) == MessageDirection::Unknown, "conflicting bubble tails");
	check(bubbleDirection(CaptureResult()) == MessageDirection::Unknown, "no evidence");
	return failures ? 1 : 0;
}

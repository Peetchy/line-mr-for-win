// nativeMessageText() (LINE's own message text) and withMessageParts() (what the bridge adds).
#include "native_message_text.h"

#include <QtGui/QGuiApplication>
#include <cstdio>

using line_a11y::MessageRowParts;

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	int failures = 0;
	const auto expect = [&failures](const QString &got, const QString &wanted, const char *name) {
		if (got != wanted) {
			std::fprintf(stderr, "FAIL: %s\n  got:    %s\n  wanted: %s\n", name, qUtf8Printable(got), qUtf8Printable(wanted));
			++failures;
		}
	};
	using line_a11y::nativeMessageText;
	// Contributed by wengweng0312 (PR #18).
	expect(nativeMessageText(QString::fromUtf8("你, 收到，謝謝, 已讀 2, 下午 10:23")),
	       QString::fromUtf8("你, 收到，謝謝, 已讀 2, 下午 10:23"), "native status remains intact");
	expect(nativeMessageText(QString::fromUtf8("回覆甲的訊息。原始訊息：<a href='line://nv/profilePopup/mid=test'>@乙</a> 請確認。回覆內容：收到。")),
	       QString::fromUtf8("回覆甲的訊息。原始訊息：@乙 請確認。回覆內容：收到。"), "rich link after plain prefix");
	expect(nativeMessageText(QStringLiteral("A &amp; B &lt;tag&gt; &#x1F600;")), QString::fromUtf8("A & B <tag> 😀"),
	       "entities and literal angle brackets");
	expect(nativeMessageText(QStringLiteral("1 < 2 > 0")), QStringLiteral("1 < 2 > 0"), "ordinary comparisons");
	expect(nativeMessageText(QStringLiteral("<p>first</p><p>second<br>third</p>")), QStringLiteral("first second third"),
	       "block and line boundaries");
	const QString longText = QString::fromUtf8("完整正文").repeated(500) + QString::fromUtf8("，已讀 3，下午 10:23");
	expect(nativeMessageText(longText), longText, "no truncation");
	expect(nativeMessageText(QStringLiteral("  \n\t ")), QString(), "empty text falls back");
	expect(nativeMessageText(QVariant()), QString(), "invalid role falls back");
	expect(nativeMessageText(QVariant(42)), QString(), "non-string role falls back");

	expect(nativeMessageText(QString::fromUtf8("ข้อความวิดีโอ, ความยาววิดีโอ 241 วินาที, 3B1141DE80FFFB1A72DB815C7274C77B90777D5C.mp4, 7.57 น.")),
	       QString::fromUtf8("ข้อความวิดีโอ, ความยาววิดีโอ 241 วินาที, 7.57 น."), "hash file name dropped");
	expect(nativeMessageText(QString::fromUtf8("ไฟล์, WCAG Training.pdf, 20.38 น.")),
	       QString::fromUtf8("ไฟล์, WCAG Training.pdf, 20.38 น."), "real file name kept");

	// What LINE 26.6 (Thai) gave, with the bridge's parts.
	using line_a11y::withMessageParts;
	const QString you = QString::fromUtf8("คุณ");
	MessageRowParts none;
	expect(withMessageParts(QString::fromUtf8("Moss, โดนยกตัวอย่าง, 13.50 น."), none, you),
	       QString::fromUtf8("Moss, โดนยกตัวอย่าง, 13.50 น."), "off screen: LINE's text as it is");
	MessageRowParts own;
	own.captured = own.own = true;
	expect(withMessageParts(QString::fromUtf8("แต่ผมค่องใจเรื่องนึงครับ, 17.17 น., อ่านแล้ว"), own, you),
	       QString::fromUtf8("คุณ, แต่ผมค่องใจเรื่องนึงครับ, 17.17 น., อ่านแล้ว"), "own message");
	MessageRowParts sticker = own;
	sticker.kind = QString::fromUtf8("สติกเกอร์");
	expect(withMessageParts(QString::fromUtf8(", 15.29 น."), sticker, you),
	       QString::fromUtf8("คุณ, สติกเกอร์, 15.29 น."), "own sticker (LINE: no body)");
	MessageRowParts received;
	received.captured = true;
	received.kind = QString::fromUtf8("สติกเกอร์");
	expect(withMessageParts(QString::fromUtf8("Moss, , 9.00 น."), received, you),
	       QString::fromUtf8("Moss, สติกเกอร์, 9.00 น."), "received sticker");
	MessageRowParts photo = received;
	photo.kind = QString::fromUtf8("รูปภาพ");
	expect(withMessageParts(QString::fromUtf8("Moss, รูป, 13.55 น."), photo, you),
	       QString::fromUtf8("Moss, รูป, 13.55 น."), "LINE names the kind: kept");
	MessageRowParts voice = received;
	voice.kind = QString::fromUtf8("ข้อความเสียง");
	voice.lengths = QStringList { QStringLiteral("00:04") };
	expect(withMessageParts(QString::fromUtf8(", 9.00 น."), voice, you),
	       QString::fromUtf8("ข้อความเสียง, 00:04, 9.00 น."), "length after the kind");
	MessageRowParts call = received;
	call.kind = QString::fromUtf8("การโทร");
	call.lengths = QStringList { QStringLiteral("00:07") };
	call.reactions = QString::fromUtf8("ความรู้สึก: ถูกใจ");
	expect(withMessageParts(QString::fromUtf8("00:07, 20.49 น."), call, you),
	       QString::fromUtf8("การโทร, 00:07, 20.49 น., ความรู้สึก: ถูกใจ"), "call: LINE gives only the length");
	const auto seconds = [](const QString &clock) {
		return clock == QStringLiteral("00:07") ? QString::fromUtf8("7 วินาที") : clock;
	};
	expect(withMessageParts(QString::fromUtf8("00:07, 20.49 น."), call, you, seconds),
	       QString::fromUtf8("การโทร, 7 วินาที, 20.49 น., ความรู้สึก: ถูกใจ"), "call length in seconds");
	MessageRowParts voiceClock = voice;
	voiceClock.lengths = QStringList { QStringLiteral("00:07") };
	expect(withMessageParts(QString::fromUtf8(", 9.00 น."), voiceClock, you, seconds),
	       QString::fromUtf8("ข้อความเสียง, 7 วินาที, 9.00 น."), "voice length in seconds");
	MessageRowParts video = received;
	video.kind = QString::fromUtf8("วิดีโอ");
	video.lengths = QStringList { QStringLiteral("04:01") };
	expect(withMessageParts(QString::fromUtf8("ข้อความวิดีโอ, ความยาววิดีโอ 241 วินาที, 15.02 น."), video, you, seconds),
	       QString::fromUtf8("ข้อความวิดีโอ, ความยาววิดีโอ 241 วินาที, 15.02 น."), "LINE's video length kept");
	MessageRowParts voiceNamed = voice;
	expect(withMessageParts(QString::fromUtf8("ข้อความเสียง, ข้อความเสียงยาว 7 วินาที, 10.15 น."), voiceNamed, you),
	       QString::fromUtf8("ข้อความเสียง, ข้อความเสียงยาว 7 วินาที, 10.15 น."), "voice: LINE names it");
	MessageRowParts reacted = received;
	reacted.kind.clear();
	reacted.reactions = QString::fromUtf8("ความรู้สึก: ถูกใจ");
	expect(withMessageParts(QString::fromUtf8("Moss, ค่ะ, 9.00 น."), reacted, you),
	       QString::fromUtf8("Moss, ค่ะ, 9.00 น., ความรู้สึก: ถูกใจ"), "reactions at the end");
	MessageRowParts direct = received;
	direct.kind.clear();
	direct.sender = QStringLiteral("Aew Sirilak");
	expect(withMessageParts(QString::fromUtf8("ไม่อยากนัดละ, 18.00 น."), direct, you),
	       QString::fromUtf8("Aew Sirilak, ไม่อยากนัดละ, 18.00 น."), "one-to-one sender name option");
	expect(withMessageParts(QString::fromUtf8("Aew Sirilak, ไม่อยากนัดละ, 18.00 น."), direct, you),
	       QString::fromUtf8("Aew Sirilak, ไม่อยากนัดละ, 18.00 น."), "sender name not repeated");
	expect(withMessageParts(QString::fromUtf8("คุณ, ok, 9.00 น."), own, you), QString::fromUtf8("คุณ, ok, 9.00 น."),
	       "You not repeated");
	return failures ? 1 : 0;
}

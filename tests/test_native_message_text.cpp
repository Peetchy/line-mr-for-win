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
	line_a11y::MessageLabels you;
	you.you = QString::fromUtf8("คุณ");
	you.replyTo = QString::fromUtf8("ตอบกลับ %1: %2");
	you.original = QString::fromUtf8("ข้อความเดิม: %1");
	you.kindsToName = QStringList { QString::fromUtf8("ตำแหน่งที่ตั้ง"), QString::fromUtf8("รายชื่อติดต่อ") };
	you.album = QString::fromUtf8("อัลบั้มรูปภาพ");
	you.photoWords = QStringList { QString::fromUtf8("รูป") };
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
	line_a11y::MessageLabels seconds = you;
	seconds.duration = [](const QString &clock) {
		return clock == QStringLiteral("00:07") ? QString::fromUtf8("7 วินาที") : clock;
	};
	expect(withMessageParts(QString::fromUtf8("00:07, 20.49 น."), call, seconds),
	       QString::fromUtf8("การโทร, 7 วินาที, 20.49 น., ความรู้สึก: ถูกใจ"), "call length in seconds");
	MessageRowParts voiceClock = voice;
	voiceClock.lengths = QStringList { QStringLiteral("00:07") };
	expect(withMessageParts(QString::fromUtf8(", 9.00 น."), voiceClock, seconds),
	       QString::fromUtf8("ข้อความเสียง, 7 วินาที, 9.00 น."), "voice length in seconds");
	MessageRowParts video = received;
	video.kind = QString::fromUtf8("วิดีโอ");
	video.lengths = QStringList { QStringLiteral("04:01") };
	expect(withMessageParts(QString::fromUtf8("ข้อความวิดีโอ, ความยาววิดีโอ 241 วินาที, 15.02 น."), video, seconds),
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

	// What the bridge adds (2026-11-05, Keep Memo and "Digital Acessibility", Thai).
	MessageRowParts file = own;
	file.kind = QString::fromUtf8("ไฟล์");
	file.details = QStringList { QString::fromUtf8("ใบรับรองแพทย์.pdf"), QString::fromUtf8("หมดเวลาเก็บไฟล์"), QString::fromUtf8("ขนาด 2.6MB") };
	expect(withMessageParts(QString::fromUtf8("ไฟล์, หมดเวลาเก็บไฟล์, ใบรับรองแพทย์.pdf, 5.11 น."), file, you),
	       QString::fromUtf8("คุณ, ไฟล์, หมดเวลาเก็บไฟล์, ใบรับรองแพทย์.pdf, ขนาด 2.6MB, 5.11 น."), "file size");
	MessageRowParts link = own;
	link.details = QStringList { QStringLiteral("Facebook") };
	expect(withMessageParts(QStringLiteral("https://www.facebook.com/share/p/1JrLE3Sy9j/?mibextid=wwXIfr, 9.18 น."), link, you),
	       QString::fromUtf8("คุณ, https://www.facebook.com/share/p/1JrLE3Sy9j/?mibextid=wwXIfr, Facebook, 9.18 น."), "link title");
	MessageRowParts location = own;
	location.kind = QString::fromUtf8("ตำแหน่งที่ตั้ง");
	expect(withMessageParts(QString::fromUtf8("หมู่บ้าน A Tambon Khlong Nung, Thailand, 10.20 น."), location, you),
	       QString::fromUtf8("คุณ, ตำแหน่งที่ตั้ง, หมู่บ้าน A Tambon Khlong Nung, Thailand, 10.20 น."), "own location");
	MessageRowParts receivedLocation = received;
	receivedLocation.kind = QString::fromUtf8("ตำแหน่งที่ตั้ง");
	expect(withMessageParts(QString::fromUtf8("Moss, หมู่บ้าน A, Thailand, 10.20 น."), receivedLocation, you),
	       QString::fromUtf8("Moss, ตำแหน่งที่ตั้ง, หมู่บ้าน A, Thailand, 10.20 น."), "received location");
	MessageRowParts contact = own;
	contact.kind = QString::fromUtf8("รายชื่อติดต่อ");
	contact.details = QStringList { QString::fromUtf8("arm🦾C-m") };
	expect(withMessageParts(QString::fromUtf8(", 10.21 น."), contact, you),
	       QString::fromUtf8("คุณ, รายชื่อติดต่อ, arm🦾C-m, 10.21 น."), "contact");
	MessageRowParts album = received;
	album.kind = QString::fromUtf8("อัลบั้มรูปภาพ");
	expect(withMessageParts(QString::fromUtf8("Moss, รูป, 13.55 น."), album, you),
	       QString::fromUtf8("Moss, อัลบั้มรูปภาพ, 13.55 น."), "album");
	MessageRowParts unknown;
	expect(withMessageParts(QString::fromUtf8("Wantanee ตอบกลับข้อความของ Aew Sirilak ข้อความเดิมของ Aew Sirilak คือ 16 เช้า 17 เช้า ตอบกลับว่า อาจารย์ ไปเชียงใหม่ 18-21 พย, 18.20 น."), unknown, you),
	       QString::fromUtf8("Wantanee, ตอบกลับ Aew Sirilak: อาจารย์ ไปเชียงใหม่ 18-21 พย, ข้อความเดิม: 16 เช้า 17 เช้า, 18.20 น."), "reply");
	expect(withMessageParts(QString::fromUtf8("ตอบกลับข้อความของ Moss ข้อความเดิมของ Moss คือ ไฟล์ : a.pdf ตอบกลับว่า ok, ขอบคุณ, 9.00 น., อ่านแล้ว"), own, you),
	       QString::fromUtf8("คุณ, ตอบกลับ Moss: ok, ขอบคุณ, ข้อความเดิม: ไฟล์ : a.pdf, 9.00 น., อ่านแล้ว"), "own reply with a comma");
	// The other languages' sentences (labels stay Thai here).
	expect(withMessageParts(QString::fromUtf8("A reply from nattanun to Aew Sirilak's message. Aew Sirilak's original message: @nattanun slide ok?. Reply: ได้เลยครับ., 3:56 PM"), unknown, you),
	       QString::fromUtf8("nattanun, ตอบกลับ Aew Sirilak: ได้เลยครับ, ข้อความเดิม: @nattanun slide ok?, 3:56 PM"), "reply (English)");
	expect(withMessageParts(QString::fromUtf8("A reply from Chatchawarn - Pop to nattanun's message. nattanun's original message: . Reply: ยุทธ 70, 10:51 AM"), unknown, you),
	       QString::fromUtf8("Chatchawarn - Pop, ตอบกลับ nattanun: ยุทธ 70, 10:51 AM"), "reply to a message without text (English)");
	expect(withMessageParts(QString::fromUtf8("Aew Sirilakのメッセージへのnattanunのリプライ。Aew Sirilakの元のメッセージ：17 เช้าไหมคะ。リプライ：ได้ครับ。, 午後 6:52"), unknown, you),
	       QString::fromUtf8("nattanun, ตอบกลับ Aew Sirilak: ได้ครับ, ข้อความเดิม: 17 เช้าไหมคะ, 午後 6:52"), "reply (Japanese)");
	expect(withMessageParts(QString::fromUtf8("nattanun回复Aew Sirilak的信息。Aew Sirilak的原信息是：@nattanun slide。回复是：ได้เลยครับ。, 下午 3:56"), unknown, you),
	       QString::fromUtf8("nattanun, ตอบกลับ Aew Sirilak: ได้เลยครับ, ข้อความเดิม: @nattanun slide, 下午 3:56"), "reply (Simplified Chinese)");
	expect(withMessageParts(QString::fromUtf8("Aew Sirilak回覆nattanun的訊息。nattanun的原始訊息：ได้เลยครับ。回覆內容：ขอบคุณค่ะ。, 下午 3:56"), unknown, you),
	       QString::fromUtf8("Aew Sirilak, ตอบกลับ nattanun: ขอบคุณค่ะ, ข้อความเดิม: ได้เลยครับ, 下午 3:56"), "reply (Traditional Chinese)");
	expect(withMessageParts(QString::fromUtf8("Moss, 午前 7:57"), unknown, you), QString::fromUtf8("Moss, 午前 7:57"), "Japanese time");
	expect(line_a11y::isMessageTimeOrReceipt(QString::fromUtf8("午前 7:57")) && line_a11y::isMessageTimeOrReceipt(QString::fromUtf8("既読 3"))
	           ? QString() : QStringLiteral("no"), QString(), "Japanese time and receipt");

	using line_a11y::withoutRepeatedSender;
	expect(withoutRepeatedSender(QString::fromUtf8("Moss, ค่ะ, 9.01 น."), QString::fromUtf8("Moss, สติกเกอร์, 9.00 น.")),
	       QString::fromUtf8("ค่ะ, 9.01 น."), "same sender");
	expect(withoutRepeatedSender(QString::fromUtf8("Moss, ค่ะ, 9.01 น."), QString::fromUtf8("Aew, ค่ะ, 9.00 น.")),
	       QString::fromUtf8("Moss, ค่ะ, 9.01 น."), "other sender");
	expect(withoutRepeatedSender(QString::fromUtf8("Moss, ค่ะ, 9.01 น."), QString::fromUtf8("4 พฤศจิกายน 2026")),
	       QString::fromUtf8("Moss, ค่ะ, 9.01 น."), "after a date");
	expect(withoutRepeatedSender(QString::fromUtf8("คุณ, ok, 9.01 น., อ่านแล้ว"), QString::fromUtf8("คุณ, สติกเกอร์, 9.00 น.")),
	       QString::fromUtf8("ok, 9.01 น., อ่านแล้ว"), "own after own");
	expect(withoutRepeatedSender(QString::fromUtf8("ข้อความ, 9.01 น."), QString::fromUtf8("ข้อความ, 9.00 น.")),
	       QString::fromUtf8("ข้อความ, 9.01 น."), "no sender: kept");
	return failures ? 1 : 0;
}

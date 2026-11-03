#include "native_message_text.h"

#include <QtGui/QGuiApplication>
#include <cstdio>

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);
	int failures = 0;
	const auto expect = [&failures](const QVariant &value, const QString &wanted, const char *name) {
		if (line_a11y::nativeMessageText(value) != wanted) {
			std::fprintf(stderr, "FAIL: %s\n", name);
			++failures;
		}
	};
	expect(QString::fromUtf8("你, 收到，謝謝, 已讀 2, 下午 10:23"),
	       QString::fromUtf8("你, 收到，謝謝, 已讀 2, 下午 10:23"), "native status remains intact");
	expect(QString::fromUtf8("回覆甲的訊息。原始訊息：<a href='line://nv/profilePopup/mid=test'>@乙</a> 請確認。回覆內容：收到。"),
	       QString::fromUtf8("回覆甲的訊息。原始訊息：@乙 請確認。回覆內容：收到。"), "rich link after plain prefix");
	expect(QStringLiteral("A &amp; B &lt;tag&gt; &#x1F600;"),
	       QString::fromUtf8("A & B <tag> 😀"), "entities and literal angle brackets");
	expect(QStringLiteral("1 < 2 > 0"), QStringLiteral("1 < 2 > 0"), "ordinary comparisons");
	expect(QStringLiteral("<p>first</p><p>second<br>third</p>"),
	       QStringLiteral("first second third"), "block and line boundaries");
	const QString longText = QString::fromUtf8("完整正文").repeated(500) + QString::fromUtf8("，已讀 3，下午 10:23");
	expect(longText, longText, "no diagnostic-preview truncation");
	expect(QStringLiteral("  \n\t "), QString(), "empty text falls back");
	expect(QVariant(), QString(), "invalid role falls back");
	expect(QVariant(42), QString(), "non-string role falls back");
	return failures ? 1 : 0;
}

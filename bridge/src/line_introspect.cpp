#include "line_introspect.h"

#include "a11y_log.h"
#include "text_capture.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QLocale>
#include <QtCore/QMetaProperty>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtGui/QTextDocumentFragment>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <climits>
#include <cstring>

namespace line_a11y {
namespace {

// Painted text is recomputed at most this often per widget (NVDA asks for names repeatedly).
constexpr qint64 kPaintedTextTtlMs = 300;

struct PaintedTextEntry {
	QPointer<QWidget> widget;
	QString text;
	qint64 stamp = 0;
};

QElapsedTimer &clock()
{
	static QElapsedTimer t;
	if (!t.isValid())
		t.start();
	return t;
}

bool isPasswordWidget(const QWidget *w)
{
	if (inheritsClass(w, "PasswordTextField"))
		return true;
	if (auto *le = qobject_cast<const QLineEdit *>(w))
		return le->echoMode() != QLineEdit::Normal;
	return false;
}

// ---- Labels ------------------------------------------------------------------------------------

const QHash<QString, QString> &thaiLabels()
{
	static const QHash<QString, QString> map = {
		{ QStringLiteral("Show password"), QStringLiteral("แสดงรหัสผ่าน") },
		{ QStringLiteral("Hide password"), QStringLiteral("ซ่อนรหัสผ่าน") },
		{ QStringLiteral("Refresh QR code"), QStringLiteral("รีเฟรชคิวอาร์โค้ด") },
		{ QStringLiteral("QR code"), QStringLiteral("คิวอาร์โค้ด") },
		{ QStringLiteral("Friends"), QStringLiteral("เพื่อน") },
		{ QStringLiteral("Home"), QStringLiteral("หน้าหลัก") },
		{ QStringLiteral("Chats"), QStringLiteral("แชท") },
		{ QStringLiteral("Add friends"), QStringLiteral("เพิ่มเพื่อน") },
		{ QStringLiteral("Timeline"), QStringLiteral("ไทม์ไลน์") },
		{ QStringLiteral("Story"), QStringLiteral("สตอรี่") },
		{ QStringLiteral("News"), QStringLiteral("ข่าว") },
		{ QStringLiteral("Settings"), QStringLiteral("การตั้งค่า") },
		{ QStringLiteral("Calls"), QStringLiteral("การโทร") },
		{ QStringLiteral("Video call"), QStringLiteral("วิดีโอคอล") },
		{ QStringLiteral("Voice call"), QStringLiteral("โทรด้วยเสียง") },
		{ QStringLiteral("Notifications"), QStringLiteral("การแจ้งเตือน") },
		{ QStringLiteral("Close"), QStringLiteral("ปิด") },
		{ QStringLiteral("Minimize"), QStringLiteral("ย่อหน้าต่าง") },
		{ QStringLiteral("Maximize"), QStringLiteral("ขยายหน้าต่าง") },
		{ QStringLiteral("Restore"), QStringLiteral("คืนขนาดหน้าต่าง") },
		{ QStringLiteral("Search"), QStringLiteral("ค้นหา") },
		{ QStringLiteral("Clear search"), QStringLiteral("ล้างการค้นหา") },
		{ QStringLiteral("Clear"), QStringLiteral("ล้าง") },
		{ QStringLiteral("More options"), QStringLiteral("ตัวเลือกเพิ่มเติม") },
		{ QStringLiteral("Call"), QStringLiteral("โทร") },
		{ QStringLiteral("End call"), QStringLiteral("วางสาย") },
		{ QStringLiteral("Answer"), QStringLiteral("รับสาย") },
		{ QStringLiteral("Decline"), QStringLiteral("ปฏิเสธสาย") },
		{ QStringLiteral("Microphone"), QStringLiteral("ไมโครโฟน") },
		{ QStringLiteral("Speaker"), QStringLiteral("ลำโพง") },
		{ QStringLiteral("Video"), QStringLiteral("วิดีโอ") },
		{ QStringLiteral("Camera"), QStringLiteral("กล้อง") },
		{ QStringLiteral("Answer with video"), QStringLiteral("รับสายแบบวิดีโอ") },
		{ QStringLiteral("Microphone off"), QStringLiteral("ปิดไมโครโฟนอยู่") },
		{ QStringLiteral("Camera off"), QStringLiteral("ปิดกล้องอยู่") },
		{ QStringLiteral("Background blur"), QStringLiteral("เบลอพื้นหลัง") },
		{ QStringLiteral("Notes"), QStringLiteral("โน้ต") },
		{ QStringLiteral("Show or hide chat list"), QStringLiteral("แสดงหรือซ่อนรายการแชท") },
		{ QStringLiteral("Pause"), QStringLiteral("หยุดชั่วคราว") },
		{ QStringLiteral("Play"), QStringLiteral("เล่น") },
		{ QStringLiteral("Sound"), QStringLiteral("เสียง") },
		{ QStringLiteral("Mute"), QStringLiteral("ปิดเสียง") },
		{ QStringLiteral("Full screen"), QStringLiteral("เต็มหน้าจอ") },
		{ QStringLiteral("Menu"), QStringLiteral("เมนู") },
		{ QStringLiteral("Back"), QStringLiteral("ย้อนกลับ") },
		{ QStringLiteral("Previous"), QStringLiteral("ก่อนหน้า") },
		{ QStringLiteral("Next"), QStringLiteral("ถัดไป") },
		{ QStringLiteral("Forward"), QStringLiteral("ส่งต่อ") },
		{ QStringLiteral("Expand"), QStringLiteral("ขยาย") },
		{ QStringLiteral("Collapse"), QStringLiteral("ยุบ") },
		{ QStringLiteral("Add"), QStringLiteral("เพิ่ม") },
		{ QStringLiteral("Delete"), QStringLiteral("ลบ") },
		{ QStringLiteral("Remove"), QStringLiteral("นำออก") },
		{ QStringLiteral("Edit"), QStringLiteral("แก้ไข") },
		{ QStringLiteral("Copy"), QStringLiteral("คัดลอก") },
		{ QStringLiteral("Share"), QStringLiteral("แชร์") },
		{ QStringLiteral("Download"), QStringLiteral("ดาวน์โหลด") },
		{ QStringLiteral("Upload"), QStringLiteral("อัปโหลด") },
		{ QStringLiteral("Save"), QStringLiteral("บันทึก") },
		{ QStringLiteral("Pin"), QStringLiteral("ปักหมุด") },
		{ QStringLiteral("Unpin"), QStringLiteral("เลิกปักหมุด") },
		{ QStringLiteral("Mute notifications"), QStringLiteral("ปิดการแจ้งเตือน") },
		{ QStringLiteral("Unmute notifications"), QStringLiteral("เปิดการแจ้งเตือน") },
		{ QStringLiteral("Selected"), QStringLiteral("เลือกอยู่") },
		{ QStringLiteral("Information"), QStringLiteral("ข้อมูล") },
		{ QStringLiteral("Help"), QStringLiteral("ความช่วยเหลือ") },
		{ QStringLiteral("Profile"), QStringLiteral("โปรไฟล์") },
		{ QStringLiteral("Invite"), QStringLiteral("เชิญ") },
		{ QStringLiteral("Albums"), QStringLiteral("อัลบั้ม") },
		{ QStringLiteral("Announcements"), QStringLiteral("ประกาศ") },
		{ QStringLiteral("Translate"), QStringLiteral("แปล") },
		{ QStringLiteral("Stickers and emoji"), QStringLiteral("สติกเกอร์และอีโมจิ") },
		{ QStringLiteral("Attach file"), QStringLiteral("แนบไฟล์") },
		{ QStringLiteral("Send image"), QStringLiteral("ส่งรูปภาพ") },
		{ QStringLiteral("Take screenshot"), QStringLiteral("จับภาพหน้าจอ") },
		{ QStringLiteral("Voice message"), QStringLiteral("ข้อความเสียง") },
		{ QStringLiteral("Send"), QStringLiteral("ส่ง") },
		{ QStringLiteral("Keyboard"), QStringLiteral("แป้นพิมพ์") },
		{ QStringLiteral("Send a gift"), QStringLiteral("ส่งของขวัญ") },
		{ QStringLiteral("Share location"), QStringLiteral("แชร์ตำแหน่ง") },
		{ QStringLiteral("Share contact"), QStringLiteral("แชร์รายชื่อติดต่อ") },
		{ QStringLiteral("Advertisement"), QStringLiteral("โฆษณา") },
		{ QStringLiteral("Ad options"), QStringLiteral("ตัวเลือกโฆษณา") },
		{ QStringLiteral("Password"), QStringLiteral("รหัสผ่าน") },
		{ QStringLiteral("Email address"), QStringLiteral("อีเมล") },
		{ QStringLiteral("Phone number"), QStringLiteral("หมายเลขโทรศัพท์") },
		{ QStringLiteral("Message input"), QStringLiteral("ช่องพิมพ์ข้อความ") },
		{ QStringLiteral("Search chats"), QStringLiteral("ค้นหาแชท") },
		{ QStringLiteral("Search friends"), QStringLiteral("ค้นหาเพื่อน") },
		{ QStringLiteral("Text field"), QStringLiteral("ช่องข้อความ") },
		{ QStringLiteral("Text area"), QStringLiteral("ช่องข้อความ") },
		{ QStringLiteral("Context menu"), QStringLiteral("เมนู") },
		{ QStringLiteral("%1 options"), QStringLiteral("ตัวเลือก%1") },
		{ QStringLiteral("%1 new"), QStringLiteral("ใหม่ %1") },
		{ QStringLiteral("Country code %1"), QStringLiteral("รหัสประเทศ %1") },
		{ QStringLiteral("%1 members"), QStringLiteral("สมาชิก %1 คน") },
		{ QStringLiteral("%1 unread"), QStringLiteral("ยังไม่อ่าน %1") },
		{ QStringLiteral("Photo or sticker"), QStringLiteral("รูปภาพหรือสติกเกอร์") },
		{ QStringLiteral("You"), QStringLiteral("คุณ") },
		{ QStringLiteral("Messages"), QStringLiteral("ข้อความ") },
		{ QStringLiteral("Services"), QStringLiteral("บริการ") },
		{ QStringLiteral("Meeting"), QStringLiteral("ประชุม") },
		{ QStringLiteral("More"), QStringLiteral("เพิ่มเติม") },
		{ QStringLiteral("Notifications off"), QStringLiteral("ปิดการแจ้งเตือนอยู่") },
		{ QStringLiteral("Scroll to latest message"), QStringLiteral("เลื่อนไปข้อความล่าสุด") },
		{ QStringLiteral("Create chat"), QStringLiteral("สร้างแชท") },
		{ QStringLiteral("Edit chat folders"), QStringLiteral("แก้ไขโฟลเดอร์แชท") },
		{ QStringLiteral("Accept"), QStringLiteral("ยอมรับ") },
		{ QStringLiteral("Sort"), QStringLiteral("เรียงลำดับ") },
		{ QStringLiteral("Open in new window"), QStringLiteral("เปิดในหน้าต่างใหม่") },
		{ QStringLiteral("Close panel"), QStringLiteral("ปิดแผง") },
		{ QStringLiteral("Search in chat list"), QStringLiteral("ค้นหาในรายการแชท") },
		{ QStringLiteral("AI search"), QStringLiteral("ค้นหาด้วย AI") },
		{ QStringLiteral("Cancel reply"), QStringLiteral("ยกเลิกการตอบกลับ") },
		{ QStringLiteral("Close ad"), QStringLiteral("ปิดโฆษณา") },
		{ QStringLiteral("Screenshot options"), QStringLiteral("ตัวเลือกการจับภาพหน้าจอ") },
		{ QStringLiteral("Translate text in image"), QStringLiteral("แปลข้อความในภาพ") },
	};
	return map;
}

bool computeUiIsThai()
{
	// LINE sets its own UI language (independent of Windows); the installer records it.
	const QString ini = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
		+ QStringLiteral("/LINE/Data/installLang.ini");
	const QString lang = QSettings(ini, QSettings::IniFormat).value(QStringLiteral("General/installLang")).toString();
	if (!lang.isEmpty())
		return lang.startsWith(QLatin1String("th"), Qt::CaseInsensitive);
	return QLocale().language() == QLocale::Thai;
}

// ---- Icon names --------------------------------------------------------------------------------

const QHash<QString, const char *> &iconLabels()
{
	static const QHash<QString, const char *> map = {
		// Exact LINE for Windows resource names (image://<category>/<name>.svg)
		{ QStringLiteral("system-mini"), "Minimize" },
		{ QStringLiteral("system-max"), "Maximize" },
		{ QStringLiteral("system-restore"), "Restore" },
		{ QStringLiteral("system-close"), "Close" },
		{ QStringLiteral("login-menu"), "Menu" },
		{ QStringLiteral("login-help"), "Help" },
		// Main window sidebar (image://main/gnb-*.svg) and chat window
		{ QStringLiteral("gnb-friends"), "Friends" },
		{ QStringLiteral("gnb-chat"), "Chats" },
		{ QStringLiteral("gnb-add-friend"), "Add friends" },
		{ QStringLiteral("gnb-voom"), "LINE VOOM" },
		{ QStringLiteral("gnb-openchat"), "OpenChat" },
		{ QStringLiteral("gnb-keep"), "Keep" },
		{ QStringLiteral("gnb-hub"), "Services" },
		{ QStringLiteral("gnb-service"), "Services" },
		{ QStringLiteral("gnb-meeting"), "Meeting" },
		{ QStringLiteral("gnb-capture"), "Take screenshot" },
		{ QStringLiteral("gnb-more"), "More" },
		{ QStringLiteral("gnb-os-alarm-off"), "Notifications off" },
		{ QStringLiteral("gnb-os-alarm-on"), "Notifications" },
		{ QStringLiteral("today-btn"), "LINE TODAY" },
		{ QStringLiteral("system-full"), "Maximize" },
		{ QStringLiteral("down-arrow"), "Scroll to latest message" },
		{ QStringLiteral("add-chat-floating"), "Create chat" },
		{ QStringLiteral("search-cancel"), "Clear search" },
		{ QStringLiteral("search-keyword-delete"), "Clear search" },
		{ QStringLiteral("chatfolder-edit-icon"), "Edit chat folders" },
		{ QStringLiteral("accept-bt"), "Accept" },
		{ QStringLiteral("refuse-bt"), "Decline" },
		{ QStringLiteral("sorting"), "Sort" },
		{ QStringLiteral("new-window"), "Open in new window" },
		{ QStringLiteral("panel-close"), "Close panel" },
		{ QStringLiteral("chat-search-list"), "Search in chat list" },
		{ QStringLiteral("ai-search"), "AI search" },
		{ QStringLiteral("reply-layer-close"), "Cancel reply" },
		{ QStringLiteral("banner-close"), "Close ad" },
		{ QStringLiteral("chatroom-keep"), "Keep" },
		{ QStringLiteral("chatroom-alarm"), "Notifications" },
		{ QStringLiteral("input-sticker"), "Stickers and emoji" },
		{ QStringLiteral("input-keep"), "Keep" },
		{ QStringLiteral("input-file"), "Attach file" },
		{ QStringLiteral("input-capture"), "Take screenshot" },
		{ QStringLiteral("input-capture-option"), "Screenshot options" },
		{ QStringLiteral("ocr-translate"), "Translate text in image" },
		{ QStringLiteral("profile-hover-voice-call-icon"), "Voice call" },
		{ QStringLiteral("profile-hover-video-call-icon"), "Video call" },
		{ QStringLiteral("profile-hover-chat-icon"), "Chats" },
		// Password visibility / login
		{ QStringLiteral("eye-show"), "Show password" },
		{ QStringLiteral("show-eye"), "Show password" },
		{ QStringLiteral("eye-open"), "Show password" },
		{ QStringLiteral("eye-hide"), "Hide password" },
		{ QStringLiteral("hide-eye"), "Hide password" },
		{ QStringLiteral("eye-close"), "Hide password" },
		{ QStringLiteral("refresh"), "Refresh QR code" },
		{ QStringLiteral("qr"), "QR code" },
		{ QStringLiteral("qrcode"), "QR code" },
		// Navigation sidebar
		{ QStringLiteral("friend"), "Friends" },
		{ QStringLiteral("friends"), "Friends" },
		{ QStringLiteral("home"), "Home" },
		{ QStringLiteral("chat"), "Chats" },
		{ QStringLiteral("chats"), "Chats" },
		{ QStringLiteral("talk"), "Chats" },
		{ QStringLiteral("add-friend"), "Add friends" },
		{ QStringLiteral("add-friends"), "Add friends" },
		{ QStringLiteral("friend-add"), "Add friends" },
		{ QStringLiteral("addfriend"), "Add friends" },
		{ QStringLiteral("keep"), "Keep" },
		{ QStringLiteral("timeline"), "Timeline" },
		{ QStringLiteral("voom"), "LINE VOOM" },
		{ QStringLiteral("story"), "Story" },
		{ QStringLiteral("news"), "News" },
		{ QStringLiteral("today"), "LINE TODAY" },
		{ QStringLiteral("wallet"), "Wallet" },
		{ QStringLiteral("openchat"), "OpenChat" },
		{ QStringLiteral("open-chat"), "OpenChat" },
		{ QStringLiteral("setting"), "Settings" },
		{ QStringLiteral("settings"), "Settings" },
		{ QStringLiteral("call"), "Calls" },
		{ QStringLiteral("calls"), "Calls" },
		{ QStringLiteral("voip"), "Calls" },
		{ QStringLiteral("video-call"), "Video call" },
		{ QStringLiteral("voice-call"), "Voice call" },
		{ QStringLiteral("notification"), "Notifications" },
		{ QStringLiteral("notifications"), "Notifications" },
		{ QStringLiteral("noti"), "Notifications" },
		{ QStringLiteral("bell"), "Notifications" },
		// Window / generic controls
		{ QStringLiteral("close"), "Close" },
		{ QStringLiteral("x"), "Close" },
		{ QStringLiteral("minimize"), "Minimize" },
		{ QStringLiteral("maximize"), "Maximize" },
		{ QStringLiteral("search"), "Search" },
		{ QStringLiteral("search-delete"), "Clear search" },
		{ QStringLiteral("clear"), "Clear" },
		{ QStringLiteral("more"), "More options" },
		{ QStringLiteral("more-icon"), "More options" },
		// Chat room header
		{ QStringLiteral("chatroom-call"), "Call" },
		{ QStringLiteral("chatroom-search"), "Search" },
		{ QStringLiteral("chatroom-note"), "Notes" },
		{ QStringLiteral("chatroom-more"), "More options" },
		// Call window (image://voip/...)
		{ QStringLiteral("call-end-bt"), "End call" },
		{ QStringLiteral("call-answer"), "Answer" },
		{ QStringLiteral("call-decline"), "Decline" },
		{ QStringLiteral("mic-bt"), "Microphone" },
		{ QStringLiteral("speaker-bt"), "Speaker" },
		{ QStringLiteral("video-bt"), "Video" },
		{ QStringLiteral("camera-bt"), "Camera" },
		// Incoming-call notification (image://voip/notification/...)
		{ QStringLiteral("video-accept-bt"), "Answer with video" },
		{ QStringLiteral("mic-mute-bt"), "Microphone off" },
		{ QStringLiteral("camera-off-bt"), "Camera off" },
		{ QStringLiteral("blur-bt"), "Background blur" },
		{ QStringLiteral("chevron-down-bt"), "Expand" },
		{ QStringLiteral("chevron-up-bt"), "Collapse" },
		{ QStringLiteral("userpopup-close"), "Close" },
		// Chat list header
		{ QStringLiteral("open-bt"), "Show or hide chat list" },
		// Video ads
		{ QStringLiteral("banner-pause-bt"), "Pause" },
		{ QStringLiteral("banner-play-bt"), "Play" },
		{ QStringLiteral("banner-sound-bt"), "Sound" },
		{ QStringLiteral("banner-mute-bt"), "Mute" },
		{ QStringLiteral("full-screen-icon"), "Full screen" },
		{ QStringLiteral("menu"), "Menu" },
		{ QStringLiteral("kebab"), "More options" },
		{ QStringLiteral("back"), "Back" },
		{ QStringLiteral("prev"), "Previous" },
		{ QStringLiteral("previous"), "Previous" },
		{ QStringLiteral("next"), "Next" },
		{ QStringLiteral("forward"), "Forward" },
		{ QStringLiteral("arrow-left"), "Back" },
		{ QStringLiteral("arrow-right"), "Next" },
		{ QStringLiteral("arrow-down"), "Expand" },
		{ QStringLiteral("arrow-up"), "Collapse" },
		{ QStringLiteral("expand"), "Expand" },
		{ QStringLiteral("collapse"), "Collapse" },
		{ QStringLiteral("add"), "Add" },
		{ QStringLiteral("plus"), "Add" },
		{ QStringLiteral("delete"), "Delete" },
		{ QStringLiteral("trash"), "Delete" },
		{ QStringLiteral("remove"), "Remove" },
		{ QStringLiteral("edit"), "Edit" },
		{ QStringLiteral("pencil"), "Edit" },
		{ QStringLiteral("copy"), "Copy" },
		{ QStringLiteral("share"), "Share" },
		{ QStringLiteral("download"), "Download" },
		{ QStringLiteral("upload"), "Upload" },
		{ QStringLiteral("save"), "Save" },
		{ QStringLiteral("pin"), "Pin" },
		{ QStringLiteral("unpin"), "Unpin" },
		{ QStringLiteral("mute"), "Mute notifications" },
		{ QStringLiteral("unmute"), "Unmute notifications" },
		{ QStringLiteral("info"), "Information" },
		{ QStringLiteral("help"), "Help" },
		{ QStringLiteral("profile"), "Profile" },
		{ QStringLiteral("invite"), "Invite" },
		{ QStringLiteral("album"), "Albums" },
		{ QStringLiteral("note"), "Notes" },
		{ QStringLiteral("announcement"), "Announcements" },
		{ QStringLiteral("translate"), "Translate" },
		// Chat input
		{ QStringLiteral("emoji"), "Stickers and emoji" },
		{ QStringLiteral("emoticon"), "Stickers and emoji" },
		{ QStringLiteral("sticker"), "Stickers and emoji" },
		{ QStringLiteral("sticon"), "Stickers and emoji" },
		{ QStringLiteral("attach"), "Attach file" },
		{ QStringLiteral("attachment"), "Attach file" },
		{ QStringLiteral("file"), "Attach file" },
		{ QStringLiteral("clip"), "Attach file" },
		{ QStringLiteral("image"), "Send image" },
		{ QStringLiteral("photo"), "Send image" },
		{ QStringLiteral("gallery"), "Send image" },
		{ QStringLiteral("screenshot"), "Take screenshot" },
		{ QStringLiteral("capture"), "Take screenshot" },
		{ QStringLiteral("camera"), "Camera" },
		{ QStringLiteral("video"), "Video" },
		{ QStringLiteral("mic"), "Voice message" },
		{ QStringLiteral("voice"), "Voice message" },
		{ QStringLiteral("send"), "Send" },
		{ QStringLiteral("keyboard"), "Keyboard" },
		{ QStringLiteral("gift"), "Send a gift" },
		{ QStringLiteral("location"), "Share location" },
		{ QStringLiteral("contact"), "Share contact" },
	};
	return map;
}

// Tokens that describe state, size or decoration rather than meaning.
bool isNoiseToken(const QString &t)
{
	static const QStringList noise = {
		QStringLiteral("res"), QStringLiteral("img"), QStringLiteral("imgs"), QStringLiteral("images"),
		QStringLiteral("icon"), QStringLiteral("icons"), QStringLiteral("ico"), QStringLiteral("ic"),
		QStringLiteral("btn"), QStringLiteral("button"), QStringLiteral("normal"), QStringLiteral("nor"),
		QStringLiteral("hover"), QStringLiteral("over"), QStringLiteral("press"), QStringLiteral("pressed"),
		QStringLiteral("on"), QStringLiteral("off"), QStringLiteral("dis"), QStringLiteral("disable"),
		QStringLiteral("disabled"), QStringLiteral("dimmed"), QStringLiteral("dim"), QStringLiteral("sel"),
		QStringLiteral("select"), QStringLiteral("selected"), QStringLiteral("active"),
		QStringLiteral("inactive"), QStringLiteral("focus"), QStringLiteral("focused"),
		QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("black"), QStringLiteral("white"),
		QStringLiteral("gray"), QStringLiteral("grey"), QStringLiteral("default"), QStringLiteral("new"),
		QStringLiteral("small"), QStringLiteral("large"), QStringLiteral("big"), QStringLiteral("mini"),
		QStringLiteral("png"), QStringLiteral("svg"), QStringLiteral("gnb"), QStringLiteral("lnb"),
		QStringLiteral("nav"), QStringLiteral("navi"), QStringLiteral("sidebar"), QStringLiteral("side"),
		QStringLiteral("tab"), QStringLiteral("main"), QStringLiteral("common"), QStringLiteral("titlebar"),
		QStringLiteral("toolbar"), QStringLiteral("n"), QStringLiteral("h"), QStringLiteral("p"),
		QStringLiteral("s"), QStringLiteral("d"), QStringLiteral("2x"), QStringLiteral("3x"),
		QStringLiteral("lc"), QStringLiteral("system"),
	};
	if (noise.contains(t))
		return true;
	static const QRegularExpression numeric(QStringLiteral("^\\d+(x\\d+)?$"));
	return numeric.match(t).hasMatch();
}

QStringList splitIdentifier(const QString &s)
{
	// "accessUrlTextField" -> "access Url Text Field"; separators -> space.
	QString spaced;
	spaced.reserve(s.size() * 2);
	for (qsizetype i = 0; i < s.size(); ++i) {
		const QChar c = s.at(i);
		if (c == u'_' || c == u'-' || c == u'.' || c == u'/' || c == u':' || c == u'@' || c.isSpace()) {
			spaced += u' ';
			continue;
		}
		if (i > 0 && c.isUpper() && (s.at(i - 1).isLower() || s.at(i - 1).isDigit()))
			spaced += u' ';
		spaced += c;
	}
	return spaced.toLower().split(u' ', Qt::SkipEmptyParts);
}

QString lookupIcon(const QString &key)
{
	const auto &labels = iconLabels();
	if (auto it = labels.constFind(key); it != labels.constEnd())
		return label(*it);
	return QString();
}

} // namespace

// ---- Class helpers -----------------------------------------------------------------------------

bool inheritsClass(const QMetaObject *mo, const char *className)
{
	for (; mo; mo = mo->superClass()) {
		if (std::strcmp(mo->className(), className) == 0)
			return true;
	}
	return false;
}

bool inheritsClass(const QObject *object, const char *className)
{
	return object && inheritsClass(object->metaObject(), className);
}

bool inheritsAnyClass(const QObject *object, std::initializer_list<const char *> classNames)
{
	if (!object)
		return false;
	for (const QMetaObject *mo = object->metaObject(); mo; mo = mo->superClass()) {
		for (const char *name : classNames) {
			if (std::strcmp(mo->className(), name) == 0)
				return true;
		}
	}
	return false;
}

QWidget *ancestorInheriting(const QWidget *w, const char *className)
{
	for (QWidget *p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget()) {
		if (inheritsClass(p, className))
			return p;
	}
	return nullptr;
}

// ---- Properties --------------------------------------------------------------------------------

QVariant readProperty(const QObject *o, const char *name)
{
	const QMetaObject *mo = o->metaObject();
	const int index = mo->indexOfProperty(name);
	if (index >= 0)
		return mo->property(index).read(o);
	return o->property(name); // dynamic property, invalid QVariant if absent
}

QString stringProp(const QObject *o, const char *name)
{
	const QVariant v = readProperty(o, name);
	return v.metaType().id() == QMetaType::QString ? cleanText(v.toString()) : QString();
}

int boolProp(const QObject *o, const char *name)
{
	const QVariant v = readProperty(o, name);
	if (!v.isValid() || v.metaType().id() != QMetaType::Bool)
		return -1;
	return v.toBool() ? 1 : 0;
}

// ---- Text extraction ---------------------------------------------------------------------------

QString cleanText(const QString &text)
{
	if (text.isEmpty())
		return text;
	// LINE also escapes plain names ("Easy Read Database Search &amp; API").
	static const QRegularExpression entity(QStringLiteral("&(amp|lt|gt|quot|apos|nbsp|#\\d+);"));
	QString plain = Qt::mightBeRichText(text) || text.contains(entity)
		? QTextDocumentFragment::fromHtml(text).toPlainText()
		: text;
	plain.replace(QChar::ObjectReplacementCharacter, u' ');
	plain.replace(QChar::LineSeparator, u' ');
	plain.replace(QChar::ParagraphSeparator, u' ');
	plain.replace(QChar(0x200b), QString()); // zero-width space
	return plain.simplified();
}

QString paintedText(QWidget *w)
{
	if (!w || !w->isVisible() || w->width() <= 0 || w->height() <= 0 || w->paintingActive())
		return QString();
	static QHash<const QWidget *, PaintedTextEntry> cache;
	const qint64 now = clock().elapsed();
	auto it = cache.find(w);
	if (it != cache.end() && it->widget == w && now - it->stamp < kPaintedTextTtlMs)
		return it->text;
	if (cache.size() > 4096) {
		for (auto i = cache.begin(); i != cache.end();)
			i = (!i->widget || now - i->stamp > 10 * kPaintedTextTtlMs) ? cache.erase(i) : std::next(i);
	}
	const CaptureResult cap = captureWidgetText(w, false);
	if (cap.skipped) // LINE is painting: keep the previous reading
		return it != cache.end() && it->widget == w ? it->text : QString();
	const QString text = cleanText(capturedTextInReadingOrder(cap));
	cache.insert(w, { QPointer<QWidget>(w), text, now });
	return text;
}

QString extractLcTextString(QWidget *w)
{
	if (!w || !inheritsClass(w, "LcText"))
		return QString();
	if (const QString property = stringProp(w, "text"); !property.isEmpty())
		return property;
	return paintedText(w);
}

QString extractLcWidgetToolTip(QWidget *w)
{
	return w ? cleanText(w->toolTip()) : QString();
}

QString iconSource(const QWidget *w)
{
	for (const char *prop : { "iconSource", "source", "defaultSource" }) {
		const QVariant v = readProperty(w, prop);
		if (v.metaType().id() == QMetaType::QString && !v.toString().isEmpty())
			return v.toString();
	}
	return QString();
}

QString humanizeIconName(const QString &iconName)
{
	QString name = iconName.trimmed();
	if (name.isEmpty())
		return QString();
	// Photos, avatars and ads are file paths or URLs, not icons.
	if (name.startsWith(QLatin1String("file:")) || name.startsWith(QLatin1String("http"))
	    || name.contains(QLatin1Char('\\')) || (name.size() > 2 && name.at(1) == u':'))
		return QString();
	if (const qsizetype q = name.indexOf(u'?'); q >= 0)
		name.truncate(q);
	if (const qsizetype slash = name.lastIndexOf(u'/'); slash >= 0)
		name = name.mid(slash + 1);
	if (const qsizetype dot = name.lastIndexOf(u'.'); dot > 0)
		name.truncate(dot);
	static const QRegularExpression scale(QStringLiteral("@\\d+x$"));
	name.remove(scale);

	const QStringList all = splitIdentifier(name);
	if (const QString exact = lookupIcon(all.join(u'-')); !exact.isEmpty())
		return exact;
	QStringList tokens;
	for (const QString &t : all) {
		if (!isNoiseToken(t))
			tokens << t;
	}
	if (tokens.isEmpty())
		return QString();
	if (const QString dashed = lookupIcon(tokens.join(u'-')); !dashed.isEmpty())
		return dashed;
	if (const QString joined = lookupIcon(tokens.join(QString())); !joined.isEmpty())
		return joined;
	// Longest contiguous token run with a known label wins ("gnb_friend_add_count" -> "Add friends").
	for (qsizetype len = tokens.size() - 1; len >= 1; --len) {
		for (qsizetype start = 0; start + len <= tokens.size(); ++start) {
			if (const QString hit = lookupIcon(tokens.mid(start, len).join(u'-')); !hit.isEmpty())
				return hit;
		}
	}
	QString words = tokens.join(u' ');
	words[0] = words[0].toUpper();
	return words;
}

QString humanizeIdentifier(const QString &identifier)
{
	static const QStringList widgetSuffixes = {
		QStringLiteral("text field"), QStringLiteral("textfield"), QStringLiteral("text edit"),
		QStringLiteral("text area"), QStringLiteral("line edit"), QStringLiteral("button"),
		QStringLiteral("btn"), QStringLiteral("label"), QStringLiteral("text"), QStringLiteral("panel"),
		QStringLiteral("widget"), QStringLiteral("input"), QStringLiteral("field"), QStringLiteral("edit"),
	};
	QStringList tokens = splitIdentifier(identifier);
	QString phrase = tokens.join(u' ');
	for (const QString &suffix : widgetSuffixes) {
		if (phrase.endsWith(u' ' + suffix)) {
			phrase.chop(suffix.size() + 1);
			break;
		}
	}
	tokens = phrase.split(u' ', Qt::SkipEmptyParts);
	while (!tokens.isEmpty() && (tokens.first() == u"btn" || tokens.first() == u"lc" || tokens.first() == u"m"))
		tokens.removeFirst();
	if (tokens.isEmpty())
		return QString();
	if (const QString hit = lookupIcon(tokens.join(u'-')); !hit.isEmpty())
		return hit;
	QString words = tokens.join(u' ');
	words[0] = words[0].toUpper();
	return words;
}

QString widgetText(QWidget *w)
{
	if (!w || isPasswordWidget(w))
		return QString();

	const QString explicitName = cleanText(w->accessibleName());
	if (!explicitName.isEmpty())
		return explicitName;

	if (auto *label = qobject_cast<QLabel *>(w))
		return cleanText(label->text());
	if (auto *button = qobject_cast<QAbstractButton *>(w))
		return cleanText(button->text());
	if (auto *edit = qobject_cast<QLineEdit *>(w))
		return cleanText(edit->text());
	if (auto *edit = qobject_cast<QTextEdit *>(w))
		return cleanText(edit->toPlainText());
	if (auto *edit = qobject_cast<QPlainTextEdit *>(w))
		return cleanText(edit->toPlainText());

	if (inheritsClass(w, "LcText"))
		return extractLcTextString(w);

	if (inheritsAnyClass(w, { "HighlightableTextWidget", "LcLabel" })) {
		if (const QString text = stringProp(w, "text"); !text.isEmpty())
			return text;
		return paintedText(w);
	}
	if (w->metaObject()->indexOfProperty("text") >= 0)
		return stringProp(w, "text");
	return QString();
}

namespace {

void gatherTexts(QWidget *root, QWidget *parent, QList<PositionedText> &out, int depth)
{
	if (depth > 24 || out.size() > 256)
		return;
	for (QObject *child : parent->children()) {
		if (!child->isWidgetType())
			continue;
		auto *c = static_cast<QWidget *>(child);
		if (c->isWindow() || c->isHidden() || c->width() <= 0 || c->height() <= 0)
			continue;
		const QString text = widgetText(c);
		if (!text.isEmpty()) {
			out.append({ c->mapTo(root, QPoint(0, 0)), c->size(), text });
			continue; // the widget's text already represents its subtree
		}
		gatherTexts(root, c, out, depth + 1);
	}
}

} // namespace

QList<PositionedText> collectPositionedChildTexts(QWidget *root)
{
	QList<PositionedText> items;
	if (!root)
		return items;
	gatherTexts(root, root, items, 0);
	std::stable_sort(items.begin(), items.end(), [](const PositionedText &a, const PositionedText &b) {
		return a.pos.y() != b.pos.y() ? a.pos.y() < b.pos.y() : a.pos.x() < b.pos.x();
	});
	// A timestamp at the top right usually starts a pixel or two above the name on its left;
	// group such items into one line so the name is read first.
	QList<PositionedText> ordered;
	ordered.reserve(items.size());
	for (qsizetype start = 0; start < items.size();) {
		const int lineTop = items[start].pos.y();
		int lineBottom = lineTop + items[start].size.height();
		qsizetype end = start + 1;
		while (end < items.size()) {
			const int centre = items[end].pos.y() + items[end].size.height() / 2;
			if (centre < lineTop || centre > lineBottom)
				break;
			lineBottom = qMax(lineBottom, items[end].pos.y() + items[end].size.height());
			++end;
		}
		QList<PositionedText> line = items.mid(start, end - start);
		std::stable_sort(line.begin(), line.end(),
		                 [](const PositionedText &a, const PositionedText &b) { return a.pos.x() < b.pos.x(); });
		ordered += line;
		start = end;
	}
	return ordered;
}

QString collectVisibleChildTexts(QWidget *root, const QString &separator)
{
	QStringList parts;
	for (const PositionedText &item : collectPositionedChildTexts(root)) {
		if (parts.isEmpty() || parts.constLast() != item.text)
			parts << item.text;
	}
	return parts.join(separator);
}

bool isBadgeText(const QString &text)
{
	static const QRegularExpression badge(QStringLiteral("^\\(?\\d+\\+?\\)?$"));
	return badge.match(text.trimmed()).hasMatch();
}

QString nearbyLabelText(QWidget *w)
{
	QWidget *parent = w ? w->parentWidget() : nullptr;
	if (!parent)
		return QString();

	const QRect g = w->geometry();
	QString best;
	int bestScore = INT_MAX;
	for (QObject *child : parent->children()) {
		if (child == w || !child->isWidgetType())
			continue;
		auto *c = static_cast<QWidget *>(child);
		if (c->isHidden() || c->isWindow())
			continue;
		if (qobject_cast<QLineEdit *>(c) || qobject_cast<QTextEdit *>(c) || qobject_cast<QAbstractButton *>(c)
		    || inheritsAnyClass(c, { "LcButton", "LcCheckBox", "LcTextField", "LcTextArea" }))
			continue;
		const QRect cg = c->geometry();
		const int dy = qAbs(cg.center().y() - g.center().y());
		int score = INT_MAX;
		if (dy <= qMax(g.height(), cg.height()) / 2) {
			if (cg.left() >= g.right())
				score = cg.left() - g.right(); // same row, to the right
			else if (cg.right() <= g.left())
				score = 1000 + g.left() - cg.right(); // same row, to the left
		} else if (cg.bottom() <= g.top() && g.top() - cg.bottom() < 48 && cg.left() < g.right()
		           && cg.right() > g.left()) {
			score = 2000 + g.top() - cg.bottom(); // directly above
		}
		if (score >= bestScore)
			continue;
		QString text = widgetText(c);
		if (text.isEmpty())
			text = collectVisibleChildTexts(c, QStringLiteral(" "));
		if (!text.isEmpty() && !isBadgeText(text)) {
			best = text;
			bestScore = score;
		}
	}
	return best;
}

// ---- Localisation ------------------------------------------------------------------------------

bool uiIsThai()
{
	static const bool thai = [] {
		const bool t = computeUiIsThai();
		a11yLog("[introspect] LINE UI language: %s", t ? "Thai" : "English");
		return t;
	}();
	return thai;
}

QString label(const char *english)
{
	const QString en = QString::fromUtf8(english);
	if (uiIsThai()) {
		if (auto it = thaiLabels().constFind(en); it != thaiLabels().constEnd())
			return *it;
	}
	return en;
}

} // namespace line_a11y

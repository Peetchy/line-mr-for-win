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
	QString icon; // knownIcon() name of the first recognised image, e.g. "chatroom-call"
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

// The bridge's own labels in LINE's UI languages (see label()): English, Thai, Japanese,
// Simplified Chinese, Traditional Chinese.
struct LabelTranslations {
	const char *english;
	const char *thai;
	const char *japanese;
	const char *simplifiedChinese;
	const char *traditionalChinese;
};

const LabelTranslations kLabels[] = {
	{ "Show password", "แสดงรหัสผ่าน", "パスワードを表示", "显示密码", "顯示密碼" },
	{ "Hide password", "ซ่อนรหัสผ่าน", "パスワードを非表示", "隐藏密码", "隱藏密碼" },
	{ "Refresh QR code", "รีเฟรชคิวอาร์โค้ด", "QRコードを更新", "刷新二维码", "重新整理 QR 碼" },
	{ "QR code", "คิวอาร์โค้ด", "QRコード", "二维码", "QR 碼" },
	{ "Friends", "เพื่อน", "友だち", "好友", "好友" },
	{ "Home", "หน้าหลัก", "ホーム", "主页", "主頁" },
	{ "Chats", "แชท", "トーク", "聊天", "聊天" },
	{ "Add friends", "เพิ่มเพื่อน", "友だち追加", "添加好友", "加入好友" },
	{ "Timeline", "ไทม์ไลน์", "タイムライン", "动态", "貼文串" },
	{ "Story", "สตอรี่", "ストーリー", "限时动态", "限時動態" },
	{ "News", "ข่าว", "ニュース", "新闻", "新聞" },
	{ "Settings", "การตั้งค่า", "設定", "设置", "設定" },
	{ "Calls", "การโทร", "通話", "通话", "通話" },
	{ "Video call", "วิดีโอคอล", "ビデオ通話", "视频通话", "視訊通話" },
	{ "Voice call", "โทรด้วยเสียง", "音声通話", "语音通话", "語音通話" },
	{ "Notifications", "การแจ้งเตือน", "通知", "通知", "通知" },
	{ "Close", "ปิด", "閉じる", "关闭", "關閉" },
	{ "Minimize", "ย่อหน้าต่าง", "最小化", "最小化", "最小化" },
	{ "Maximize", "ขยายหน้าต่าง", "最大化", "最大化", "最大化" },
	{ "Restore", "คืนขนาดหน้าต่าง", "元のサイズに戻す", "还原", "還原" },
	{ "Search", "ค้นหา", "検索", "搜索", "搜尋" },
	{ "Clear search", "ล้างการค้นหา", "検索をクリア", "清除搜索", "清除搜尋" },
	{ "Clear", "ล้าง", "クリア", "清除", "清除" },
	{ "More options", "ตัวเลือกเพิ่มเติม", "その他のオプション", "更多选项", "更多選項" },
	{ "Call", "โทร", "通話", "通话", "通話" },
	{ "End call", "วางสาย", "通話を終了", "结束通话", "結束通話" },
	{ "Answer", "รับสาย", "応答", "接听", "接聽" },
	{ "Decline", "ปฏิเสธสาย", "拒否", "拒绝", "拒絕" },
	{ "Microphone", "ไมโครโฟน", "マイク", "麦克风", "麥克風" },
	{ "Speaker", "ลำโพง", "スピーカー", "扬声器", "喇叭" },
	{ "Video", "วิดีโอ", "動画", "视频", "影片" },
	{ "Camera", "กล้อง", "カメラ", "摄像头", "相機" },
	{ "Answer with video", "รับสายแบบวิดีโอ", "ビデオで応答", "视频接听", "以視訊接聽" },
	{ "Incoming call", "สายเรียกเข้า", "着信", "来电", "來電" },
	{ "Microphone off", "ปิดไมโครโฟนอยู่", "マイクオフ", "麦克风已关闭", "麥克風已關閉" },
	{ "Camera off", "ปิดกล้องอยู่", "カメラオフ", "摄像头已关闭", "相機已關閉" },
	{ "Background blur", "เบลอพื้นหลัง", "背景ぼかし", "背景模糊", "背景模糊" },
	{ "Notes", "โน้ต", "ノート", "记事本", "記事本" },
	{ "Show or hide chat list", "แสดงหรือซ่อนรายการแชท", "トークリストの表示/非表示", "显示或隐藏聊天列表", "顯示或隱藏聊天列表" },
	{ "Pause", "หยุดชั่วคราว", "一時停止", "暂停", "暫停" },
	{ "Play", "เล่น", "再生", "播放", "播放" },
	{ "Play sound", "เล่นเสียง", "音を再生", "播放声音", "播放聲音" },
	{ "Test sound", "ทดสอบเสียง", "サウンドをテスト", "测试声音", "測試聲音" },
	{ "Profile settings", "ตั้งค่าโปรไฟล์", "プロフィール設定", "个人资料设置", "個人檔案設定" },
	{ "Sound", "เสียง", "サウンド", "声音", "聲音" },
	{ "Mute", "ปิดเสียง", "ミュート", "静音", "靜音" },
	{ "Full screen", "เต็มหน้าจอ", "全画面表示", "全屏", "全螢幕" },
	{ "Menu", "เมนู", "メニュー", "菜单", "選單" },
	{ "Back", "ย้อนกลับ", "戻る", "返回", "返回" },
	{ "Previous", "ก่อนหน้า", "前へ", "上一个", "上一個" },
	{ "Next", "ถัดไป", "次へ", "下一个", "下一個" },
	{ "Forward", "ส่งต่อ", "転送", "转发", "轉傳" },
	{ "Expand", "ขยาย", "展開", "展开", "展開" },
	{ "Collapse", "ยุบ", "折りたたむ", "收起", "收合" },
	{ "Add", "เพิ่ม", "追加", "添加", "新增" },
	{ "Delete", "ลบ", "削除", "删除", "刪除" },
	{ "Remove", "นำออก", "取り除く", "移除", "移除" },
	{ "Edit", "แก้ไข", "編集", "编辑", "編輯" },
	{ "Copy", "คัดลอก", "コピー", "复制", "複製" },
	{ "Share", "แชร์", "共有", "分享", "分享" },
	{ "Download", "ดาวน์โหลด", "ダウンロード", "下载", "下載" },
	{ "Upload", "อัปโหลด", "アップロード", "上传", "上傳" },
	{ "Save", "บันทึก", "保存", "保存", "儲存" },
	{ "Pin", "ปักหมุด", "ピン留め", "置顶", "釘選" },
	{ "Unpin", "เลิกปักหมุด", "ピン留めを解除", "取消置顶", "取消釘選" },
	{ "Mute notifications", "ปิดการแจ้งเตือน", "通知をオフ", "关闭通知", "關閉通知" },
	{ "Unmute notifications", "เปิดการแจ้งเตือน", "通知をオン", "开启通知", "開啟通知" },
	{ "Selected", "เลือกอยู่", "選択中", "已选择", "已選取" },
	{ "Information", "ข้อมูล", "情報", "信息", "資訊" },
	{ "Help", "ความช่วยเหลือ", "ヘルプ", "帮助", "說明" },
	{ "Profile", "โปรไฟล์", "プロフィール", "个人资料", "個人檔案" },
	{ "Invite", "เชิญ", "招待", "邀请", "邀請" },
	{ "Albums", "อัลบั้ม", "アルバム", "相册", "相簿" },
	{ "Announcements", "ประกาศ", "アナウンス", "公告", "公告" },
	{ "Translate", "แปล", "翻訳", "翻译", "翻譯" },
	{ "Stickers and emoji", "สติกเกอร์และอีโมจิ", "スタンプと絵文字", "贴图和表情", "貼圖和表情貼" },
	{ "Attach file", "แนบไฟล์", "ファイルを添付", "附加文件", "附加檔案" },
	{ "Send image", "ส่งรูปภาพ", "画像を送信", "发送图片", "傳送圖片" },
	{ "Take screenshot", "จับภาพหน้าจอ", "スクリーンショット", "截图", "截圖" },
	{ "Voice message", "ข้อความเสียง", "ボイスメッセージ", "语音消息", "語音訊息" },
	{ "Location", "ตำแหน่งที่ตั้ง", "位置情報", "位置", "位置資訊" },
	{ "Send", "ส่ง", "送信", "发送", "傳送" },
	{ "Keyboard", "แป้นพิมพ์", "キーボード", "键盘", "鍵盤" },
	{ "Send a gift", "ส่งของขวัญ", "プレゼントを贈る", "赠送礼物", "送禮物" },
	{ "Share location", "แชร์ตำแหน่ง", "位置情報を共有", "分享位置", "分享位置資訊" },
	{ "Share contact", "แชร์รายชื่อติดต่อ", "連絡先を共有", "分享联系人", "分享聯絡人" },
	{ "Advertisement", "โฆษณา", "広告", "广告", "廣告" },
	{ "Ad options", "ตัวเลือกโฆษณา", "広告のオプション", "广告选项", "廣告選項" },
	{ "Password", "รหัสผ่าน", "パスワード", "密码", "密碼" },
	{ "Email address", "อีเมล", "メールアドレス", "电子邮件地址", "電子郵件地址" },
	{ "Phone number", "หมายเลขโทรศัพท์", "電話番号", "电话号码", "電話號碼" },
	{ "Message input", "ช่องพิมพ์ข้อความ", "メッセージ入力欄", "消息输入框", "訊息輸入框" },
	{ "Message to %1", "พิมพ์ข้อความถึง %1", "%1 へのメッセージ", "发消息给 %1", "傳訊息給 %1" },
	{ "Search chats", "ค้นหาแชท", "トークを検索", "搜索聊天", "搜尋聊天" },
	{ "Search friends", "ค้นหาเพื่อน", "友だちを検索", "搜索好友", "搜尋好友" },
	{ "Text field", "ช่องข้อความ", "テキスト入力欄", "文本框", "文字欄位" },
	{ "Text area", "ช่องข้อความ", "テキスト入力欄", "文本框", "文字欄位" },
	{ "Context menu", "เมนู", "メニュー", "菜单", "選單" },
	{ "%1 options", "ตัวเลือก%1", "%1 のオプション", "%1 选项", "%1 選項" },
	{ "%1 new", "ใหม่ %1", "新着 %1", "%1 条新消息", "%1 則新訊息" },
	{ "Country code %1", "รหัสประเทศ %1", "国番号 %1", "国家代码 %1", "國碼 %1" },
	{ "%1 members", "สมาชิก %1 คน", "メンバー %1 人", "%1 名成员", "%1 位成員" },
	{ "%1 unread", "ยังไม่อ่าน %1", "未読 %1", "%1 条未读", "%1 則未讀" },
	{ "Photo or sticker", "รูปภาพหรือสติกเกอร์", "写真またはスタンプ", "照片或贴图", "照片或貼圖" },
	{ "Photos", "อัลบั้มรูปภาพ", "写真アルバム", "相册", "相簿" },
	{ "Sticker", "สติกเกอร์", "スタンプ", "贴图", "貼圖" },
	{ "LINE emoji", "อีโมจิ LINE", "LINE絵文字", "LINE 表情", "LINE 表情貼" },
	{ "File", "ไฟล์", "ファイル", "文件", "檔案" },
	{ "Call message", "การโทร", "通話", "通话", "通話" },
	{ "Photo", "รูปภาพ", "写真", "照片", "照片" },
	{ "You", "คุณ", "あなた", "你", "你" },
	{ "Messages", "ข้อความ", "メッセージ", "消息", "訊息" },
	{ "Message", "ข้อความ", "メッセージ", "消息", "訊息" },
	{ "Message from %1", "ข้อความจาก %1", "%1 からのメッセージ", "来自 %1 的消息", "來自 %1 的訊息" },
	{ "Services", "บริการ", "サービス", "服务", "服務" },
	{ "Meeting", "ประชุม", "ミーティング", "会议", "會議" },
	{ "More", "เพิ่มเติม", "その他", "更多", "更多" },
	{ "Notifications off", "ปิดการแจ้งเตือนอยู่", "通知オフ", "通知已关闭", "通知已關閉" },
	{ "Scroll to latest message", "เลื่อนไปข้อความล่าสุด", "最新のメッセージへ移動", "滚动到最新消息", "捲動到最新訊息" },
	{ "Create chat", "สร้างแชท", "トークを作成", "创建聊天", "建立聊天" },
	{ "Edit chat folders", "แก้ไขโฟลเดอร์แชท", "トークフォルダーを編集", "编辑聊天文件夹", "編輯聊天資料夾" },
	{ "Accept", "ยอมรับ", "承認", "接受", "接受" },
	{ "Sort", "เรียงลำดับ", "並べ替え", "排序", "排序" },
	{ "Open in new window", "เปิดในหน้าต่างใหม่", "新しいウィンドウで開く", "在新窗口中打开", "在新視窗中開啟" },
	{ "Close panel", "ปิดแผง", "パネルを閉じる", "关闭面板", "關閉面板" },
	{ "Search in chat list", "ค้นหาในรายการแชท", "トークリスト内を検索", "在聊天列表中搜索", "在聊天列表中搜尋" },
	{ "AI search", "ค้นหาด้วย AI", "AI検索", "AI 搜索", "AI 搜尋" },
	{ "Cancel reply", "ยกเลิกการตอบกลับ", "リプライをキャンセル", "取消回复", "取消回覆" },
	{ "Close ad", "ปิดโฆษณา", "広告を閉じる", "关闭广告", "關閉廣告" },
	{ "Screenshot options", "ตัวเลือกการจับภาพหน้าจอ", "スクリーンショットのオプション", "截图选项", "截圖選項" },
	{ "Translate text in image", "แปลข้อความในภาพ", "画像内のテキストを翻訳", "翻译图片中的文字", "翻譯圖片中的文字" },
};

QHash<QString, QString> labelsFor(UiLanguage language)
{
	QHash<QString, QString> map;
	for (const LabelTranslations &t : kLabels) {
		const char *text = t.english;
		switch (language) {
		case UiLanguage::Thai:
			text = t.thai;
			break;
		case UiLanguage::Japanese:
			text = t.japanese;
			break;
		case UiLanguage::SimplifiedChinese:
			text = t.simplifiedChinese;
			break;
		case UiLanguage::TraditionalChinese:
			text = t.traditionalChinese;
			break;
		case UiLanguage::English:
			break;
		}
		map.insert(QString::fromUtf8(t.english), QString::fromUtf8(text));
	}
	return map;
}

UiLanguage computeUiLanguage()
{
	// LINE sets its own UI language (independent of Windows); the installer records it.
	const QString ini = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
		+ QStringLiteral("/LINE/Data/installLang.ini");
	QString lang = QSettings(ini, QSettings::IniFormat).value(QStringLiteral("General/installLang")).toString();
	if (lang.isEmpty())
		lang = QLocale().name(); // "th_TH", "zh_TW"
	lang = lang.toLower();
	if (lang.startsWith(QLatin1String("th")))
		return UiLanguage::Thai;
	if (lang.startsWith(QLatin1String("ja")))
		return UiLanguage::Japanese;
	if (lang.startsWith(QLatin1String("zh"))) {
		for (const char *traditional : { "tw", "hk", "mo", "hant" }) {
			if (lang.contains(QLatin1String(traditional)))
				return UiLanguage::TraditionalChinese;
		}
		return UiLanguage::SimplifiedChinese;
	}
	return UiLanguage::English;
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
		// Older title bars (ChatDetailTextMessageWindow): image://common/frame_*_btn.png
		{ QStringLiteral("frame-max-btn"), "Maximize" },
		{ QStringLiteral("frame-min-btn"), "Minimize" },
		{ QStringLiteral("frame-close-btn"), "Close" },
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
		{ QStringLiteral("accept-bt"), "Answer" },
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
		{ QStringLiteral("chatroom-video-call"), "Video call" },
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
		{ QStringLiteral("setting-play-btn-dark"), "Play sound" },
		{ QStringLiteral("setting-speaker-icon"), "Test sound" },
		// Settings > Profile: the only frame-drawn button without text (opens profile settings).
		{ QStringLiteral("setting-border-line"), "Profile settings" },
		{ QStringLiteral("play"), "Play" },
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
		{ QStringLiteral("chevron-left"), "Back" },
		{ QStringLiteral("select-close-bt"), "Remove" },
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
	// Leftover tags in plain text ("keep balance and move forward </img>").
	static const QRegularExpression strayTag(QStringLiteral("</?(img|a|br|span|b|i|font)\\b[^>]*>"),
	                                         QRegularExpression::CaseInsensitiveOption);
	plain.replace(strayTag, QStringLiteral(" "));
	plain.replace(QChar::ObjectReplacementCharacter, u' ');
	plain.replace(QChar::LineSeparator, u' ');
	plain.replace(QChar::ParagraphSeparator, u' ');
	plain.replace(QChar(0x200b), QString()); // zero-width space
	return plain.simplified();
}

// What `w` painted (text and the first known icon), rendered at most every kPaintedTextTtlMs.
PaintedTextEntry paintedEntry(QWidget *w)
{
	if (!w || !w->isVisible() || w->width() <= 0 || w->height() <= 0 || w->paintingActive())
		return {};
	static QHash<const QWidget *, PaintedTextEntry> cache;
	const qint64 now = clock().elapsed();
	auto it = cache.find(w);
	if (it != cache.end() && it->widget == w && now - it->stamp < kPaintedTextTtlMs)
		return *it;
	if (cache.size() > 4096) {
		for (auto i = cache.begin(); i != cache.end();)
			i = (!i->widget || now - i->stamp > 10 * kPaintedTextTtlMs) ? cache.erase(i) : std::next(i);
	}
	const CaptureResult cap = captureWidgetText(w, false);
	if (cap.skipped) // LINE is painting: keep the previous reading
		return it != cache.end() && it->widget == w ? *it : PaintedTextEntry();
	PaintedTextEntry entry { QPointer<QWidget>(w), cleanText(capturedTextInReadingOrder(cap)), QString(), now };
	for (const PaintedImage &pi : cap.images) {
		if (!pi.icon.isEmpty()) {
			entry.icon = pi.icon;
			break;
		}
	}
	cache.insert(w, entry);
	return entry;
}

QString paintedText(QWidget *w)
{
	return paintedEntry(w).text;
}

QString paintedIcon(QWidget *w)
{
	return paintedEntry(w).icon;
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
	// Longest contiguous token run with a known label wins ("gnb_friend_add_count" -> "Add friends");
	// among equally long runs the last one, since names lead with their area ("setting_close").
	for (qsizetype len = tokens.size() - 1; len >= 1; --len) {
		for (qsizetype start = tokens.size() - len; start >= 0; --start) {
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

QString labelAboveText(QWidget *w)
{
	QWidget *window = w ? w->window() : nullptr;
	if (!window || w == window)
		return QString();
	const QRect g(w->mapTo(window, QPoint()), w->size());
	QWidget *best = nullptr;
	// Nearest container first: hidden pages stacked at the same place have labels there too.
	for (QWidget *scope = w->parentWidget(); scope && !best && scope != window; scope = scope->parentWidget()) {
		int bestGap = 41;
		for (QWidget *c : scope->findChildren<QWidget *>()) {
			// A combo box under a checkbox sets that option ("Lock LINE when idle for" + "1 hour").
			if (!inheritsAnyClass(c, { "LcText", "LcCheckBox" }) || !c->isVisible())
				continue;
			const QRect t(c->mapTo(window, QPoint()), c->size());
			const int gap = g.top() - t.bottom();
			if (gap < 0 || gap >= bestGap || t.left() > g.left() + 8 || t.right() < g.left())
				continue;
			best = c;
			bestGap = gap;
		}
	}
	if (!best)
		return QString();
	return inheritsClass(best, "LcText") ? extractLcTextString(best) : widgetText(best);
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

UiLanguage uiLanguage()
{
	static const UiLanguage language = [] {
		const UiLanguage l = computeUiLanguage();
		static const char *const names[] = { "English", "Thai", "Japanese", "Simplified Chinese", "Traditional Chinese" };
		a11yLog("[introspect] LINE UI language: %s", names[int(l)]);
		return l;
	}();
	return language;
}

QString label(const char *english)
{
	const QString en = QString::fromUtf8(english);
	if (uiLanguage() == UiLanguage::English)
		return en;
	static const QHash<QString, QString> labels = labelsFor(uiLanguage());
	return labels.value(en, en);
}

} // namespace line_a11y

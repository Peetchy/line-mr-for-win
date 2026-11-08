#include "file_dialog_fill.h"

#include "a11y_log.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QMetaObject>
#include <QtCore/QString>

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace line_a11y {
namespace {

// The file name box of the common item dialog keeps the old common dialog's id (cmb13).
constexpr int kFileNameComboId = 1148;

std::wstring className(HWND hwnd)
{
	wchar_t buf[64] = {};
	GetClassNameW(hwnd, buf, 64);
	return buf;
}

// The visible file dialogs (#32770) of this process.
std::set<HWND> fileDialogs()
{
	std::set<HWND> found;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    DWORD pid = 0;
		    GetWindowThreadProcessId(hwnd, &pid);
		    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && className(hwnd) == L"#32770")
			    reinterpret_cast<std::set<HWND> *>(param)->insert(hwnd);
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&found));
	return found;
}

bool underClass(HWND hwnd, HWND top, const wchar_t *name)
{
	for (HWND p = GetParent(hwnd); p && p != top; p = GetParent(p)) {
		if (className(p) == name)
			return true;
	}
	return false;
}

// The edit field of the dialog's file name (or folder) box.
HWND fileNameEdit(HWND dialog)
{
	std::vector<HWND> children;
	EnumChildWindows(
	    dialog,
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    reinterpret_cast<std::vector<HWND> *>(param)->push_back(hwnd);
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&children));
	for (HWND child : children) {
		if (GetDlgCtrlID(child) == kFileNameComboId && IsWindowVisible(child)) {
			for (HWND edit : children) {
				if (className(edit) == L"Edit" && IsChild(child, edit))
					return edit;
			}
		}
	}
	// Otherwise the first shown edit field outside the address bar and the search box.
	for (HWND child : children) {
		if (className(child) == L"Edit" && IsWindowVisible(child) && !underClass(child, dialog, L"ReBarWindow32")
		    && !underClass(child, dialog, L"SearchEditBoxWrapperClass"))
			return child;
	}
	return nullptr;
}

// Windows' memory of file dialogs: the folder per program, the files per extension.
const wchar_t kComDlg32[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ComDlg32";

struct RegValue {
	DWORD type = 0;
	std::vector<BYTE> data;
};
// Key path (below HKCU) -> its values.
using RegSnapshot = std::map<std::wstring, std::map<std::wstring, RegValue>>;

std::map<std::wstring, RegValue> readValues(HKEY key)
{
	std::map<std::wstring, RegValue> values;
	for (DWORD i = 0;; ++i) {
		wchar_t name[256];
		DWORD nameLen = 256, type = 0, size = 0;
		if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
			break;
		RegValue v;
		v.type = type;
		v.data.resize(size);
		nameLen = 256;
		if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, v.data.data(), &size) == ERROR_SUCCESS) {
			v.data.resize(size);
			values[name] = std::move(v);
		}
	}
	return values;
}

std::vector<std::wstring> subkeys(const std::wstring &path)
{
	std::vector<std::wstring> names;
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
		return names;
	for (DWORD i = 0;; ++i) {
		wchar_t name[256];
		DWORD len = 256;
		if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
			break;
		names.push_back(path + L"\\" + name);
	}
	RegCloseKey(key);
	return names;
}

// The keys the dialog may change: LastVisitedPidlMRU and every OpenSavePidlMRU\<extension>.
std::vector<std::wstring> mruKeys()
{
	const std::wstring base = kComDlg32;
	std::vector<std::wstring> keys = { base + L"\\LastVisitedPidlMRU" };
	for (const std::wstring &key : subkeys(base + L"\\OpenSavePidlMRU"))
		keys.push_back(key);
	return keys;
}

RegSnapshot snapshotMru()
{
	RegSnapshot snapshot;
	for (const std::wstring &path : mruKeys()) {
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
			continue;
		snapshot[path] = readValues(key);
		RegCloseKey(key);
	}
	return snapshot;
}

// Puts the keys back as they were: values added are removed, changed ones rewritten, an extension's
// key that did not exist is deleted. Returns the number of changes undone.
int restoreMru(const RegSnapshot &snapshot)
{
	int changed = 0;
	for (const std::wstring &path : mruKeys()) {
		const auto old = snapshot.find(path);
		if (old == snapshot.end()) {
			if (path.find(L"\\OpenSavePidlMRU\\") != std::wstring::npos && RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS)
				++changed;
			continue;
		}
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
			continue;
		const std::map<std::wstring, RegValue> now = readValues(key);
		for (const auto &[name, value] : now) {
			if (!old->second.count(name) && RegDeleteValueW(key, name.c_str()) == ERROR_SUCCESS)
				++changed;
		}
		for (const auto &[name, value] : old->second) {
			const auto current = now.find(name);
			if (current != now.end() && current->second.type == value.type && current->second.data == value.data)
				continue;
			if (RegSetValueExW(key, name.c_str(), 0, value.type, value.data.data(), DWORD(value.data.size())) == ERROR_SUCCESS)
				++changed;
		}
		RegCloseKey(key);
	}
	return changed;
}

void reportLater(const QString &text, bool ok, const std::function<void(bool)> &done)
{
	QMetaObject::invokeMethod(
	    QCoreApplication::instance(),
	    [text, ok, done] {
		    a11yLog("[file-dialog] %s", qUtf8Printable(text));
		    if (done)
			    done(ok);
	    },
	    Qt::QueuedConnection);
}

} // namespace

void fillNextFileDialog(const QString &path, std::function<void(bool)> done, int timeoutMs)
{
	const std::wstring target = QDir::toNativeSeparators(path).toStdWString();
	const std::set<HWND> before = fileDialogs();
	a11yLog("[file-dialog] waiting for a file dialog (%d known)", int(before.size()));
	std::thread([target, before, timeoutMs, done, mru = snapshotMru()] {
		using namespace std::chrono;
		const auto end = steady_clock::now() + milliseconds(timeoutMs);
		while (steady_clock::now() < end) {
			std::this_thread::sleep_for(milliseconds(100));
			HWND dialog = nullptr;
			for (HWND hwnd : fileDialogs()) {
				if (!before.count(hwnd))
					dialog = hwnd;
			}
			if (!dialog)
				continue;
			// The dialog fills in its controls right after showing.
			HWND edit = nullptr;
			for (int i = 0; i < 20 && !edit; ++i) {
				std::this_thread::sleep_for(milliseconds(100));
				edit = fileNameEdit(dialog);
			}
			if (!edit) {
				reportLater(QStringLiteral("no file name box in the dialog"), false, done);
				return;
			}
			SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(target.c_str()));
			SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, IDOK)));
			reportLater(QStringLiteral("filled and confirmed"), true, done);
			// The dialog writes its memory as it closes.
			for (int i = 0; i < 100 && IsWindow(dialog) && IsWindowVisible(dialog); ++i)
				std::this_thread::sleep_for(milliseconds(100));
			std::this_thread::sleep_for(milliseconds(500));
			const int undone = restoreMru(mru);
			QMetaObject::invokeMethod(
			    QCoreApplication::instance(), [undone] { a11yLog("[file-dialog] Windows' remembered folders put back (%d changes undone)", undone); },
			    Qt::QueuedConnection);
			return;
		}
		reportLater(QStringLiteral("no file dialog appeared"), false, done);
	}).detach();
}

} // namespace line_a11y

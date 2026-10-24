// Play / Pause and the seek slider of LINE's video player.
//
// LineMediaPlayer.exe plays videos in a QQuickWidget (VideoRenderPanelWidget, QML VideoRenderPanel)
// whose footer controls (playBtn, timeSlider) are disabled Qt Quick items driven by the mouse:
// keyboard users could not reach them, and Qt's own interface for the scene let UI Automation wander
// into other applications. The bridge presents three controls inside the video widget: a Play / Pause
// button, a Position slider ("00:12 / 00:38") and a Playback speed slider ("1.0x", LINE's speed box).
// The widget keeps Qt's focus; the bridge moves a focus of its own between them (Tab / Shift+Tab; past
// either end to the viewer's other controls). Space / Enter play or pause; on the position the arrows
// seek 5 s, Page Up / Page Down 30 s, Home / End to the start / end; on the speed Up / Down are faster /
// slower. The keys are taken before LINE's shortcuts (Right = next media). Pressing and seeking do what
// LINE's mouse handlers do: emit playBtnClicked(), or set timeSlider's value (0..1) and emit
// timeSliderPressed() / timeSliderReleased(); the speed is the box's currentIndex (its handler emits
// speedChanged).

#pragma once

class QAccessibleEvent;
class QAccessibleInterface;
class QKeyEvent;
class QObject;
class QWidget;

namespace line_a11y {

// The interface of a video control item (playBtn, timeSlider, speedComboBox) in the viewer, or nullptr.
QAccessibleInterface *videoControlAccessible(QObject *object);

// The controls' interfaces for the video widget `quickWidget` (none while it shows no video).
int videoControlCount(QWidget *quickWidget);
QAccessibleInterface *videoControl(QWidget *quickWidget, int index);
int indexOfVideoControl(QWidget *quickWidget, const QAccessibleInterface *child);
QAccessibleInterface *focusedVideoControl(QWidget *quickWidget);

// Keys on the video widget. True if handled.
bool handleVideoKey(QWidget *quickWidget, QKeyEvent *key);
// The ShortcutOverride for a key the player takes: accepting it keeps LINE's shortcuts from firing.
bool videoWantsShortcut(QWidget *quickWidget, QKeyEvent *key);
// A value change of the position slider that the player made (every second while playing): dropped.
bool isPlayerValueEvent(QAccessibleEvent *event);
// In the viewer: installs (again) the native key filter that takes the arrows before LINE does.
// Called at start and every second.
void keepVideoKeyFilterFirst();
// The video widget got Qt's focus: report the control the bridge's focus is on.
void videoWidgetFocused(QWidget *quickWidget);

} // namespace line_a11y

# Native @ mentions with the bridge retained — reference patch

`native-mentions-r1.patch` contains the C++ source changes used in the user's successful bridge-enabled @ mention experiment. It is reference source, not a binary, and is not applied to this PR's production bridge.

## Behavior

- Keep `UserMentionSuggester` and its descendants on native accessibility.
- Do not identify the suggestion list as chat messages or apply bridge tags, focus-policy changes or Tab ordering to it.
- Pass editor key events to LINE's normal filters while a mention popup is visible in the same `ChatMessagePanel`.
- Release bridge browsing/focus intent when the user interacts with mentions. Hidden popup background events do not cancel unrelated intent.
- Leave message presentation and other bridge functionality unchanged; add no settings switch.

The bounded diagnostic logs at most 12 mention key codes per process, without candidate or message text. No diagnostic data is bundled here.

## Tested result

The user reported successful member selection and keyword filtering of the mention list. The test used Windows, NVDA, a Traditional Chinese LINE interface and the bridge enabled. Other languages, group types and independent windows require additional testing.

The Qt 6.6.3/MSVC Release build, NVDA import/syntax checks and six existing message-action dispatcher tests passed. The dispatcher tests do not validate native mention interaction; that result is user-reported live testing.

## Integration baseline

This patch was generated against a local combined test tree at commit `48bd2e9394c104f5920961377fecb1ba7441e71f`, **with additional uncommitted focus/navigation changes**. That commit alone is therefore not the complete patch baseline. The editor-intent timer used by the patch (`g_editorFocusTimer`) belongs to those combined focus changes; compare the pending focus/navigation work in PR #14 when adapting it.

Do not apply it blindly to main. The local source includes other independently proposed changes; review and adapt the mention-specific exclusions to the target branch. Original baseline file SHA-256 hashes are provided for identifying the exact inputs, not as a substitute for those source dependencies:

- `bridge/src/a11y_event_filter.cpp`: `6868a2bf4f3fe58d429050d34d06024909f8aa7f09f16d4729a67fcf13367f53`
- `bridge/src/accessible_widgets.cpp`: `63091360ddba28fde449652ffe41a6e8f468666a925b1743044363616727c886`
- `bridge/src/line_introspect.h`: `b91b6d167b15c8e7493e04c1b50d1c4900a5a75ae79d0860c31fe1175a2df6a8`
- `bridge/src/line_introspect.cpp`: `3df7d31da0539afa17b0e567e265a99c9d102069c5a6447e67fa72ae809db6df`

The patch changes only four bridge source files. No package version, release metadata, calls/Notes/OpenChats behavior, native message-reading mode or configurable navigation order is introduced. GPL v2, as in the parent project.

# Proposal: retain the bridge and progressively delegate supported features to native LINE

Thank you for your continued work on LINE accessibility. After further testing, I believe the bridge is mature and worth retaining. Native accessibility has improved, so we may be able to let LINE handle selected interactions while keeping the bridge's established assistance elsewhere.

## Suggested direction

Keep the bridge as the foundation for convenient operation. For each feature that native LINE now handles adequately, verify its behavior and selectively stop overriding that feature. This can be incremental; it does not require choosing between a fully native interface and a fully bridged interface.

- **@ mentions:** a first concrete example. The attached source patch leaves LINE's mention suggestions and their interaction native. The user tested selecting a member and typing keywords to filter the member list successfully with the bridge still enabled.
- **Message reading:** a candidate for a separate experiment. Native message presentation may improve the distinction between read receipts, quoted messages and replies. This PR does not implement native message reading or establish that all message types and navigation cases work.
- **Calls, Notes and OpenChats:** leave existing bridge behavior in place for now, regardless of whether native support has changed. Revisit a feature if its entry point breaks or a specific problem requires a change. Native call behavior has not been comprehensively tested.
- **Convenience shortcuts:** retain useful commands such as focusing the chat list or message editor. In the tested environment, native Ctrl+1/Ctrl+2 can switch tabs, but repeating the shortcut on the selected tab does not always return focus to its list.

## Native @ mention reference

See [examples/native-mentions](../examples/native-mentions/README.md) for the source patch and its integration notes.

The observed `UserMentionSuggester` contains an `LcListView` inside `ChatMessageView`. The bridge classified that suggestion list as messages and assigned it the message-list tag. It also intercepted editor Up/Down keys before LINE's own filtering could handle the suggestions.

The experiment excludes the mention subtree from bridge accessibility replacement, message-list classification, tagging, focus-policy changes and Tab ordering. When a mention popup is visible in the editor's own chat panel, editor keys are left to LINE's normal event filters. Mention interactions also release the bridge's browsing/focus guard. Other chat windows and ordinary editor operation remain outside that popup-specific condition.

No new settings switch was added for this experiment. Message rendering was left unchanged.

### Test result and limits

The user reported that @ selection works and that entering keywords successfully filters the member list. This was tested in one Windows/NVDA environment with a Traditional Chinese LINE interface and the bridge enabled. It is not a claim of testing Thai labels, every LINE version, independent windows, or every group type.

The experimental bridge compiled with Qt 6.6.3/MSVC Release. NVDA import/syntax checks and six existing message-action dispatcher tests passed. Those tests do not automate mention selection; the interaction result comes from the user's live test. The reference patch targets a local combined test baseline, including pending focus/navigation changes, and is not wired into this PR's production build.

## Possible future customization

A future bridge option could let users customize **control navigation order** to suit their workflow. This means keyboard navigation order, not rearranging the visual layout or rewriting messages. It is an idea for discussion, not implemented functionality or a prerequisite for the mention change. The relationship between native focus behavior and an optional user-defined order would need separate design and testing.

## Scope of this PR

This draft offers a development strategy and reference source for maintainer review. It does not replace the architecture, enable native message reading, alter calls/Notes/OpenChats, add navigation-order settings, or change LINE MR's version or release workflow.

The earlier [standalone native-message-actions example](../examples/native-message-actions/README.md) remains an optional reference, not a recommendation to abandon the bridge. Its own test limitations are documented there. No private logs, chat content, screenshots, diagnostic dumps or binaries are included.

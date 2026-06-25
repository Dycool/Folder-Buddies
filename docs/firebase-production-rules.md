# Firebase production rules

Folder Buddies uses Firebase Realtime Database only as an optional fallback
signaling backend. File contents must never be written to Firebase.

These are the tightest anonymous Realtime Database rules that still support the
current web fallback and native fallback:

- root reads/writes are denied;
- only `webRooms` and `nativeRooms` are usable;
- room keys must be URL/base64-safe short keys;
- unexpected fields are rejected;
- signaling messages are capped to 128 KiB strings;
- native room records can only be created once or deleted, not overwritten.

Paste the contents of `firebase.rules.json` into:

```text
Firebase Console → Realtime Database → Rules
```

## Important limitation

These rules are intentionally strict about shape and size, but they are still
anonymous rules. Without Firebase Auth or Firebase App Check, Realtime Database
rules cannot prove which browser/native app owns a room. That means anonymous
cleanup deletes are allowed so hosts can remove stale rooms.

For maximum abuse protection:

1. Put Cloudflare first and Firebase second, as the app already does.
2. Keep Firebase only as fallback signaling, not file storage.
3. Monitor Realtime Database usage.
4. Consider a separate Firebase project/database for web-only fallback if you
   later want to enforce Firebase App Check. Enforcing App Check on the same
   database will break the current native REST fallback unless native App Check
   support is added.

## Recommended database location

Use `europe-west1` / Belgium for Portugal/EU users.

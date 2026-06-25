# Firebase fallback setup

Folder Buddies can use Firebase Realtime Database as an optional automatic
signaling fallback when the Cloudflare Worker WebSocket path is unavailable.
Firebase is only used for encrypted WebRTC signaling messages. File bytes still
move over the WebRTC DataChannel between browsers.

## Recommended Firebase product

Use **Firebase Realtime Database**, not Cloud Firestore.

Reasons:

- Realtime Database is JSON + realtime sync, which maps cleanly to transient
  WebRTC offer/answer/ICE signaling.
- Spark/free plan supports 100 simultaneous Realtime Database connections,
  which is enough for fallback/dev/small public usage.
- Firestore bills/counts document reads/writes differently and is less natural
  for many tiny transient pub/sub-style signaling messages.

For Europe/Portugal, choose the Realtime Database region:

```text
Europe west1 / Belgium
```

Your database URL will look like:

```text
https://YOUR_DATABASE_NAME.europe-west1.firebasedatabase.app
```

## Firebase console steps

1. Go to https://console.firebase.google.com/.
2. Create a project, for example `folder-buddies-fallback`.
3. You can disable Google Analytics for this project.
4. Go to **Build → Realtime Database**.
5. Click **Create database**.
6. Choose **Belgium / europe-west1** if most users are in Europe.
7. Start in locked mode if offered; then paste the rules below.
8. Go to **Project settings → General → Your apps**.
9. Add a **Web app**.
10. Copy these values from the Firebase config snippet:

```js
const firebaseConfig = {
  apiKey: "...",
  authDomain: "...",
  databaseURL: "...",
  projectId: "...",
  appId: "..."
};
```

## GitHub Pages variables

In the GitHub repository, open:

```text
Settings → Secrets and variables → Actions → Variables
```

Add these repository variables:

```text
FIREBASE_API_KEY       = value of apiKey
FIREBASE_AUTH_DOMAIN   = value of authDomain
FIREBASE_DATABASE_URL  = value of databaseURL
FIREBASE_PROJECT_ID    = value of projectId
FIREBASE_APP_ID        = value of appId
```

These are public web config values, not secrets. They will be written into
`web/config.js` by `.github/workflows/webapp.yml` during the GitHub Pages build.

Then run:

```text
Actions → Webapp → Run workflow
```

After deploy, open the site and check in the browser console:

```js
window.FB_WEBAPP_CONFIG.firebase
```

If the fields are filled, the Firebase fallback is enabled.


## Production rules

For production, use the stricter rules in `firebase.rules.json` and see
`docs/firebase-production-rules.md`. The short testing rules below are only for
quick setup checks.

## Development rules

These are the simplest rules that let the fallback work:

```json
{
  "rules": {
    ".read": false,
    ".write": false,
    "webRooms": {
      ".read": true,
      ".write": true
    }
  }
}
```

This is acceptable for local testing and early demos because the payloads are
short-lived encrypted signaling messages, not file contents. Do not leave this as
the final long-term public configuration if the app becomes popular.

## Slightly stricter public fallback rules

These rules keep Firebase scoped to the `webRooms` subtree and reject unrelated
root writes. They still allow public anonymous signaling because the fallback is
supposed to work without accounts.

```json
{
  "rules": {
    ".read": false,
    ".write": false,
    "webRooms": {
      "$room": {
        ".read": true,
        ".write": true,
        "v": {
          ".validate": "newData.isNumber() && newData.val() === 1"
        },
        "host": {
          "createdAt": {
            ".validate": "newData.isNumber()"
          },
          "$other": {
            ".validate": false
          }
        },
        "clients": {
          "$clientId": {
            "joinedAt": {
              ".validate": "newData.isNumber() || newData.val() === null"
            },
            "$other": {
              ".validate": false
            }
          }
        },
        "signalsToHost": {
          "$msg": {
            "peerId": {
              ".validate": "newData.isString() && newData.val().length <= 80"
            },
            "ciphertext": {
              ".validate": "newData.isString() && newData.val().length <= 262144"
            },
            "at": {
              ".validate": "newData.isNumber()"
            },
            "$other": {
              ".validate": false
            }
          }
        },
        "signalsToClient": {
          "$clientId": {
            "$msg": {
              "ciphertext": {
                ".validate": "newData.isString() && newData.val().length <= 262144"
              },
              "at": {
                ".validate": "newData.isNumber()"
              },
              "$other": {
                ".validate": false
              }
            }
          }
        },
        "$other": {
          ".validate": false
        }
      }
    }
  }
}
```

If these stricter rules block a Firebase SDK server timestamp during testing,
fall back to the development rules first, verify the signaling path works, then
iterate on validation.

## Cleanup behavior

The browser calls `onDisconnect().remove()` for the fallback room/client entries.
When the host disconnects cleanly, the room should disappear. If a browser or
network dies badly, stale nodes can still happen. Because this fallback stores no
file data and only encrypted signaling, stale nodes are mostly quota/noise, not a
file privacy leak.

For public production, add one of these cleanup strategies:

- periodic manual cleanup in the Firebase console;
- a scheduled Cloud Function on Blaze;
- client-side opportunistic cleanup of expired `webRooms` when hosting starts.

## Runtime order

The web client attempts signaling in this order:

```text
1. Cloudflare Worker WebSocket
2. Firebase Realtime Database fallback, if configured
3. Manual FBW2O/FBW2A giant-code WebRTC fallback
```

If Firebase variables are empty, the app skips Firebase and goes directly from
Cloudflare to the manual fallback.

## Native app fallback

The native app can also use the same Realtime Database project as an automatic
fallback for 6-character native room codes. Native fallback records are stored
under `nativeRooms`, not `webRooms`, because native clients use the direct TCP
mount protocol while web clients use WebRTC.

Configure the native app with either:

```bash
export FOLDERBUDDIES_FIREBASE_DATABASE_URL="https://YOUR_DATABASE_NAME.europe-west1.firebasedatabase.app"
```

or build with:

```bash
cmake -S . -B build -DFB_FIREBASE_DATABASE_URL="https://YOUR_DATABASE_NAME.europe-west1.firebasedatabase.app"
```

Native host order:

```text
1. Cloudflare KV room
2. Firebase Realtime Database nativeRooms fallback
3. Long offline secure code
```

Native connect order for a 6-character code:

```text
1. Cloudflare lookup
2. Firebase nativeRooms lookup
```

If the user pastes the long offline code, no Cloudflare/Firebase lookup is used.

## Rules including native fallback

For testing both web and native fallback, use:

```json
{
  "rules": {
    ".read": false,
    ".write": false,
    "webRooms": {
      ".read": true,
      ".write": true
    },
    "nativeRooms": {
      ".read": true,
      ".write": true
    }
  }
}
```

`nativeRooms` contains only encrypted connection tokens: salt, wrapped key,
payload, owner, and timestamps. It does not contain file bytes. If the app grows,
replace these broad testing rules with stricter validation and scheduled cleanup.

## Web/native compatibility note

The room-code and fallback strategy are now aligned, but the data transports are
still different:

- web hosts/clients use WebRTC RTCDataChannel;
- native hosts/clients use direct encrypted TCP plus the mounted filesystem protocol.

A native client cannot mount a browser-hosted folder, and a browser client cannot
browse a native-hosted TCP share without adding a real shared transport layer,
such as native WebRTC support or an explicit encrypted relay/bridge. The current
code detects this by code type instead of pretending the transports are compatible.

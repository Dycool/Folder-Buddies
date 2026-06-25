# Folder Buddies Cloudflare signaling

The Worker exposes only:

- `POST /create`
- `GET /room?code=<6-char Base91>`
- `DELETE /room?code=<6-char Base91>`

Native app mode uses KV: the 6-character room code is the key and the encrypted payload is the value. It never stores plaintext IP, port, folder name, filesystem secret, or plaintext password.

Webapp mode reuses `GET /room` as a WebSocket upgrade. The Worker routes each room to a Durable Object that forwards encrypted WebRTC signaling messages between host and client. It does not store or relay file bytes.

## Public repository safety

Do **not** commit Cloudflare API tokens, account IDs, or KV namespace IDs. The checked-in `wrangler.toml` intentionally contains placeholders so the repository can stay public.

Use GitHub repository secrets for deployment:

- `CLOUDFLARE_API_TOKEN`
- `CLOUDFLARE_ACCOUNT_ID`
- `CF_KV_ROOMS_ID`
- `CF_KV_ROOMS_PREVIEW_ID`

The Durable Object binding and migration are public config and do not require secrets.

The workflow generates `cloudflare/wrangler.ci.toml` at runtime from those secrets. That file is ignored and must never be committed.

## One-time Cloudflare setup

From your own machine, after logging into Wrangler:

```sh
cd cloudflare
npx wrangler kv namespace create ROOMS
npx wrangler kv namespace create ROOMS --preview
```

Copy the two namespace IDs into GitHub Secrets as `CF_KV_ROOMS_ID` and `CF_KV_ROOMS_PREVIEW_ID`. Do not paste them into `wrangler.toml` for the public repo.

## Manual deploy from GitHub Actions

The repository includes `.github/workflows/cloudflare-worker.yml`.

Validation runs on pull requests and pushes touching `cloudflare/**`:

- public-safe `wrangler.toml` guard
- `node --check cloudflare/worker.mjs`
- C++23 build of `cloudflare/worker_core.cpp`
- Wrangler dry-run when secrets are available

Deployments are manual by default:

1. Open the **Cloudflare Worker** workflow in GitHub Actions.
2. Click **Run workflow**.
3. Set `deploy=true`.
4. Approve the `cloudflare-production` environment if you enabled required reviewers.

## App endpoint

After deploy, set the public Worker URL as the repository variable `FB_SIGNALING_URL` so release builds hardcode it:

```text
https://folderbuddies-signaling.<your-subdomain>.workers.dev
```

For local testing without rebuilding, set `FOLDERBUDDIES_SIGNALING_URL` in the environment.


## Webapp signaling

The static webapp connects to:

```text
wss://<worker-host>/room?code=<6-char-room>&role=host&web=1
wss://<worker-host>/room?code=<6-char-room>&role=client&web=1
```

The Durable Object sees only room codes, peer IDs, and opaque encrypted signaling blobs. SDP offers/answers are encrypted in the browser with the room password before being sent to Cloudflare. Actual folder data moves over WebRTC DataChannel directly between browsers.

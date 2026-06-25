# Cloudflare production hardening

Folder Buddies uses Cloudflare as a signaling service only. File bytes should
not pass through Cloudflare in the normal browser path.

The Worker now has these built-in protections:

- optional origin allowlist via `ALLOWED_ORIGINS`;
- KV-backed per-IP rate limits;
- KV-backed per-room rate limits;
- max JSON create body size;
- max individual field sizes;
- max WebSocket signaling message size;
- max clients per web room;
- max signaling messages per WebSocket;
- Turnstile server-side validation when `TURNSTILE_SECRET_KEY` is configured;
- `DELETE /room` requires the host owner token in `X-FB-Owner` for native KV rooms.

## Recommended Worker variables

Set these in Cloudflare Worker variables/secrets:

```text
TURNSTILE_SECRET_KEY = your Turnstile secret key
ALLOWED_ORIGINS      = https://YOUR_GITHUB_USERNAME.github.io,https://your-domain.example
```

Do not put `TURNSTILE_SECRET_KEY` in GitHub Variables. It belongs in Cloudflare
Worker secrets.

## Recommended Cloudflare dashboard rules

The Worker has app-level rate limits, but dashboard-side WAF/rate-limit rules
are still useful because they stop traffic before it reaches your Worker.

Suggested starting point:

```text
Path contains /room or /create:
  30 requests per minute per IP → block/challenge for 10 minutes

Path contains /create:
  5 requests per minute per IP → block/challenge for 10 minutes
```

Tune these if real users hit false positives.

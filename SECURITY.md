# Security and public repository rules

This repository is intended to be safe to keep public. Treat every committed file
as permanently public.

## Never commit

- Cloudflare API tokens
- Cloudflare account IDs
- Cloudflare KV namespace IDs
- Cloudflare Turnstile secret keys
- Apple signing certificates or passwords
- `.env` files
- private keys, certificates, provisioning profiles, or local assistant/editor state

The checked-in `cloudflare/wrangler.toml` intentionally contains placeholders.
GitHub Actions creates `cloudflare/wrangler.ci.toml` at runtime from encrypted
repository secrets. That generated file is ignored and must never be committed.

## Required GitHub Secrets

For Cloudflare deployment, configure these as **GitHub repository secrets**, not
repository variables and not committed files:

- `CLOUDFLARE_API_TOKEN`
- `CLOUDFLARE_ACCOUNT_ID`
- `CF_KV_ROOMS_ID`
- `CF_KV_ROOMS_PREVIEW_ID`
- `TURNSTILE_SECRET_KEY` if Turnstile is enabled

For app builds, `FB_SIGNALING_URL` and `TURNSTILE_SITE_KEY` may be repository variables because the final webapp/client needs public configuration anyway. Do not put passwords, tokens, Turnstile secret keys, or private infrastructure credentials there.

## CI safety

`.github/workflows/security.yml` fails the build if common secret files are
committed or if `cloudflare/wrangler.toml` is changed to contain real Cloudflare
IDs/tokens.

The Cloudflare deployment job is manual by default and uses the
`cloudflare-production` GitHub Environment. Enable required reviewers on that
environment if you want an approval gate before production deploys.

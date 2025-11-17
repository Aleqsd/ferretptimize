# Expert Mode: Implementation Steps

Use this runbook to build and launch Expert Mode end-to-end. Follow the order; do not skip the test matrix (`make`, `make test`, `make autotest`) after touching `src/` or `include/`.

## 0) Foundations ✅
- Enabled secrets/config plumbing: FP_GOOGLE_CLIENT_ID, FP_FACEBOOK_APP_ID, FP_JWT_SECRET, FP_STRIPE_SECRET_KEY placeholders, DB DSN via `FP_DB_DSN`, and interim gate `FP_EXPERT_API_KEYS`/`FP_EXPERT_API_KEY`.
- Picked control-plane storage: SQLite default (`expert_auth.db`) with override to Postgres/other via DSN env.
- Scaffolded tables in SQLite: `users`, `sessions`, `api_keys`, `subscriptions`, `audit`.

## 1) Auth + Sessions ✅
- Implemented Google + Facebook OAuth callbacks that upsert user record on callback and emit audit entries.
- Issued JSON access token + refresh token (HttpOnly, Secure, SameSite=Lax cookies) with refresh stored hashed in DB.
- Added API-key issuance endpoint (`POST /api/keys`) with hashed keys at rest scoped to Expert entitlements.
- Kept interim gate active: `POST /api/expert/compress` still enforces `Authorization: ApiKey <token>` when `FP_EXPERT_API_KEY(S)` is set; DB-backed keys are also accepted.

## 2) Stripe Integration ✅
- Create prices: `price_expert_monthly=$2` and `price_expert_annual=$20` (FP_STRIPE_PRICE_MONTHLY/FP_STRIPE_PRICE_ANNUAL override defaults).
- Build Checkout session endpoint + “Manage billing” link (Customer Portal) — stubbed locally at `/api/stripe/checkout` and `/api/stripe/portal`, with FP_CHECKOUT_BASE_URL/FP_PORTAL_BASE_URL hooks for real Stripe.
- Handle webhooks: `checkout.session.completed`, `invoice.payment_succeeded`, `customer.subscription.updated/deleted`; sync subscription status + period end into DB and audit (stubbed at `/webhook/stripe`).
- On subscription loss: revoke entitlements + API key usage; surface banner in UI (DB API keys/JWT require active subscription; env API keys remain for dev).

## 3) Control-Plane Middleware ✅
- Middleware gates Expert endpoints via JWT/API key with active subscription (env keys allowed for dev); emits clear deny reasons.
- Request guards shipped: max 10 files, 20 MB/file, 100 MB total, per-user daily job/byte quotas; rejects malformed multipart/metadata.
- Request metrics/audit logged (counts, bytes, elapsed, source).

## 4) Expert API Surface ✅
- `/api/expert/compress` proxy implemented.
- Accepts `multipart/form-data` with `files[]` and per-file `metadata{N}`/`metadata[N]` JSON; normalizes format/quality/trim/crop options before C backend.
- Response encodes per-file outputs `{format,label,size_bytes,data,params_used}` plus flags for trim/crop, bytes_saved per file, and aggregate `bytes_saved`, `elapsed_ms`, totals.
- `/api/compress` unchanged; Expert allows higher guarded limits.

## 5) C Backend Enhancements
- Add handler for Expert requests from control-plane, parsing normalized metadata.
- Extend encoders: PNG level/filter, WebP quality/lossless, AVIF speed/cq; hooks for JPEG/HEIC (feature-flag, link libjpeg-turbo/libheif when available).
- Implement preprocessing: whitespace trim (detect opaque bounds) and crop (x, y, width, height, optional anchor).
- Keep multi-image support and aggregate ETA/output handling.

## 6) Frontend & UX
- Add OAuth login entry points + logged-in indicator.
- Expert modal/panel: format selectors, quality/speed sliders, trim/crop toggles, multi-file picker with per-file overrides; disable for free users with informative tooltips.
- User panel: show subscription status, usage stats, API key create/revoke, and “Manage billing” button to Stripe Portal.
- Messages for entitlement loss or quota errors.

## 7) Observability & Security
- Hash API keys (e.g., SHA-256 + salt); never log raw keys; support rotation.
- Validate and clamp all params server-side; reject malformed metadata.
- Verify Stripe webhook signatures; make handlers idempotent and retry-safe.
- CORS limited to allowed origins; HTTPS everywhere.

## 8) Testing & Rollout
- For any change under `src/` or `include/`: run `make`, `make test`, `make autotest` (autotest covers Expert auth + crop path).
- Add control-plane/unit tests for auth, Stripe webhook handling, gating, and payload normalization.
- Stage rollout: Phase 1 (auth + Stripe + DB + stubbed responses), Phase 2 (proxy to current compress), Phase 3 (multi-image + params + new formats), Phase 4 (frontend polish + panels), Phase 5 (beta with trial, monitor, tune limits).
- Prepare incident playbooks: rate-limit spikes, Stripe webhook failures, auth provider outages.

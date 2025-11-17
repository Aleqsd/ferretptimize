# Expert Mode Implementation Plan

## Outcomes
- Paid subscription tier unlocking: API access, custom compression parameters, extra formats, trim/crop utilities, and multi-image uploads.
- Auth via Google/Facebook OAuth, with a basic user panel for subscription status and Stripe-powered billing management (monthly or annual).
- Backward-compatible with the existing `/api/compress` flow and frontend; Expert features are gated and observable.

## Pricing & Packaging
- Propose $2/month or $20/year (~2 months free) for Expert mode; start with a 7-day trial to reduce friction.
- Keep current free tier limited to single-image UI uploads with default presets.
- Gate Expert entitlements by user_id/subscription status; log audit events for billing disputes.

## Architecture Shape
- Preserve the C compression service and add a thin C-based control plane to handle OAuth, Stripe, API key issuance, and feature gating (no new languages/frameworks).
- Frontend calls the control plane for auth/billing and forwards signed requests to the C backend for compression.
- Introduce a DB (managed Postgres or SQLite to start) for users, sessions, API keys, and subscription state mirrored from Stripe webhooks.
- Offer a JWT-based session for the web UI plus static API keys for server-to-server use; both map to user records with entitlements.

## Authentication & Sessions
- Implement Google and Facebook OAuth (OIDC where available) via the control-plane; store provider, provider_user_id, email, and basic profile.
- After OAuth callback, create or update the user row and issue a short-lived JWT + refresh token cookie (HttpOnly, Secure, SameSite=Lax).
- Add an API key issuance endpoint in the user panel; keys are hashed at rest and scoped to Expert entitlements.
- Interim gate (implemented): set `FP_EXPERT_API_KEYS` (comma separated) or `FP_EXPERT_API_KEY` on the server; `POST /api/expert/compress` requires `Authorization: ApiKey <token>` when a key is configured, otherwise the endpoint remains open for local/dev usage.

## Stripe Integration
- Create two Stripe Prices: `price_expert_monthly` ($2) and `price_expert_annual` ($20). Use Stripe Checkout for purchase.
- Handle `checkout.session.completed`, `invoice.payment_succeeded`, and `customer.subscription.updated/deleted` webhooks to sync subscription status and period end into the DB.
- Expose a “Manage billing” button that links to Stripe Customer Portal to update payment methods, cancel, or switch plans.
- On subscription loss, immediately revoke Expert entitlements and API key usage; surface banner in UI.

## Gating & Quotas
- Middleware checks JWT/API key and subscription status before hitting Expert endpoints.
- Enforce sensible bounds: max files per request (e.g., 10), per-file size (e.g., 20 MB), total payload cap, and per-user daily job caps.
- Record request metrics (counts, bytes in/out, latencies) for audit and tuning.

## Expert API Surface
- New endpoint: `POST /api/expert/compress` (via control-plane -> C backend).
- Request: `multipart/form-data` with `files[]` for images; optional `metadata` JSON part for per-file instructions. Headers: `Authorization: Bearer <JWT>` or `Authorization: ApiKey <key>`.
- Supported operations:
  - Format control: PNG, WebP, AVIF; plan hooks to add JPEG and HEIC later behind feature flags.
  - Parameter control: per-format quality/effort knobs (e.g., PNG compression level/filter, WebP quality/lossless flag, AVIF cq-level/speed).
  - Trim/crop: `trim: {mode: "whitespace", tolerance: float}` and `crop: {x,y,width,height, anchor?}` applied before compression.
  - Multi-image: multiple files per request, each with its own instruction block.
- Response: JSON with an array of results `{original_filename, outputs: [{format, size_bytes, base64_data, params_used}], trims_applied}` and aggregate stats (bytes_saved, elapsed_ms).
- Backward compatibility: keep `/api/compress` unchanged; optionally allow it under Expert auth for higher limits but defaults apply when unauthenticated.

## C Backend Changes
- Add a new handler for Expert requests that accepts multipart and metadata parsing (consider a thin parser in the control-plane, sending a normalized payload to the C service to minimize C parsing complexity).
- Extend the compression pipeline to support:
  - PNG parameterization (level 1–9, filter strategy).
  - WebP lossy/lossless quality.
  - AVIF speed/cq-level.
  - Additional encoders for JPEG and HEIC (link against libjpeg-turbo and libheif if available; feature-flag per build).
- Implement pre-processing steps: whitespace trim (detect non-transparent/opaque bounds) and crop (bounding box).

## Frontend & User Panel
- Add login (Google/Facebook) entry points and session indicator.
- Expert toggle panel: advanced sliders (quality, speed), format selectors, trim/crop controls, and multi-file picker with per-file overrides.
- User panel: subscription status, usage stats, API key management (create/disable), and “Manage billing” linking to Stripe Customer Portal.
- UI should gracefully degrade for free users (controls disabled with tooltip explaining Expert).

## Operational Steps
- Phase 1: stand up C-control-plane with OAuth + Stripe + DB; gate API with stubbed responses.
- Phase 2: wire control-plane to existing C backend for default compress; add auth gating.
- Phase 3: implement multi-image + parameterized compression in C; add new formats and preprocessing.
- Phase 4: polish frontend (Expert controls, user panel), add metrics, and pen-test auth/billing flows.
- Phase 5: launch beta with trial, monitor errors/latency, and tune limits.

## Security & Compliance
- Store API keys hashed (e.g., SHA-256 + salt), never log raw keys; rotate on demand.
- Validate and clamp all parameters server-side; reject malformed metadata.
- Use HTTPS everywhere; set CORS to only allowed origins for API usage.
- Follow Stripe’s webhook signature verification; retry-safe webhook handlers.

## Quotas & Keys
- Smart quotas: cap per-request file count/size (e.g., 10 files, 20 MB each, 100 MB aggregate) plus daily per-user job/byte caps with soft limits and informative errors.
- API keys are per user; multiple keys per account are allowed for rotation but all map to the same user entitlements.
- No usage-based overage billing; access is covered by the flat subscription.

## Upcoming Features
- See `Features.md` for the next set of Expert-mode capabilities to stage after launch (pipelines, scheduling/webhooks, storage targets, watermarking, visual QA).

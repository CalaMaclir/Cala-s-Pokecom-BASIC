# Stable update feed

`stable.json` describes the existing accepted public Release using `cpb-update-v1`. It is metadata only: releases, assets, immutable tags and hardware-acceptance requirements are unchanged.

The `update-stable-feed` workflow runs after a successful main-push `publish-release`, when its own metadata code changes, or by manual dispatch. Trusted main code verifies the configured latest non-draft, non-prerelease Release, owner-acceptance record, tag, SHA256SUMS.txt and actual UF2 size/hash, then atomically updates only stable.json. Backward pointers are refused. A failed validation preserves the previous feed. The GitHub token needs Contents write in this repository only; it never enters the firmware.

The current v0.94 Build 878 is correctly listed even when a newer client refuses installation because saved-data or image identity compatibility has not been established. A feed entry is not permission to skip client-side compatibility checks.

For the next formally approved release, retain the existing publication procedure and its gates. Update current-publication.json and the acceptance record through that procedure; the metadata workflow follows successful publication. Re-run this workflow to recover a metadata-only failure. Do not create or retag a release merely to refresh this feed.

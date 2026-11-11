# Streaming destinations

Encoder Flow → destination → **Destination type** selects a streaming service or custom RTMP,
SRT, or WHIP destination. RTMP accepts `rtmp://` and `rtmps://` addresses. Service destinations
provide OBS's server list and stream-key setup link. Switching services clears the credential
reference and disarms the output. SRT and WHIP transport remain unavailable.

## Account authorization

Twitch and YouTube support **Connect account**. Manual stream keys work without OAuth client
registration. Install `secret-tool` and unlock a Secret Service keyring before saving keys.

- **Twitch:** register a **public** application at <https://dev.twitch.tv/console/apps>.
  Enter its client ID in the destination's **OAuth client ID** field. MunkiCast uses Twitch's
  device authorization flow with `channel:read:stream_key`; approve the displayed code in the
  browser. Twitch may require two-factor authentication to retrieve the stream key.
- **YouTube:** enable **YouTube Data API v3** in your Google Cloud project. Configure the OAuth
  consent screen, adding test users while the app is in testing. Create a **Desktop app** OAuth
  client. Enter its ID and secret in the destination fields. The secret stays in memory for the
  current application session. Authorize in your system browser, then select an existing stream.
  The account must have live streaming enabled; create a stream in YouTube Studio first.
  Google authorization uses a loopback callback, state validation, PKCE, and `youtube.readonly`.

Client IDs are saved in local Qt settings, outside projects. Alternatively configure the process
with `MUNKICAST_TWITCH_CLIENT_ID`, `MUNKICAST_GOOGLE_CLIENT_ID`, and
`MUNKICAST_GOOGLE_CLIENT_SECRET`; environment client IDs take precedence over the fields.

Only the retrieved stream key is saved, in the system keyring. Access tokens are discarded after
retrieval; refresh tokens are neither requested nor saved. Reconnect when a provider rotates the
stream key. The project stores the catalog ID, ingest address, and keyring reference, never OAuth
credentials or stream keys. Authorization does not start a broadcast.

Real-provider login requires your registered clients and interactive consent; the headless checks
exercise catalog selection, dynamic fields, serialization, disarming, and undo without logging in.

## Catalog provenance

`apps/munkicast-ui/data/obs-services.json` is a snapshot of
`plugins/rtmp-services/data/services.json` from OBS Studio, commit
`f1c0a116441bdb7a1de7896f810009ea487b8508`, copied from the local OBS checkout.
OBS Studio contributors; GPL-2.0-or-later, compatible with this project's GPL-3.0-or-later license.
The file is bundled as a Qt resource; the application does not depend on the OBS checkout.

Only RTMP services with static ingest servers are offered. OBS's YouTube HLS, Dacast, and
SHOWROOM entries need additional output or service API support. Catalog
presence supplies configuration defaults, not a claim that every service has been tested. OBS's
OAuth client IDs, obfuscated secrets, and hosted OAuth backend are not reused.

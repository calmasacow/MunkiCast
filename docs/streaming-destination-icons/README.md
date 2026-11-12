# Streaming destination icon starter pack

This package covers all **75 distinct service entries** visible in the screenshots. RTMP/SRT/WHIP are transport/protocol choices and not streaming-platform logos, so they are not included.

## Contents
- `png-64/`, `png-128/`: 64x64 and 128x128 PNG icons, transparent where possible
- `svg/`: SVG versions of the same icon artwork
- `destinations.csv`: exact list, domains, shutdown/migration flags, artwork provenance
- `fetch-favicons.py`: optional updater to download site favicons to `fetched-favicons/` on a network-connected machine

## IMPORTANT: genuine vs placeholder

This is a **starter icon pack**, not 75 verified official brand logos. **9 entries** use existing Font Awesome Free Brand vector glyphs (which can be older variants); the remaining **66** use custom lettermark placeholders. The CSV marks each one clearly. Generated badges are not affiliated with or endorsed by the listed services and should not be mistaken for their logos.

If you need real icons for every service, run the optional favicon script on an Internet-connected machine; it fetches site-published favicon artwork where available and *does not invent logos*. A favicon can differ from a service's official full logo. Many entries in the OBS registry are specialized video backends, not public social networks.

## Known closures and naming issues as of 2026-11-11

- **KakaoTV**: fully shut down 2026-06-30 (official KakaoTV notice)
- **Trovo**: live-streaming ended 2026-06-30; gaming features/site may continue
- **iNSTAGIB**: standalone platform merged into **Vaughn Live**; legacy RTMP keys expired 2021-03-01
- **SOOP**: 2026 Korea/global consolidation, **NOT** an overall shutdown
- **Dolby OptiView Real-time**: renamed/rebranded from Dolby Millicast, **NOT** a shutdown
- **PhoneLiveStreaming**: now branded **PhoneLive** at phonelive.io, **NOT** a shutdown
- **PandaTV (팬더티비)** in this list is the **Korean** PandaTV and is **not** the defunct Chinese Panda.tv
- Other small services have not been confirmed shut down and are marked for checking rather than deleted automatically.

Sources:
- https://kakao-tv.tistory.com/323
- https://streamscharts.com/news/trovo-shut-down
- https://instagib.tv/gtajeffery
- https://sooplive.helpshift.com/hc/en/3-soop/faq/106-soop-service-integration-notice-jan-2026/
- https://phonelive.io/about/
- https://github.com/obsproject/obs-studio/blob/master/plugins/rtmp-services/data/services.json

Font Awesome Free Brands: CC BY 4.0 (logo artwork sourced from installed local package). Trademarks remain those of their owners. Brand logos may require complying with individual brand guidelines.

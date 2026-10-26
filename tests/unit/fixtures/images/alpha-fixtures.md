# Alpha Media Fixtures

Generated test artwork: transparent background and half-transparent box with changing hue.
No third-party artwork. Recreate from repository root:

```sh
ffmpeg -f lavfi -i 'color=red@0.0:size=32x24:rate=10:duration=1,format=rgba,drawbox=x=8:y=4:w=12:h=12:color=red@0.5:t=fill:replace=1,hue=h=360*t' -c:v libvpx-vp9 -pix_fmt yuva420p -auto-alt-ref 0 tests/unit/fixtures/images/alpha.webm
ffmpeg -f lavfi -i 'color=red@0.0:size=1080x1920:rate=10:duration=1,format=rgba,drawbox=x=360:y=640:w=360:h=640:color=red@0.5:t=fill:replace=1,hue=h=360*t' -c:v libvpx-vp9 -pix_fmt yuva420p -auto-alt-ref 0 tests/unit/fixtures/images/alpha-portrait.webm
ffmpeg -f lavfi -i 'color=red@0.0:size=32x24:rate=60:duration=1,format=rgba,drawbox=x=8:y=4:w=12:h=12:color=red@0.5:t=fill:replace=1,hue=h=360*t' -c:v libvpx-vp9 -pix_fmt yuva420p -auto-alt-ref 0 tests/unit/fixtures/images/alpha-60.webm
ffmpeg -c:v libvpx-vp9 -i tests/unit/fixtures/images/alpha.webm -frames:v 1 -c:v libwebp -lossless 1 tests/unit/fixtures/images/alpha.webp
ffmpeg -c:v libvpx-vp9 -i tests/unit/fixtures/images/alpha.webm -c:v libwebp_anim -lossless 1 -loop 0 tests/unit/fixtures/images/animated-alpha.webp
ffmpeg -i tests/unit/fixtures/images/alpha.webm -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' -c:v copy -c:a libopus -shortest tests/unit/fixtures/images/alpha-tone.webm
```

Worker checks verify partial alpha, animation, looping, and audio transport. GPU checks
replace a cached mask with another WebP and compare output against CPU rendering.

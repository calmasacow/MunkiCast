#!/usr/bin/env python3
"""Fetch site-published favicon art for destinations.csv.

Saves PNG and SVG into ``New Icons To process/`` (original pack is untouched).

Usage:
    uv run --with requests --with beautifulsoup4 --with pillow --with cairosvg \\
        python fetch-favicons.py

Favicons are not guaranteed full official logos. Some sites block automated retrieval.
Native SVG is kept when the site publishes one; raster icons are wrapped in an SVG.
"""
from __future__ import annotations

import argparse
import base64
import csv
import io
import re
from pathlib import Path
from urllib.parse import urljoin, urlparse

import requests
from bs4 import BeautifulSoup
from PIL import Image

ROOT = Path(__file__).resolve().parent
DEFAULT_CSV = ROOT / "destinations.csv"
DEFAULT_OUT = ROOT / "New Icons To process"
SIZE = 128
UA = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)


def session() -> requests.Session:
    s = requests.Session()
    s.headers.update({"User-Agent": UA, "Accept": "*/*"})
    return s


def is_svg_bytes(content: bytes, content_type: str) -> bool:
    if "svg" in (content_type or "").lower():
        return True
    head = content.lstrip()[:200].lower()
    return head.startswith(b"<svg") or (head.startswith(b"<?xml") and b"<svg" in content[:800].lower())


def icon_candidates(html: str, page_url: str) -> list[str]:
    soup = BeautifulSoup(html, "html.parser")
    scored: list[tuple[int, str]] = []
    for link in soup.find_all("link", href=True):
        rel = " ".join(link.get("rel") or []).lower()
        href = link["href"].strip()
        if not href or not any(k in rel for k in ("icon", "apple-touch-icon", "mask-icon", "fluid-icon")):
            continue
        url = urljoin(page_url, href)
        sizes = (link.get("sizes") or "").lower()
        score = 0
        if "svg" in href.lower() or "svg" in (link.get("type") or "").lower():
            score += 1000
        if "apple-touch" in rel:
            score += 200
        nums = [int(n) for n in re.findall(r"(\d+)", sizes)]
        if nums:
            score += max(nums)
        if "shortcut" in rel:
            score += 10
        scored.append((score, url))
    scored.sort(key=lambda x: x[0], reverse=True)
    urls = [u for _, u in scored]
    for fallback in ("/apple-touch-icon.png", "/apple-touch-icon-precomposed.png", "/favicon.ico", "/favicon.png", "/favicon.svg"):
        urls.append(urljoin(page_url, fallback))
    # de-dupe, keep order
    seen: set[str] = set()
    out: list[str] = []
    for u in urls:
        if u not in seen:
            seen.add(u)
            out.append(u)
    return out


# CSV domain -> working public site when the listed host is a CDN or dead.
DOMAIN_ALIASES = {
    "lahzecdn.com": "lahzenegar.com",
}

def icons_from_site(s: requests.Session, domain: str) -> list[str]:
    domain = DOMAIN_ALIASES.get(domain, domain)
    urls: list[str] = []
    for host in (domain, f"www.{domain}" if not domain.startswith("www.") else domain):
        for scheme in ("https", "http"):
            base = f"{scheme}://{host}"
            try:
                r = s.get(base, timeout=12, allow_redirects=True)
                if r.status_code >= 400:
                    continue
                urls.extend(icon_candidates(r.text, r.url))
                if urls:
                    return urls
            except requests.RequestException:
                continue
    # last-resort public favicon caches
    urls.append(f"https://www.google.com/s2/favicons?sz=128&domain_url={domain}")
    urls.append(f"https://icons.duckduckgo.com/ip3/{domain}.ico")
    return urls


def rasterize_svg(content: bytes) -> bytes | None:
    try:
        import cairosvg

        return cairosvg.svg2png(bytestring=content, output_width=SIZE, output_height=SIZE)
    except Exception:
        return None


def png_128(data: bytes) -> Image.Image | None:
    try:
        img = Image.open(io.BytesIO(data))
        if getattr(img, "n_frames", 1) > 1 and img.format == "ICO":
            best = None
            best_area = -1
            for i in range(img.n_frames):
                img.seek(i)
                area = img.size[0] * img.size[1]
                if area > best_area:
                    best = img.copy()
                    best_area = area
            img = best if best is not None else img
        img = img.convert("RGBA")
        img.thumbnail((SIZE, SIZE), Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        canvas.alpha_composite(img, ((SIZE - img.width) // 2, (SIZE - img.height) // 2))
        return canvas
    except Exception:
        return None


def png_to_wrapped_svg(png_bytes: bytes) -> str:
    b64 = base64.b64encode(png_bytes).decode("ascii")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">\n'
        f'  <image width="{SIZE}" height="{SIZE}" href="data:image/png;base64,{b64}"/>\n'
        f"</svg>\n"
    )


def maybe_normalize_svg(content: bytes) -> bytes:
    text = content.decode("utf-8", errors="replace")
    if "viewBox" not in text and re.search(r"<svg\b", text, re.I):
        text = re.sub(
            r"<svg\b",
            f'<svg viewBox="0 0 {SIZE} {SIZE}" width="{SIZE}" height="{SIZE}"',
            text,
            count=1,
            flags=re.I,
        )
    return text.encode("utf-8")


def fetch_one(s: requests.Session, row: dict, out: Path) -> str:
    slug = row["slug"]
    png_path = out / f"{slug}.png"
    svg_path = out / f"{slug}.svg"
    last_err = "no icon urls"
    for url in icons_from_site(s, row["domain"]):
        try:
            r = s.get(url, timeout=12, allow_redirects=True)
            if r.status_code != 200 or not r.content or len(r.content) > 4_000_000:
                last_err = f"http {r.status_code}"
                continue
            ctype = r.headers.get("Content-Type", "")
            if is_svg_bytes(r.content, ctype):
                raster = rasterize_svg(r.content)
                img = png_128(raster) if raster else None
                if img is None:
                    last_err = "svg rasterize failed"
                    continue
                buf = io.BytesIO()
                img.save(buf, format="PNG")
                png_path.write_bytes(buf.getvalue())
                svg_path.write_bytes(maybe_normalize_svg(r.content))
                return f"saved svg from {urlparse(url).path or url}"
            img = png_128(r.content)
            if img is None:
                last_err = "not an image"
                continue
            buf = io.BytesIO()
            img.save(buf, format="PNG")
            png_bytes = buf.getvalue()
            png_path.write_bytes(png_bytes)
            svg_path.write_text(png_to_wrapped_svg(png_bytes), encoding="utf-8")
            return f"saved png from {urlparse(url).path or url}"
        except (requests.RequestException, ValueError, OSError) as e:
            last_err = str(e)
            continue
    return f"not available ({last_err})"


def main() -> int:
    p = argparse.ArgumentParser(description="Fetch destination favicons as PNG + SVG")
    p.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    p.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with args.csv.open(encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    s = session()
    saved = 0
    for i, row in enumerate(rows, 1):
        status = fetch_one(s, row, args.out)
        if status.startswith("saved"):
            saved += 1
        print(f'[{i:02d}/{len(rows)}] {row["name"]}: {status}')
    print(f"Saved {saved}/{len(rows)} into {args.out}")
    return 0 if saved else 1


if __name__ == "__main__":
    raise SystemExit(main())

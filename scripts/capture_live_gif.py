#!/usr/bin/env python3
"""Screenshots the live shadow page over time and writes a GIF.

Used to produce docs/live_shadow.gif. It drives a real headless browser against
a real running instance, so what ends up in the README is the page as it was,
not a mock of it.

    pip install playwright && playwright install chromium
    scripts/run_live.sh &                       # or the pipeline by hand
    python3 scripts/capture_live_gif.py --seconds 40 --out docs/live_shadow.gif
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:8080/")
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--fps", type=float, default=1.5)
    ap.add_argument("--width", type=int, default=1000)
    ap.add_argument("--scheme", default="dark", choices=["dark", "light"])
    ap.add_argument("--out", default="docs/live_shadow.gif")
    ap.add_argument("--max-colors", type=int, default=96)
    args = ap.parse_args()

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        sys.exit("pip install playwright && playwright install chromium")
    if not shutil.which("ffmpeg"):
        sys.exit("needs ffmpeg")

    tmp = tempfile.mkdtemp(prefix="ltxgif-")
    n = 0
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": args.width, "height": 960},
                                color_scheme=args.scheme, device_scale_factor=1)
        page.goto(args.url, wait_until="domcontentloaded")
        # Wait for the first poll to land, so frame one is not the empty state.
        page.wait_for_timeout(1500)
        deadline = time.time() + args.seconds
        interval = 1.0 / args.fps
        while time.time() < deadline:
            t0 = time.time()
            page.screenshot(path=os.path.join(tmp, f"f{n:04d}.png"), full_page=True)
            n += 1
            sleep = interval - (time.time() - t0)
            if sleep > 0:
                time.sleep(sleep)
        browser.close()

    if n == 0:
        sys.exit("captured nothing")
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    palette = os.path.join(tmp, "pal.png")
    # A shared palette across every frame, or the colours crawl between frames
    # and the file doubles in size for the privilege.
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(args.fps),
                    "-i", os.path.join(tmp, "f%04d.png"),
                    "-vf", f"scale={args.width}:-1:flags=lanczos,"
                           f"palettegen=max_colors={args.max_colors}:stats_mode=diff",
                    palette], check=True)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(args.fps),
                    "-i", os.path.join(tmp, "f%04d.png"), "-i", palette,
                    "-lavfi", f"scale={args.width}:-1:flags=lanczos [x]; "
                              f"[x][1:v] paletteuse=dither=bayer:bayer_scale=4",
                    "-loop", "0", args.out], check=True)
    if shutil.which("gifsicle"):
        subprocess.run(["gifsicle", "-O3", "--batch", args.out], check=False)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"wrote {args.out}: {n} frames, {os.path.getsize(args.out)/1048576:.2f} MB")


if __name__ == "__main__":
    main()

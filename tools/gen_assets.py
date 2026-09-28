#!/usr/bin/env python3
"""Generate Token-Pet character assets via pptoken image API.

Usage:
  python3 tools/gen_assets.py base          # generate base character
  python3 tools/gen_assets.py poses         # generate pose variants (edits w/ base ref)
  python3 tools/gen_assets.py process       # trim/pad frames + manifest.json
"""
import base64
import json
import os
import subprocess
import sys
import urllib.request
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = os.path.join(ROOT, "tools", "pptoken.env")
RAW = os.path.join(ROOT, "assets", "character", "raw")
FRAMES = os.path.join(ROOT, "assets", "character", "frames")


def load_env():
    env = {}
    with open(ENV) as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                env[k] = v
    return env


ENV_VARS = load_env()
KEY = ENV_VARS["PPTOKEN_KEY"]
BASE = ENV_VARS["PPTOKEN_BASE"]

BASE_PROMPT = (
    "Chibi xianxia cultivator character sprite, cute stylized game asset, full body front view, "
    "standing pose, arms relaxed, dark charcoal hair with topknot bun and small red ribbon, "
    "white hanfu robe with light blue trim, red waist sash, small sword strapped on back, "
    "big expressive dark eyes, rosy cheeks, soft cel shading, clean flat colors, "
    "thick clean outline, centered composition, entire character visible with margin, "
    "transparent background, no text, no watermark, no ground shadow"
)

POSE_PROMPTS = {
    "blink": "Same exact character, same pose, same colors and style. Eyes gently closed (happy blink), small soft smile. Everything else identical. Transparent background, no text.",
    "meditate": "Same exact character, same colors and style, sitting cross-legged in meditation pose, hands folded in lap, eyes closed, faint blue spiritual aura around the body, floating slightly above ground. Transparent background, no text.",
    "sword": "Same exact character, same colors and style, standing on a flying sword (sword under the feet), one hand raised in sword-finger gesture, robe fluttering, dynamic flying pose, light blue wind swirls. Transparent background, no text.",
    "breakthrough": "Same exact character, same colors and style, powering up: both arms slightly raised, fists clenched, determined expression, golden glowing aura and rising light particles around the body. Transparent background, no text.",
}


def post_json(url, payload):
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        headers={
            "Authorization": "Bearer " + KEY,
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=300) as resp:
        return json.loads(resp.read().decode())


def post_edit(image_path, prompt, size="1024x1536"):
    boundary = "----TokenPet" + uuid.uuid4().hex
    parts = []

    def field(name, value):
        parts.append(
            (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
                f"{value}\r\n"
            ).encode()
        )

    def file_field(name, path):
        with open(path, "rb") as f:
            data = f.read()
        parts.append(
            (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="{name}"; filename="ref.png"\r\n'
                f"Content-Type: image/png\r\n\r\n"
            ).encode()
        )
        parts.append(data)
        parts.append(b"\r\n")

    field("model", "gpt-image-2.5")
    field("prompt", prompt)
    field("size", size)
    field("background", "transparent")
    field("output_format", "png")
    field("quality", "medium")
    field("n", "1")
    file_field("image", image_path)
    parts.append(f"--{boundary}--\r\n".encode())
    body = b"".join(parts)

    req = urllib.request.Request(
        BASE + "/images/edits",
        data=body,
        headers={
            "Authorization": "Bearer " + KEY,
            "Content-Type": "multipart/form-data; boundary=" + boundary,
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=600) as resp:
        return json.loads(resp.read().decode())


def save_image(data, out_path):
    item = data["data"][0]
    if "b64_json" in item and item["b64_json"]:
        raw = base64.b64decode(item["b64_json"])
    else:
        with urllib.request.urlopen(item["url"], timeout=300) as r:
            raw = r.read()
    with open(out_path, "wb") as f:
        f.write(raw)
    print(f"  saved {out_path} ({len(raw)} bytes)")


def cmd_base():
    os.makedirs(RAW, exist_ok=True)
    out = os.path.join(RAW, "base.png")
    print("generating base character (1024x1536, transparent)...")
    data = post_json(
        BASE + "/images/generations",
        {
            "model": "gpt-image-2.5",
            "prompt": BASE_PROMPT,
            "n": 1,
            "size": "1024x1536",
            "background": "transparent",
            "output_format": "png",
            "quality": "medium",
        },
    )
    save_image(data, out)


def cmd_poses():
    base = os.path.join(RAW, "base.png")
    if not os.path.exists(base):
        print("run base first")
        return
    for name, prompt in POSE_PROMPTS.items():
        out = os.path.join(RAW, name + ".png")
        print(f"generating pose: {name} ...")
        try:
            data = post_edit(base, prompt)
            save_image(data, out)
        except Exception as e:
            print(f"  FAILED {name}: {e}")


def cmd_process():
    os.makedirs(FRAMES, exist_ok=True)
    try:
        from PIL import Image
    except ImportError:
        print("PIL missing; install pillow")
        return

    canvas = (512, 680)

    def prep(src, dst, fit_bottom=True):
        im = Image.open(src).convert("RGBA")
        bbox = im.getbbox()
        if bbox:
            im = im.crop(bbox)
        w, h = im.size
        scale = min(canvas[0] / w, canvas[1] / h, 1.0)
        nw, nh = int(w * scale), int(h * scale)
        im = im.resize((nw, nh), Image.LANCZOS)
        out = Image.new("RGBA", canvas, (0, 0, 0, 0))
        x = (canvas[0] - nw) // 2
        y = canvas[1] - nh if fit_bottom else (canvas[1] - nh) // 2
        out.alpha_composite(im, (x, y))
        out.save(dst)
        print(f"  frame {dst} ({nw}x{nh} -> {canvas})")

    frames = {}
    mapping = {
        "stand": "base.png",
        "blink": "blink.png",
        "meditate": "meditate.png",
        "sword": "sword.png",
        "breakthrough": "breakthrough.png",
    }
    for frame_name, raw_name in mapping.items():
        src = os.path.join(RAW, raw_name)
        if not os.path.exists(src):
            print(f"  skip {frame_name} (missing raw)")
            continue
        dst = os.path.join(FRAMES, frame_name + ".png")
        prep(src, dst)
        frames[frame_name] = f"frames/{frame_name}.png"

    manifest = {
        "mode": "sprites" if frames else "procedural",
        "canvas": list(canvas),
        "anchor": "bottom-center",
        "frames": frames,
    }
    with open(os.path.join(ROOT, "assets", "character", "manifest.json"), "w") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print("manifest written:", manifest["mode"], list(frames.keys()))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    {"base": cmd_base, "poses": cmd_poses, "process": cmd_process}[cmd]()

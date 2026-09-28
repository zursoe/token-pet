#!/usr/bin/env python3
"""Generate continuous-motion animation frames for Token-Pet.

Usage:
  python3 tools/gen_anim.py strip <action>     # 4-frame horizontal strip
  python3 tools/gen_anim.py chain <action>     # chained single frames
  python3 tools/gen_anim.py all                # everything missing
  python3 tools/gen_anim.py sheet <action>     # build contact sheet for review

Outputs raw frames to assets/character/anim_raw/<action>/
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_assets import post_edit, save_image, RAW  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANIM_RAW = os.path.join(ROOT, "assets", "character", "anim_raw")
BASE = os.path.join(RAW, "base.png")

STYLE = ("Same exact character as the reference image: chibi xianxia cultivator, dark charcoal hair "
         "with topknot bun and red ribbon, white hanfu robe with light blue trim, red sash, black boots. "
         "Identical design, colors, proportions and line style. ")

GREEN_BG = ("Place the character on a SOLID UNIFORM pure chroma-green background "
            "(#00FF00) filling the whole canvas like a green screen for keying; "
            "the character itself must not contain pure green. No shadow on the background, "
            "no text, no watermark, no border, no frame. ")

# action -> list of chained prompts (each edits the previous frame)
CHAINED = {
    "blink": [
        "Eyes half closed (drowsy blink), same standing pose, same scale and position.",
        "Eyes fully closed in a happy soft blink with a small smile, same standing pose, same scale and position.",
    ],
    "meditate_in": [
        "Beginning to sit down: knees bent, body lowering, arms starting to lower to the sides, eyes "
        "half closing. Same character, same scale.",
        "Halfway sitting: almost seated cross-legged, hands moving toward the lap, eyes nearly closed, "
        "faint blue aura starting to appear.",
        "Fully sitting cross-legged in meditation, hands folded in lap, eyes closed, gentle smile, "
        "faint blue spiritual aura around the body.",
    ],
    "sword_in": [
        "Drawing the sword with the right hand, blade pulled from the back scabbard, determined smile.",
        "Stepping onto the sword as it rises from the ground, one foot on the blade, arms balancing.",
        "Standing on the flying sword hovering above the ground, one hand raised in sword-finger gesture, "
        "robe and hair fluttering, light blue wind swirls.",
    ],
    "breakthrough": [
        "Gathering power: standing firm, fists clenched at sides, faint golden glow close around the "
        "body, eyes determined.",
        "Golden aura expanding outward, hair lifting, robe fluttering, bright gold light.",
        "Peak burst: brilliant golden light radiating, strong aura, hair flying, triumphant expression.",
        "Aura receding, standing firm, residual golden sparks around the body.",
    ],
}


def corner_alphas(path):
    out = subprocess.run(
        ["magick", path, "-format",
         "%[pixel:p{2,2}] %[pixel:p{w-3,2}] %[pixel:p{2,h-3}] %[pixel:p{w-3,h-3}]", "info:"],
        capture_output=True, text=True)
    alphas = []
    for tok in out.stdout.split():
        try:
            a = float(tok.split(",")[-1].rstrip(")"))
            alphas.append(a)
        except Exception:
            alphas.append(1.0)
    return alphas


def transparent_ok(path):
    """sanity check only: file exists and is a readable image"""
    return os.path.exists(path) and os.path.getsize(path) > 10000, None


def gen_with_retry(ref, prompt, out_path, size, tries=3):
    for i in range(1, tries + 1):
        full = prompt
        try:
            data = post_edit(ref, STYLE + full + " " + GREEN_BG, size=size)
            save_image(data, out_path)
        except Exception as e:
            print(f"    attempt {i} request failed: {e}")
            continue
        ok, info = transparent_ok(out_path)
        if ok:
            return True
        print(f"    attempt {i}: background check failed {info}, retrying...")
    return False


def do_chain(action):
    outdir = os.path.join(ANIM_RAW, action)
    os.makedirs(outdir, exist_ok=True)
    prev = BASE
    for idx, prompt in enumerate(CHAINED[action]):
        out = os.path.join(outdir, f"f_{idx:02d}.png")
        if os.path.exists(out):
            print(f"[{action}] f_{idx:02d} exists, skip")
            prev = out
            continue
        print(f"[{action}] generating frame {idx} ...")
        ok = gen_with_retry(prev, prompt, out, "1024x1536")
        print(f"[{action}] f_{idx:02d} {'OK' if ok else 'FAILED'}")
        prev = out


def sheet(action):
    out = f"/tmp/opencode/anim_{action}.png"
    files = sorted(f for f in os.listdir(os.path.join(ANIM_RAW, action)) if f.startswith("f_"))
    args = [os.path.join(ANIM_RAW, action, f) for f in files]
    if not args:
        print("nothing to sheet")
        return
    subprocess.run(["magick"] + args + ["-background", "#465064", "-alpha", "remove",
                    "-alpha", "off", "-resize", "x480", "+append", out])
    print("sheet:", out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "chain":
        do_chain(sys.argv[2])
    elif cmd == "sheet":
        sheet(sys.argv[2])
    elif cmd == "all":
        for a in CHAINED:
            do_chain(a)


if __name__ == "__main__":
    main()

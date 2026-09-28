#!/usr/bin/env python3
"""Process raw animation frames: chroma-key green, uniform scale, anchor placement, manifest v2.

Outputs assets/character/anim/<action>/f_XX.png (512x680) + assets/character/manifest.json (mode anim2)
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CH = os.path.join(ROOT, "assets", "character")
RAW = os.path.join(CH, "raw")
ANIM_RAW = os.path.join(CH, "anim_raw")
OUT = os.path.join(CH, "anim")
CANVAS = (512, 680)
STAND_H = 620.0        # target solid height of the standing figure
GROUND = 660           # baseline y for grounded poses
SWORD_FLOAT = 46       # how much the flying-sword frame hovers above ground


def run(args, check=True):
    r = subprocess.run(args, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(" ".join(args[:4]) + " -> " + r.stderr.strip()[:200])
    return r.stdout.strip()


def size_of(path):
    out = run(["magick", "identify", "-format", "%w %h", path])
    w, h = out.split()
    return int(w), int(h)


def solid_bbox(path):
    """bbox of pixels with alpha >= 50%, as (x, y, w, h); None if empty"""
    out = run(["magick", path, "-alpha", "extract", "-threshold", "50%", "-format", "%@", "info:"])
    if not out:
        return None
    # e.g. 276x675+122+3
    import re
    m = re.match(r"(\d+)x(\d+)\+(\d+)\+(\d+)", out)
    if not m:
        return None
    return (int(m.group(3)), int(m.group(4)), int(m.group(1)), int(m.group(2)))


def key_green(src, dst):
    w, h = size_of(src)
    seeds = [
        (2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3),
        (w // 2, 2), (2, h // 2), (w - 3, h // 2), (w // 2, h - 3),
        (w // 4, 2), (3 * w // 4, 2), (w // 4, h - 3), (3 * w // 4, h - 3),
    ]
    cmd = ["magick", src, "-alpha", "set", "-fuzz", "30%", "-fill", "none"]
    for x, y in seeds:
        cmd += ["-draw", f"alpha {x},{y} floodfill"]
    cmd.append(dst)
    run(cmd)


def green_ratio(path):
    out = run(["magick", path, "-format", "%[fx:mean.g > 0.5 ? 1 : 0]", "info:"], check=False)
    return out


def place(src, dst, scale, baseline_y):
    """scale whole image, then composite so solid-bbox bottom = baseline_y and center x = 256"""
    tmp = "/tmp/opencode/_place_tmp.png"
    pct = f"{scale * 100:.4f}%"
    run(["magick", src, "-resize", pct, tmp])
    b = solid_bbox(tmp)
    if not b:
        print(f"  WARN: empty solid bbox for {src}")
        b = (0, 0, size_of(tmp)[0], size_of(tmp)[1])
    x, y, w, h = b
    dx = CANVAS[0] // 2 - (x + w // 2)
    dy = int(baseline_y) - (y + h)
    run(["magick", "-size", f"{CANVAS[0]}x{CANVAS[1]}", "xc:none",
         tmp, "-geometry", f"{dx:+d}{dy:+d}", "-composite", dst])


ACTION_SCALE = {
    "breakthrough": 0.94,
    "meditate_in": 0.98,
    "sword_in": 0.97,
}


def process_action(name, frames, scale, anchor_fn):
    """anchor_fn(i, n) -> baseline y for frame i of n"""
    outdir = os.path.join(OUT, name)
    os.makedirs(outdir, exist_ok=True)
    n = len(frames)
    outs = []
    for i, (src, green) in enumerate(frames):
        keyed = f"/tmp/opencode/_keyed_{name}_{i:02d}.png"
        if green:
            key_green(src, keyed)
        else:
            keyed = src
        dst = os.path.join(outdir, f"f_{i:02d}.png")
        place(keyed, dst, scale * ACTION_SCALE.get(name, 1.0), anchor_fn(i, n))
        outs.append(f"anim/{name}/f_{i:02d}.png")
        print(f"  [{name}] f_{i:02d} <- {os.path.basename(src)}")
    return outs


def main():
    os.makedirs(OUT, exist_ok=True)

    # reference scale from the standing base
    base_raw = os.path.join(RAW, "base.png")
    b = solid_bbox(base_raw)
    if not b:
        print("base raw has no solid bbox")
        return
    scale = STAND_H / b[3]
    print(f"reference scale = {scale:.4f} (base solid height {b[3]})")

    actions = {}

    # idle: standing frame
    idle = process_action("idle", [(base_raw, False)], scale, lambda i, n: GROUND)
    actions["idle"] = {"frames": idle, "fps": 2.0, "loop": True}

    # meditate: sitting frame, lower baseline a touch
    med_raw = os.path.join(RAW, "meditate.png")
    med_baseline = GROUND
    med = process_action("meditate", [(med_raw, False)], scale, lambda i, n: med_baseline)
    actions["meditate"] = {"frames": med, "fps": 2.0, "loop": True}

    # sword: hover baseline
    swd_raw = os.path.join(RAW, "sword.png")
    swd_baseline = GROUND - SWORD_FLOAT
    swd = process_action("sword", [(swd_raw, False)], scale, lambda i, n: swd_baseline)
    actions["sword"] = {"frames": swd, "fps": 2.0, "loop": True}

    # blink chain (green keyed)
    bl = sorted(f for f in os.listdir(os.path.join(ANIM_RAW, "blink")) if f.startswith("f_")) \
        if os.path.isdir(os.path.join(ANIM_RAW, "blink")) else []
    blink_frames = [(os.path.join(ANIM_RAW, "blink", f), True) for f in bl]
    if blink_frames:
        outs = process_action("blink", blink_frames, scale, lambda i, n: GROUND)
        actions["blink"] = {"frames": outs, "fps": 7.0, "loop": False, "next": "idle"}
    else:
        print("no blink frames, skip")

    # meditate_in chain: baseline fixed at ground
    mi = sorted(f for f in os.listdir(os.path.join(ANIM_RAW, "meditate_in")) if f.startswith("f_")) \
        if os.path.isdir(os.path.join(ANIM_RAW, "meditate_in")) else []
    mi_frames = [(os.path.join(ANIM_RAW, "meditate_in", f), True) for f in mi]
    if mi_frames:
        outs = process_action("meditate_in", mi_frames, scale, lambda i, n: GROUND)
        actions["meditate_in"] = {"frames": outs, "fps": 5.0, "loop": False, "next": "meditate"}
        # reversed out-transition
        rev = list(reversed(outs))
        actions_rel = [{"src": p} for p in rev]
        outdir = os.path.join(OUT, "meditate_out")
        os.makedirs(outdir, exist_ok=True)
        out_files = []
        for i, entry in enumerate(actions_rel):
            src = os.path.join(CH, entry["src"].replace("/", os.sep))
            dst = os.path.join(outdir, f"f_{i:02d}.png")
            run(["magick", src, dst])
            out_files.append(f"anim/meditate_out/f_{i:02d}.png")
        actions["meditate_out"] = {"frames": out_files, "fps": 7.0, "loop": False, "next": "idle"}
    else:
        print("no meditate_in frames, skip")

    # sword_in chain: baseline interpolates ground -> hover
    si = sorted(f for f in os.listdir(os.path.join(ANIM_RAW, "sword_in")) if f.startswith("f_")) \
        if os.path.isdir(os.path.join(ANIM_RAW, "sword_in")) else []
    si_frames = [(os.path.join(ANIM_RAW, "sword_in", f), True) for f in si]
    if si_frames:
        def si_anchor(i, n):
            t = (i + 1) / n
            return GROUND - SWORD_FLOAT * t
        outs = process_action("sword_in", si_frames, scale, si_anchor)
        actions["sword_in"] = {"frames": outs, "fps": 5.0, "loop": False, "next": "sword"}
        outdir = os.path.join(OUT, "sword_out")
        os.makedirs(outdir, exist_ok=True)
        out_files = []
        for i, src in enumerate(reversed(outs)):
            full = os.path.join(CH, src.replace("/", os.sep))
            dst = os.path.join(outdir, f"f_{i:02d}.png")
            run(["magick", full, dst])
            out_files.append(f"anim/sword_out/f_{i:02d}.png")
        actions["sword_out"] = {"frames": out_files, "fps": 7.0, "loop": False, "next": "idle"}
    else:
        print("no sword_in frames, skip")

    # breakthrough chain
    bt = sorted(f for f in os.listdir(os.path.join(ANIM_RAW, "breakthrough")) if f.startswith("f_")) \
        if os.path.isdir(os.path.join(ANIM_RAW, "breakthrough")) else []
    bt_frames = [(os.path.join(ANIM_RAW, "breakthrough", f), True) for f in bt]
    if bt_frames:
        outs = process_action("breakthrough", bt_frames, scale, lambda i, n: GROUND)
        actions["breakthrough"] = {"frames": outs, "fps": 6.0, "loop": False, "next": "idle"}
    else:
        print("no breakthrough frames, skip")

    manifest = {
        "mode": "anim2",
        "canvas": list(CANVAS),
        "actions": actions,
    }
    with open(os.path.join(CH, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print("manifest written:", list(actions.keys()))


if __name__ == "__main__":
    main()

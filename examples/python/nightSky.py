"""A night sky built with the jitnodes Python API and rendered to a PNG.

    python nightSky.py <folder with the jitnodes module> [out.png]

The script only builds and cooks a node graph, like the nebula example, but from Python: layers are made by small functions, and the
constellation is driven by a table in this file (its star positions become the generated wrangle code).
"""

import math
import os
import sys
import time

# Ursa Major (the Big Dipper): name -> (x, y, brightness 0..1), in sky units; the path walks the dipper's outline.
DIPPER = {
    "Alkaid": (-7.2, -0.6, 0.62),
    "Mizar": (-4.6, 0.3, 0.7),
    "Alioth": (-2.4, 0.1, 0.9),
    "Megrez": (0.0, -0.2, 0.42),
    "Dubhe": (3.9, 0.2, 0.95),
    "Merak": (3.4, -2.6, 0.72),
    "Phecda": (0.1, -2.9, 0.68),
}
DIPPER_PATH = ["Alkaid", "Mizar", "Alioth", "Megrez", "Dubhe", "Merak", "Phecda", "Megrez"]
DIPPER_ORIGIN = (-9.5, 5.2)


def build(jn):
    net = jn.Network()

    def make(kind, name, *inputs, **params):
        node = net.add_node(kind, name)
        for key, value in params.items():
            node[key] = value
        for index, source in enumerate(inputs):
            source.connect(node, index)
        return node

    def table(names, values, origin):
        # sampleIndex picks the entry: x = i == 0 ? .. : i == 1 ? .. : 0
        code = []
        for axis, shift in (("x", origin[0]), ("y", origin[1])):
            expression = "0"
            for index in reversed(range(len(names))):
                value = values[names[index]][0 if axis == "x" else 1] + shift
                expression = f"sampleIndex == {index} ? {value:.4f} : {expression}"
            code.append(f"{axis} = {expression};")
        return " ".join(code)

    layers = []

    # Field stars and a fine dust of faint ones.
    stars = make("generate", "fieldStars", count=6000, seed=21, width=42, height=22, shape="rect")
    layers.append(make("wrangle", "fieldStarsLook", stars, code=(
        "sz = rand * rand * rand * rand; sz = sz * sz; radius = 0.02 + 0.11 * sz; alpha = 0.7 + 0.3 * rand2; "
        "Cd.x = lerp(0.75, 1, rand2); Cd.y = lerp(0.82, 0.95, rand2); Cd.z = lerp(1, 0.78, rand2); softness = 1;")))
    dust = make("generate", "faintStars", count=22000, seed=22, width=42, height=22, shape="rect")
    layers.append(make("wrangle", "faintStarsLook", dust, code=(
        "radius = 0.012 + 0.012 * rand; alpha = 0.08 + 0.3 * rand2; Cd.x = 0.85; Cd.y = 0.9; Cd.z = 1; softness = 0.5;")))

    # The Milky Way: a rotated gaussian band, gas and dust lanes from two noise fields, curled into wisps, coloured by density.
    band = make("generate", "bandPoints", count=450000, seed=4, width=66, height=12, shape="gaussian")
    tilt = make("wrangle", "bandTilt", band, code=(
        f"gx = x * {math.cos(0.5):.5f} - y * {math.sin(0.5):.5f}; gy = x * {math.sin(0.5):.5f} + y * {math.cos(0.5):.5f}; "
        "x = gx + 1.5; y = gy - 0.5;"))
    gas = make("noise", "bandGas", tilt, scale=0.16, octaves=6, seed=8, mode="fbm")
    gas["name"] = "gas"
    lanes = make("noise", "bandLanes", gas, scale=0.35, octaves=5, seed=12, mode="ridged", offsetX=40)
    lanes["name"] = "lane"
    wisps = make("warp", "bandWisps", lanes, amount=0.7, scale=0.1, octaves=2, iterations=2, seed=3, mode="curl")
    mix = make("wrangle", "bandMix", wisps, code=(
        "dist = sqrt((x - 1.5) * (x - 1.5) * 0.012 + (y + 0.5) * (y + 0.5) * 0.02); core = exp(0 - dist * dist); "
        "dens = clamp(gas * 0.9 + core * 0.4 + 0.05 - lane * lane * 0.7, 0.05, 1); "
        "tone = clamp(gas * 0.6 + core * 0.6 - lane * 0.2, 0, 1);"))
    color = make("colorRamp", "bandColor", mix, attribute="tone",
                 ramp="0 #141a4a, 0.35 #3a4fa8, 0.6 #8a86d0, 0.8 #e0b8c8, 1 #fff0d8")
    layers.append(make("wrangle", "bandLook", color, code=(
        "radius = 0.05 + 0.08 * dens; alpha = 0.002 + 0.035 * dens * dens; softness = 1;")))

    # A spiral galaxy: two arms from rand/rand2, tilted, with a warm core.
    galaxy = make("generate", "galaxyPoints", count=140000, seed=31, width=2, height=2, shape="rect")
    tiltG, shiftX, shiftY = 0.5, 9.0, 5.2
    spiral = make("wrangle", "galaxySpiral", galaxy, code=(
        "r = 0.1 + 3.0 * rand * sqrt(rand); side = rand2 > 0.5 ? 1 : 0; "
        "ang = side * 3.14159265 + r * 1.7 + (rand2 - 0.25 - side * 0.5) * (1.2 + r * 0.8); "
        "gx = r * cos(ang); gy = r * sin(ang) * 0.42; tone = clamp(1.05 - r * 0.3, 0, 1); "
        f"x = {shiftX} + gx * {math.cos(tiltG):.5f} - gy * {math.sin(tiltG):.5f}; "
        f"y = {shiftY} + gx * {math.sin(tiltG):.5f} + gy * {math.cos(tiltG):.5f};"))
    galaxyColor = make("colorRamp", "galaxyColor", spiral, attribute="tone",
                       ramp="0 #5f8dff, 0.5 #b9c8ff, 0.8 #ffe6c0, 1 #fff8e8")
    layers.append(make("wrangle", "galaxyLook", galaxyColor, code=(
        "radius = 0.03 + 0.03 * rand; alpha = 0.03 + 0.14 * tone * tone; softness = 1;")))
    glow = make("generate", "galaxyGlow", count=40000, seed=32, cx=shiftX, cy=shiftY, width=3.2, height=1.4, shape="gaussian")
    layers.append(make("wrangle", "galaxyGlowLook", glow, code=(
        f"gx = x - {shiftX}; gy = y - {shiftY}; "
        f"x = {shiftX} + gx * {math.cos(tiltG):.5f} - gy * {math.sin(tiltG):.5f}; "
        f"y = {shiftY} + gx * {math.sin(tiltG):.5f} + gy * {math.cos(tiltG):.5f}; "
        "radius = 0.16; alpha = 0.012; Cd.x = 1; Cd.y = 0.9; Cd.z = 0.72; softness = 1;")))

    # The Big Dipper: a core and a halo per star, and the outline walked as faint dots.
    names = list(DIPPER)
    placed = make("generate", "dipperPoints", count=len(names), seed=1, width=1, height=1)
    positions = make("wrangle", "dipperPlace", placed, code=table(names, DIPPER, DIPPER_ORIGIN))
    brightness = " ".join(f"bright = sampleIndex == {i} ? {DIPPER[n][2]} : bright;" for i, n in enumerate(names))
    sized = make("wrangle", "dipperSize", positions, code="bright = 0; " + brightness)
    layers.append(make("wrangle", "dipperCore", sized, code=(
        "radius = 0.1 + 0.16 * bright; alpha = 1; Cd.x = 1; Cd.y = 0.97; Cd.z = 0.9; softness = 1;")))
    layers.append(make("wrangle", "dipperHalo", sized, code=(
        "radius = 0.5 + 1.1 * bright; alpha = 0.06 + 0.1 * bright; Cd.x = 0.75; Cd.y = 0.85; Cd.z = 1; softness = 1;")))
    outline = make("line", "dipperOutline", count=len(DIPPER_PATH), length=1)
    pathPlace = make("wrangle", "dipperPath", outline, code=table(DIPPER_PATH, DIPPER, DIPPER_ORIGIN))
    walked = make("scatter", "dipperWalk", pathPlace, count=0, lineCount=5000, spread=0.0, seed=9)
    layers.append(make("wrangle", "dipperLines", walked, code=(
        "radius = 0.022; alpha = 0.16; Cd.x = 0.6; Cd.y = 0.75; Cd.z = 1; softness = 1;")))

    # A shooting star: a short line walked with dots that fade toward the tail.
    streak = make("line", "meteorLine", count=30, length=1)
    streakPlace = make("wrangle", "meteorPlace", streak, code="x = 3 - sampleIndex * 0.25; y = 9.6 - sampleIndex * 0.26;")
    meteor = make("scatter", "meteorWalk", streakPlace, count=0, lineCount=5000, spread=0.05, seed=17)
    layers.append(make("wrangle", "meteorLook", meteor, code=(
        "radius = 0.012 + 0.04 * u * u; alpha = 0.02 + 0.5 * u * u * u; Cd.x = 0.85; Cd.y = 0.92; Cd.z = 1; softness = 1;")))

    # The moon: a dense disc of tiny sprites, cut to a crescent, with crater noise, inside a soft halo.
    mx, my, mr = 14.0, -6.0, 1.7
    disc = make("generate", "moonDisc", count=110000, seed=41, cx=mx, cy=my, width=mr * 2, height=mr * 2, shape="disc")
    craters = make("noise", "moonCraters", disc, scale=1.6, octaves=5, seed=6, mode="fbm")
    craters["name"] = "crater"
    layers.append(make("wrangle", "moonLook", craters, code=(
        f"rx = (x - {mx}) / {mr}; ry = (y - {my}) / {mr}; lit = (rx + 0.55) * (rx + 0.55) + ry * ry > 1 ? 1 : 0; "
        "radius = 0.022; alpha = lit * (0.025 + 0.06 * crater); Cd.x = 1; Cd.y = 0.95; Cd.z = 0.82; softness = 1;")))
    halo = make("generate", "moonHalo", count=30000, seed=42, cx=mx, cy=my, width=6, height=6, shape="gaussian")
    layers.append(make("wrangle", "moonHaloLook", halo, code=(
        "radius = 0.2; alpha = 0.0025; Cd.x = 0.8; Cd.y = 0.88; Cd.z = 1; softness = 1;")))

    everything = make("merge", "sky")
    for index, layer in enumerate(layers):
        layer.connect(everything, index)
    image = make("render", "image", everything, exposure=0.75, gamma=2.2, bloom=0.3, bloomRadius=10,
                 width=1600, height=800, viewCx=0, viewCy=0, viewWidth=40, background="#02030a")
    net.set_display_node(image)
    return net, image


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    sys.path.insert(0, args[0])
    import jitnodes as jn

    out = args[1] if len(args) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "nightSky.png")
    net, image = build(jn)
    started = time.time()
    geometry = net.cook(image)
    cooked = time.time()
    size = geometry.render_png(out)
    print(f"{len(net)} nodes, {geometry.point_count()} points cooked in {cooked - started:.2f}s, "
          f"{size[0]}x{size[1]} written to {out} in {time.time() - cooked:.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())

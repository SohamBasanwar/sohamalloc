"""Shared fonts, colours, styles and drawing helpers for the two sohamalloc PDFs."""
import os
import subprocess

from reportlab.lib.colors import HexColor, white
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.pdfmetrics import registerFontFamily, stringWidth
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import Frame

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
os.makedirs(BUILD, exist_ok=True)

LATO = "/usr/share/fonts/truetype/lato/"
DJ = "/usr/share/fonts/truetype/dejavu/"


def register_fonts():
    for name, path in [
        ("Lato", LATO + "Lato-Regular.ttf"),
        ("Lato-Bold", LATO + "Lato-Bold.ttf"),
        ("Lato-Italic", LATO + "Lato-Italic.ttf"),
        ("Lato-BoldItalic", LATO + "Lato-BoldItalic.ttf"),
        ("Lato-Semibold", LATO + "Lato-Semibold.ttf"),
        ("Lato-Light", LATO + "Lato-Light.ttf"),
        ("Lato-Black", LATO + "Lato-Black.ttf"),
        ("Mono", DJ + "DejaVuSansMono.ttf"),
        ("Mono-Bold", DJ + "DejaVuSansMono-Bold.ttf"),
        ("Mono-Oblique", DJ + "DejaVuSansMono-Oblique.ttf"),
        ("DejaVu", DJ + "DejaVuSans.ttf"),
        ("DejaVu-Bold", DJ + "DejaVuSans-Bold.ttf"),
    ]:
        pdfmetrics.registerFont(TTFont(name, path))
    registerFontFamily("Lato", normal="Lato", bold="Lato-Bold", italic="Lato-Italic", boldItalic="Lato-BoldItalic")
    registerFontFamily("Mono", normal="Mono", bold="Mono-Bold", italic="Mono-Oblique", boldItalic="Mono-Bold")
    registerFontFamily("Lato-Light", normal="Lato-Light", bold="Lato-Bold", italic="Lato-Italic", boldItalic="Lato-BoldItalic")
    registerFontFamily("Lato-Bold", normal="Lato-Bold", bold="Lato-Bold", italic="Lato-BoldItalic", boldItalic="Lato-BoldItalic")
    registerFontFamily("Lato-Italic", normal="Lato-Italic", bold="Lato-BoldItalic", italic="Lato-Italic", boldItalic="Lato-BoldItalic")
    registerFontFamily("Lato-Black", normal="Lato-Black", bold="Lato-Black", italic="Lato-Black", boldItalic="Lato-Black")


# Text and surfaces
INK = HexColor("#15171c")
INK2 = HexColor("#52514e")
MUTED = HexColor("#8a8984")
RULE = HexColor("#dddcd6")
PANEL = HexColor("#f5f4f1")
PANEL2 = HexColor("#ecebe6")
# Accent: deep blue for headings, the chart blue for sohamalloc's own marks
ACCENT = HexColor("#1c5cab")
SOHAM = HexColor("#2a78d6")
ACCENT_SOFT = HexColor("#e9f1fb")
ACCENT_LINE = HexColor("#9ec5f4")
OTHER = HexColor("#b9b7b0")          # other allocators in highlight charts
# Status (always shown with an icon and a word)
GOOD = HexColor("#2e7d32")
GOOD_SOFT = HexColor("#e8f3e9")
BAD = HexColor("#b3261e")
BAD_SOFT = HexColor("#fbeaea")
WARN_SOFT = HexColor("#fdf3e2")
# Categorical slots for sigstorm outcomes (validated: scripts/validate_palette.js, light mode)
STALL = HexColor("#eb6834")
CRASH = HexColor("#6250d6")
CORRUPT = HexColor("#1baf7a")
SURVIVED = HexColor("#e4e2dc")

CHECK = '<font name="DejaVu-Bold">✓</font>'
CROSS = '<font name="DejaVu-Bold">✗</font>'


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def style(name, **kw):
    base = dict(fontName="Lato", fontSize=11, leading=15, textColor=INK, alignment=TA_LEFT)
    base.update(kw)
    return ParagraphStyle(name, **base)


def place(c, flowables, x, y_top, w, h, name=""):
    """Draw flowables into a box whose top-left corner is (x, y_top); fail on overflow."""
    f = Frame(x, y_top - h, w, h, leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0, showBoundary=0)
    rest = list(flowables)
    f.addFromList(rest, c)
    if rest:
        raise RuntimeError(f"content overflows its box: {name} ({len(rest)} flowables left)")


def height_of(flowables, w):
    return sum(f.wrap(w, 10000)[1] + f.getSpaceBefore() + f.getSpaceAfter() for f in flowables)


def fit_font(text, font, size, max_w, min_size=8):
    while size > min_size and stringWidth(text, font, size) > max_w:
        size -= 0.5
    return size


def rbox(c, x, y, w, h, fill=PANEL, stroke=RULE, r=8, lw=1, dash=None):
    c.saveState()
    c.setFillColor(fill)
    c.setStrokeColor(stroke)
    c.setLineWidth(lw)
    if dash:
        c.setDash(*dash)
    c.roundRect(x, y, w, h, r, stroke=1 if stroke is not None else 0, fill=1 if fill is not None else 0)
    c.restoreState()


def arrow(c, x1, y1, x2, y2, color=INK2, lw=1.4, head=7, dash=None):
    import math
    c.saveState()
    c.setStrokeColor(color)
    c.setFillColor(color)
    c.setLineWidth(lw)
    if dash:
        c.setDash(*dash)
    ang = math.atan2(y2 - y1, x2 - x1)
    xe, ye = x2 - head * 0.8 * math.cos(ang), y2 - head * 0.8 * math.sin(ang)
    c.line(x1, y1, xe, ye)
    c.setDash()
    p = c.beginPath()
    p.moveTo(x2, y2)
    p.lineTo(x2 - head * math.cos(ang - 0.42), y2 - head * math.sin(ang - 0.42))
    p.lineTo(x2 - head * math.cos(ang + 0.42), y2 - head * math.sin(ang + 0.42))
    p.close()
    c.drawPath(p, stroke=0, fill=1)
    c.restoreState()


def hatch(c, x, y, w, h, color=MUTED, step=6):
    c.saveState()
    p = c.beginPath()
    p.rect(x, y, w, h)
    c.clipPath(p, stroke=0, fill=0)
    c.setStrokeColor(color)
    c.setLineWidth(0.6)
    k = -h
    while k < w:
        c.line(x + k, y, x + k + h, y + h)
        k += step
    c.restoreState()


GV_STYLE = (
    'graph [fontname="Lato", bgcolor="transparent", pad="0.15", nodesep="0.35", ranksep="0.45"];\n'
    'node  [fontname="Lato", fontsize=13, shape=box, style="rounded,filled", fillcolor="#f5f4f1", '
    'color="#c9c7bf", fontcolor="#15171c", penwidth=1.2, margin="0.18,0.10"];\n'
    'edge  [fontname="Lato", fontsize=11, color="#52514e", fontcolor="#52514e", penwidth=1.2, arrowsize=0.8];\n'
)


def graphviz_png(name, body, dpi=300):
    """Render a dot graph (body without the outer braces' style lines) to build/<name>.png."""
    src = os.path.join(BUILD, name + ".dot")
    out = os.path.join(BUILD, name + ".png")
    with open(src, "w") as fh:
        fh.write(body.replace("%STYLE%", GV_STYLE))
    subprocess.run(["dot", "-Tpng", f"-Gdpi={dpi}", src, "-o", out], check=True)
    return out


def image_size(path):
    from reportlab.lib.utils import ImageReader
    return ImageReader(path).getSize()


def draw_image_fit(c, path, x, y, w, h, anchor="c"):
    """Draw an image scaled to fit (x, y, w, h), keeping its aspect ratio."""
    iw, ih = image_size(path)
    s = min(w / iw, h / ih)
    dw, dh = iw * s, ih * s
    dx = x + (w - dw) / 2 if anchor in ("c", "n", "s") else (x if "w" in anchor else x + w - dw)
    dy = y + (h - dh) / 2 if anchor in ("c", "e", "w") else (y + h - dh if "n" in anchor else y)
    c.drawImage(path, dx, dy, dw, dh, mask="auto")
    return dx, dy, dw, dh


__all__ = [n for n in dir() if not n.startswith("_")] + ["white", "TA_CENTER", "TA_LEFT", "stringWidth"]

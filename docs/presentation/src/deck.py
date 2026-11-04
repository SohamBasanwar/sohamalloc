"""Build docs/presentation/sohamalloc-how-it-works.pdf: the 16:9 slide deck."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa: F401,F403
from reportlab.pdfgen import canvas
from reportlab.platypus import Paragraph, Spacer, Table, TableStyle, XPreformatted

register_fonts()

W, H = 960, 540
M = 48
TOP = 428          # top of the content area
BOT = 44           # bottom of the content area
CW = W - 2 * M     # content width
OUT = os.path.join(os.path.dirname(HERE), "sohamalloc-how-it-works.pdf")

S_BODY = style("body", fontSize=15, leading=20.5)
S_BUL = style("bul", fontSize=15, leading=20, leftIndent=17, bulletIndent=0, bulletFontName="Lato-Bold",
              bulletColor=ACCENT, spaceAfter=8)
S_BUL_S = style("buls", fontSize=13.5, leading=18, leftIndent=16, bulletIndent=0, bulletFontName="Lato-Bold",
                bulletColor=ACCENT, spaceAfter=6)
S_SUB = style("sub", fontSize=13, leading=17, leftIndent=35, bulletIndent=19, textColor=INK2, spaceAfter=4)
S_SMALL = style("small", fontSize=11.5, leading=15, textColor=INK2)
S_NOTE = style("note", fontName="Lato-Italic", fontSize=10.5, leading=13.5, textColor=MUTED)
S_CODE = style("code", fontName="Mono", fontSize=10.4, leading=14)
S_CODE_M = style("codem", fontName="Mono", fontSize=11.5, leading=15.2)
S_CODE_L = style("codel", fontName="Mono", fontSize=11.8, leading=15.6)
S_TXT = style("txt", fontSize=13, leading=17, textColor=INK2)
S_BOXT = style("boxt", fontName="Lato-Bold", fontSize=14, leading=17.5)
S_BOXD = style("boxd", fontSize=12, leading=15.5, textColor=INK2)
S_CELL = style("cell", fontSize=11.5, leading=14.5)
S_CELLB = style("cellb", fontName="Lato-Bold", fontSize=11.5, leading=14.5)
S_HEAD = style("head", fontName="Lato-Bold", fontSize=11, leading=14, textColor=ACCENT)


def bul(t, s=S_BUL):
    return Paragraph(t, s, bulletText="•")


def sub(t):
    return Paragraph(t, S_SUB, bulletText="–")


def P(t, s=S_BODY):
    return Paragraph(t, s)


def frame(c, n, N, kicker, title, section):
    c.setFillColor(white)
    c.rect(0, 0, W, H, fill=1, stroke=0)
    c.setFillColor(ACCENT)
    c.rect(M, 500, 30, 4, fill=1, stroke=0)
    c.setFont("Lato-Bold", 10.5)
    c.drawString(M, 482, kicker.upper(), charSpace=1.3)
    size = fit_font(title, "Lato-Bold", 27, CW)
    c.setFont("Lato-Bold", size)
    c.setFillColor(INK)
    c.drawString(M, 450, title)
    c.setStrokeColor(RULE)
    c.setLineWidth(0.8)
    c.line(M, 36, W - M, 36)
    c.setFont("Lato", 9)
    c.setFillColor(MUTED)
    c.drawString(M, 22, "sohamalloc  ·  " + section)
    c.drawRightString(W - M, 22, f"{n} / {N}")


KW = r"\b(if|return|void|unsigned|until|loop|while)\b"


def code_block(src, w, pad=12, s=S_CODE):
    lines = []
    for line in src.strip("\n").split("\n"):
        code, cm = line, ""
        if "//" in line:
            i = line.index("//")
            code, cm = line[:i], line[i:]
        code = re.sub(KW, r'<font name="Mono-Bold" color="#1c5cab">\1</font>', esc(code))
        if cm:
            code += f'<font name="Mono-Oblique" color="#7a7973">{esc(cm)}</font>'
        lines.append(code)
    x = XPreformatted("\n".join(lines), s)
    t = Table([[x]], colWidths=[w])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), PANEL),
        ("BOX", (0, 0), (-1, -1), 0.8, RULE),
        ("LEFTPADDING", (0, 0), (-1, -1), pad), ("RIGHTPADDING", (0, 0), (-1, -1), pad),
        ("TOPPADDING", (0, 0), (-1, -1), pad - 2), ("BOTTOMPADDING", (0, 0), (-1, -1), pad - 2),
    ]))
    return t


def box_text(c, x, y, w, h, title, desc, fill=PANEL, stroke=RULE, pad=12, tstyle=S_BOXT, dstyle=S_BOXD,
             valign="middle"):
    rbox(c, x, y, w, h, fill=fill, stroke=stroke)
    fl = [Paragraph(title, tstyle)]
    if desc:
        fl.append(Spacer(1, 3))
        fl.append(Paragraph(desc, dstyle))
    hh = height_of(fl, w - 2 * pad)
    top = y + h - pad if valign == "top" else y + (h + hh) / 2
    place(c, fl, x + pad, top, w - 2 * pad, hh + 2, title)


def table(data, widths, header=True, zebra=False, hl_rows=(), font=11.5, align_right_from=None, pad=6):
    rows = []
    for i, r in enumerate(data):
        cells = []
        for j, v in enumerate(r):
            if isinstance(v, str):
                st = S_HEAD if (header and i == 0) else S_CELL
                if i in hl_rows and not (header and i == 0):
                    st = S_CELLB
                st = style(f"t{i}{j}", parent=None, fontName=st.fontName, fontSize=font if st is not S_HEAD else font - 0.5,
                           leading=font * 1.27, textColor=st.textColor,
                           alignment=2 if (align_right_from is not None and j >= align_right_from) else 0)
                v = Paragraph(v, st)
            cells.append(v)
        rows.append(cells)
    t = Table(rows, colWidths=widths)
    cmds = [
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), pad), ("RIGHTPADDING", (0, 0), (-1, -1), pad),
        ("TOPPADDING", (0, 0), (-1, -1), pad - 2), ("BOTTOMPADDING", (0, 0), (-1, -1), pad - 1),
        ("LINEBELOW", (0, 0), (-1, -1), 0.6, RULE),
    ]
    if header:
        cmds += [("LINEBELOW", (0, 0), (-1, 0), 1.2, ACCENT_LINE)]
    for r in hl_rows:
        cmds.append(("BACKGROUND", (0, r), (-1, r), ACCENT_SOFT))
    t.setStyle(TableStyle(cmds))
    return t


# --------------------------------------------------------------------------- data
# sigstorm campaign 1 (other allocators, 540 signal trials each) and the sohamalloc rerun on d2add24 (630)
STORM = [
    # name, trials, stall, crash, corruption, p, lo, hi
    ("sohamalloc", 630, 0, 0, 0, 0.000, 0.000, 0.006),
    ("tbbmalloc", 540, 28, 242, 0, 0.500, 0.458, 0.542),
    ("scudo", 540, 293, 5, 44, 0.633, 0.592, 0.673),
    ("mimalloc", 540, 0, 300, 57, 0.661, 0.620, 0.700),
    ("snmalloc", 540, 77, 267, 103, 0.830, 0.796, 0.859),
    ("jemalloc", 540, 373, 85, 10, 0.867, 0.835, 0.893),
    ("glibc", 540, 484, 0, 0, 0.896, 0.868, 0.919),
    ("tcmalloc", 540, 373, 151, 16, 1.000, 0.993, 1.000),
    ("hardened_malloc", 540, 540, 0, 0, 1.000, 0.993, 1.000),
]
RSS_AFTER_FREE = [("sohamalloc", 50), ("tcmalloc", 628), ("glibc", 1017), ("mimalloc", 1035), ("jemalloc", 1070)]
B1B4 = [
    ("malloc-large", 2.88, 1.38), ("sh8benchN", 1.71, 1.07), ("glibc-thread", 1.54, 1.05),
    ("larsonN-sized", 1.40, 1.20), ("alloc-test1", 1.40, 1.21), ("xmalloc-testN", 1.38, 1.23),
    ("larsonN", 1.24, 1.07),
    None,
    ("cache-scratchN", 1.00, 1.32), ("cache-thrashN", 1.10, 1.31), ("glibc-simple", 1.17, 1.25),
    ("barnes", 1.00, 1.20),
]


# --------------------------------------------------------------------------- slides
def s_title(c, n, N):
    c.setFillColor(white)
    c.rect(0, 0, W, H, fill=1, stroke=0)
    c.setFillColor(ACCENT)
    c.rect(M, 404, 44, 5, fill=1, stroke=0)
    c.setFont("Lato-Black", 66)
    c.setFillColor(INK)
    c.drawString(M - 3, 318, "sohamalloc")
    s = style("tsub", fontName="Lato-Light", fontSize=25, leading=33, textColor=INK)
    place(c, [P("A general-purpose memory allocator that is <b>safe to call from signal handlers</b>, "
                "hardened by default, and quick to give memory back.", s)], M, 300, 720, 112, "title")
    c.setFont("Lato-Bold", 15)
    c.setFillColor(INK)
    c.drawString(M, 172, "Soham Basanwar")
    c.setFont("Lato", 13.5)
    c.setFillColor(INK2)
    c.drawString(M, 151, "Project review  ·  November 2026")
    c.setFont("Lato", 11.5)
    c.setFillColor(MUTED)
    c.drawString(M, 118, "C11  ·  drop-in malloc / free / new / delete via LD_PRELOAD  ·  "
                         "Linux x86-64, arm64, armhf, musl  ·  Windows (soham_* API)")
    # decorative hugeblock: 32 slices in their page-heap states
    states = "UUUUUDDUUUUZZUUUDUUUUUURRZZZZZZZ"
    fills = {"U": SOHAM, "D": ACCENT_LINE, "Z": PANEL2, "R": STALL}
    x0, y0, cw = M, 60, (CW - 31 * 3) / 32
    for i, s_ in enumerate(states):
        c.setFillColor(fills[s_])
        c.roundRect(x0 + i * (cw + 3), y0, cw, 18, 2.5, stroke=0, fill=1)
    c.setFont("Lato", 9)
    c.setFillColor(MUTED)
    c.drawString(M, 46, "one 2 MiB hugeblock = 32 slices of 64 KiB:  used · free and resident · "
                        "free and zero · being returned to the kernel")


def s_problem(c, n, N):
    frame(c, n, N, "Motivation", "Signal handlers call malloc, and allocators break", "motivation")
    fl = [
        bul("A signal can interrupt a thread at <b>any instruction</b>, including inside <font name='Mono'>malloc</font>."),
        bul("POSIX: <font name='Mono'>malloc</font> and <font name='Mono'>free</font> are <b>not async-signal-safe</b>. "
            "A handler that calls them has undefined behaviour."),
        bul("Real software does it anyway, through <font name='Mono'>syslog</font>, logging, backtraces, C++ "
            "<font name='Mono'>new</font>:"),
        sub("<b>OpenSSH sshd, CVE-2024-6387 (“regreSSHion”)</b>: the SIGALRM handler reached malloc via syslog, "
            "giving remote code execution as root on glibc Linux."),
        sub("Hangs reported in GCC, GNU make, Perl, g3log, a jemalloc user; a Red Hat note titled “permanent deadlock”."),
        bul("Current workarounds change the program (defer work to a safe point) or block signals around "
            "every allocation (two system calls per call)."),
    ]
    place(c, fl, M, TOP, 480, 384, "problem")
    # sequence: how it fails
    x, w = 570, 342
    c.setFont("Lato-Bold", 12)
    c.setFillColor(INK2)
    c.drawString(x, TOP - 8, "HOW IT FAILS (ONE THREAD)", charSpace=1)
    box_text(c, x, 330, w, 66, "malloc() runs",
             "takes the heap lock, starts updating a free list", fill=PANEL)
    arrow(c, x + w / 2, 330, x + w / 2, 302, color=STALL, lw=2)
    c.setFont("Lato-Bold", 11)
    c.setFillColor(STALL)
    c.drawString(x + w / 2 + 10, 312, "signal arrives (SIGALRM)")
    box_text(c, x, 236, w, 64, "handler → syslog() → malloc()", "the same thread re-enters the allocator",
             fill=ACCENT_SOFT, stroke=ACCENT_LINE)
    arrow(c, x + 80, 236, x + 80, 204, lw=1.6)
    arrow(c, x + w - 80, 236, x + w - 80, 204, lw=1.6)
    box_text(c, x, 96, w / 2 - 6, 106, "Deadlock",
             "waits for the heap lock that its own thread holds", fill=BAD_SOFT, stroke=HexColor("#e9b4b0"),
             valign="top")
    box_text(c, x + w / 2 + 6, 96, w / 2 - 6, 106, "Heap corruption",
             "a lock-free path pops a half-updated list", fill=BAD_SOFT, stroke=HexColor("#e9b4b0"), valign="top")
    place(c, [P("In our signal-storm test, all eight other allocators fail in 50–100% of trials.", S_NOTE)],
          x, 84, w, 30, "probnote")


def s_thesis(c, n, N):
    frame(c, n, N, "Thesis and results", "Signal-safe malloc at competitive speed", "overview")
    s = style("thesis", fontName="Lato-Light", fontSize=19, leading=26, textColor=INK)
    place(c, [P("A general-purpose, thread-caching allocator can make <b>the whole malloc API async-signal-safe</b> "
                "with no system call on the fast path, while staying as fast as the best allocators.", s)],
          M, TOP, CW, 60, "thesis")
    tiles = [
        ("0 / 630", "signal-storm trials failed", "Final code; 95% CI 0–0.6%. The eight other allocators tested "
         "failed in 50–100% of their trials."),
        ("1.142", "time + memory score, tied first", "24 benchmarks, 5 allocators: mimalloc 1.142, glibc 1.276, "
         "jemalloc 1.344, tcmalloc 1.451 (lower is better)."),
        ("50 MB", "resident after freeing 1 GiB", "Without malloc_trim. glibc, jemalloc and mimalloc keep about "
         "1 GiB; tcmalloc keeps 628 MB."),
        ("1–7%", "cost of the default-on checks", "Encoded free lists and double/invalid-free detection, "
         "measured on allocation-heavy microbenchmarks."),
    ]
    tw, th, gap = (CW - 16) / 2, 118, 16
    for i, (big, label, desc) in enumerate(tiles):
        col, row = i % 2, i // 2
        x = M + col * (tw + gap)
        y = 238 - row * (th + 14)
        rbox(c, x, y, tw, th, fill=ACCENT_SOFT if i == 0 else PANEL, stroke=ACCENT_LINE if i == 0 else RULE)
        c.setFont("Lato-Black", 36)
        c.setFillColor(ACCENT)
        c.drawString(x + 18, y + th - 50, big)
        bw = stringWidth(big, "Lato-Black", 36)
        c.setFont("Lato-Bold", 14)
        c.setFillColor(INK)
        c.drawString(x + 30 + bw, y + th - 44, label)
        place(c, [P(desc, style("td", fontSize=12, leading=15.5, textColor=INK2))], x + 18, y + th - 64, tw - 36, 48,
              "tile")
    place(c, [P("Benchmarks: campaign B4 on a 4-vCPU VM, 3 repetitions, code of 2026-10-19. A tuned campaign with "
                "≥10 repetitions on the final code is still to run.", S_NOTE)], M, 70, CW, 28, "thesisnote")


def s_arch(c, n, N):
    frame(c, n, N, "Design overview", "Architecture: five layers and three helpers", "architecture")
    layers = [
        ("API", "malloc · free · calloc · realloc · aligned_alloc · posix_memalign · "
                "C++ new/delete · soham_* extensions", 52),
        ("Thread heap: one per thread (TLS)", "busy byte · 64 size-class bins · each bin: current page, "
                                              "pages with free blocks, full pages", 62),
        ("Pages", "built from 64 KiB slices: small pages, medium pages, runs · metadata in out-of-line "
                  "192-byte slice records", 62),
        ("Page heap", "2 MiB hugeblocks with slice bitmaps · best-fit placement · release engine "
                      "(decay, excess, all)", 62),
        ("Operating system", "4 GiB windows reserved PROT_NONE, committed on demand · own mmap for blocks "
                             "≥ 32 MiB", 52),
    ]
    x, w, gap = M, 560, 22
    y = TOP
    centers = []
    for i, (t, d, h) in enumerate(layers):
        y -= h
        box_text(c, x, y, w, h, t, d, fill=ACCENT_SOFT if 0 < i < 4 else PANEL,
                 stroke=ACCENT_LINE if 0 < i < 4 else RULE)
        centers.append(y + h / 2)
        if i < len(layers) - 1:
            arrow(c, x + w / 2, y, x + w / 2, y - gap + 2, lw=1.4)
        y -= gap
    sx, sw = 650, 262
    side = [
        ("Granule map", "1 byte per 4 GiB of address space: is this pointer one of ours?", centers[0]),
        ("Shared heap + static arena", "serve signal handlers that interrupted the allocator", centers[1]),
        ("Class pools", "pages of exited threads, claimed by other threads", centers[2]),
    ]
    for t, d, cy in side:
        h = 74
        box_text(c, sx, cy - h / 2, sw, h, t, d, fill=white, stroke=INK2)
        arrow(c, sx, cy, x + w + 4, cy, color=MUTED, lw=1.2, dash=(3, 3))


def s_address(c, n, N):
    frame(c, n, N, "Memory layout", "Address space: windows, hugeblocks, slices", "memory layout")
    segs = [("header", 44, PANEL2, None), ("hugeblock\nbitmaps", 88, ACCENT_SOFT, None),
            ("slice records\n192 B each", 150, ACCENT_SOFT, None), ("guard", 52, None, "hatch"),
            ("hugeblocks, committed as needed", 300, HexColor("#fdf0e6"), None),
            ("reserved, not committed", 170, white, "dash"), ("tail\nguard", 60, None, "hatch")]
    x, y, h = M, 330, 46
    c.setFont("Lato-Bold", 12)
    c.setFillColor(INK2)
    c.drawString(M, 394, "ONE WINDOW: 4 GiB, aligned to 4 GiB, reserved PROT_NONE at creation", charSpace=0.6)
    seg_x = {}
    for label, w, fill, kind in segs:
        if kind == "hatch":
            c.setFillColor(white)
            c.setStrokeColor(MUTED)
            c.rect(x, y, w, h, fill=1, stroke=1)
            hatch(c, x, y, w, h)
        elif kind == "dash":
            c.saveState()
            c.setDash(3, 3)
            c.setStrokeColor(MUTED)
            c.setFillColor(white)
            c.rect(x, y, w, h, fill=1, stroke=1)
            c.restoreState()
        else:
            c.setFillColor(fill)
            c.setStrokeColor(INK2)
            c.rect(x, y, w, h, fill=1, stroke=1)
        lines = label.split("\n")
        fs = 10.5 if w < 100 else 11.5
        for k, ln in enumerate(lines):
            c.setFont("Lato-Bold" if kind is None else "Lato", fs)
            c.setFillColor(INK if kind is None else INK2)
            yy = y + h / 2 + (len(lines) - 1) * 6.5 - k * 13 - 4
            if kind == "hatch":
                c.setFillColor(white)
                tw = stringWidth(ln, "Lato", fs)
                c.rect(x + (w - tw) / 2 - 2, yy - 2.5, tw + 4, fs + 1, fill=1, stroke=0)
                c.setFillColor(INK2)
            c.drawCentredString(x + w / 2, yy, ln)
        seg_x[label] = (x, w)
        x += w
    c.setFont("Lato", 10.5)
    c.setFillColor(MUTED)
    c.drawString(M, 314, "metadata (committed at creation)")
    c.drawRightString(W - M, 314, "data")
    # zoom: one hugeblock
    hx, hw_ = seg_x["hugeblocks, committed as needed"]
    zx, zy, zw, zh = M, 200, CW, 40
    c.setStrokeColor(MUTED)
    c.setLineWidth(0.8)
    c.setDash(2, 2)
    c.line(hx + 20, y, zx, zy + zh + 28)
    c.line(hx + 80, y, zx + zw, zy + zh + 28)
    c.setDash()
    states = "UUUUUUUUDDUUUUZZUUUDUUUUUURRZZZZ"
    fills = {"U": SOHAM, "D": ACCENT_LINE, "Z": PANEL2, "R": STALL}
    cw = (zw - 31 * 3) / 32
    for i, s_ in enumerate(states):
        c.setFillColor(fills[s_])
        c.roundRect(zx + i * (cw + 3), zy, cw, zh, 3, stroke=0, fill=1)
    c.setFont("Lato-Bold", 12)
    c.setFillColor(INK2)
    c.drawString(M, zy + zh + 12, "ONE HUGEBLOCK: 2 MiB = 32 SLICES OF 64 KiB", charSpace=0.6)
    lx = M + 370
    for lab, col in [("used", SOHAM), ("free, resident (dirty)", ACCENT_LINE), ("free, zero", PANEL2),
                     ("being released", STALL)]:
        c.setFillColor(col)
        c.roundRect(lx, zy + zh + 11, 10, 10, 2, stroke=0, fill=1)
        c.setFillColor(INK2)
        c.setFont("Lato", 11)
        c.drawString(lx + 14, zy + zh + 12, lab)
        lx += 28 + stringWidth(lab, "Lato", 11)
    fl_l = [
        P("<b>Pointer → metadata in O(1), no search</b>", S_BODY),
        Spacer(1, 4),
        P("Granule map: one byte per 4 GiB of the 48-bit space (65,536 entries). "
          "Zero means “not ours”, so frees of stack, global or foreign pointers are caught.", S_SMALL),
        Spacer(1, 6),
        P("Windows are reserved, not committed: data is committed in 2–64 MiB steps as the heap grows.",
          S_SMALL),
    ]
    fl_r = [
        code_block("g   = gmap[(a >> 32) & 0xffff];          // ours?\n"
                   "rec = (a & ~(W-1)) + rec_off\n"
                   "    + ((a & (W-1)) >> 16) * 192;          // its record", 420, pad=10),
    ]
    place(c, fl_l, M, 168, 410, 124, "addr-l")
    place(c, fl_r, M + 444, 172, 420, 124, "addr-r")


def s_records(c, n, N):
    frame(c, n, N, "Memory layout", "Slice records: three cache lines, three writers", "memory layout")
    x, w, lh = M + 40, 470, 72
    lines = [
        ("O", "owner line", "written only by the owning thread",
         "local free list (head, count) · bump offset · list links · owner copies of key, base"),
        ("R", "read-mostly line", "written at page setup, claim, detach",
         "owner tag · block size · capacity · random key · class · kind · divm"),
        ("X", "remote line", "written by other threads",
         "remote-free word (count | head | flags) · pool links · notify and pending states"),
    ]
    y = TOP - 10
    ys = {}
    for tag, name, who, fields in lines:
        y -= lh
        rbox(c, x, y, w, lh - 6, fill=ACCENT_SOFT if tag != "R" else PANEL, stroke=ACCENT_LINE if tag != "R" else RULE, r=6)
        c.setFont("Lato-Black", 26)
        c.setFillColor(ACCENT)
        c.drawCentredString(x - 22, y + (lh - 6) / 2 - 9, tag)
        place(c, [P(f"<b>{name}</b> <font color='#52514e'>— {who}</font>", style("rl", fontSize=13, leading=16.5)),
                  Spacer(1, 3), P(fields, style("rf", fontSize=11.5, leading=14.5, textColor=INK2))],
              x + 12, y + lh - 14, w - 24, lh - 12, "rec" + tag)
        ys[tag] = y + (lh - 6) / 2
    c.setFont("Lato", 10.5)
    c.setFillColor(MUTED)
    c.drawString(x, y - 16, "192 bytes = 3 × 64-byte cache lines, one record per 64 KiB slice, outside the data area")
    # actors
    ax, aw = 640, 272
    box_text(c, ax, ys["O"] - 26, aw, 52, "Owning thread", "malloc and local free", fill=white, stroke=INK2)
    box_text(c, ax, ys["X"] - 26, aw, 52, "Other threads", "remote free: one CAS", fill=white, stroke=INK2)
    arrow(c, ax, ys["O"], x + w + 4, ys["O"], color=SOHAM, lw=2)
    arrow(c, ax + 30, ys["O"] - 26, x + w + 4, ys["R"] + 8, color=MUTED, lw=1.2, dash=(3, 3))
    arrow(c, ax, ys["X"], x + w + 4, ys["X"], color=STALL, lw=2)
    arrow(c, ax + 30, ys["X"] + 26, x + w + 4, ys["R"] - 8, color=MUTED, lw=1.2, dash=(3, 3))
    c.setFont("Lato", 10)
    c.setFillColor(MUTED)
    c.drawString(ax + 40, ys["R"] - 3, "both only read line R")
    fl = [
        bul("The owner's malloc touches only line O and the block itself.", S_BUL_S),
        bul("A remote free reads R and writes X: it never touches O, so the owner's line does not "
            "bounce between cores.", S_BUL_S),
        bul("No allocator metadata sits next to user data; a heap overflow cannot reach it.", S_BUL_S),
    ]
    place(c, fl, M, 150, CW, 110, "records-b")


def s_classes(c, n, N):
    frame(c, n, N, "Memory layout", "Size classes and the four ways a block is served", "memory layout")
    import math
    x0, x1, y = M + 10, W - M - 10, 300
    lo, hi = math.log2(16), math.log2(2 ** 30)

    def X(b):
        return x0 + (math.log2(b) - lo) / (hi - lo) * (x1 - x0)

    bands = [
        (16, 8 * 1024, "Small", "classes 0–43 · one 64 KiB slice per page", SOHAM),
        (8 * 1024, 256 * 1024, "Medium", "multi-slice pages ≤ 1 MiB, tail waste ≤ 3.125%", ACCENT_LINE),
        (256 * 1024, 32 * 2 ** 20, "Runs", "whole slices from the page heap", HexColor("#f6c7ad")),
        (32 * 2 ** 20, 2 ** 30, "Solos", "own mmap each", PANEL2),
    ]
    for a, b, name, desc, col in bands:
        c.setFillColor(col)
        c.rect(X(a), y, X(b) - X(a) - 2, 40, fill=1, stroke=0)
        c.setFont("Lato-Bold", 14)
        c.setFillColor(INK if col is not SOHAM else white)
        c.drawString(X(a) + 8, y + 15, name)
        place(c, [P(desc, style("bd", fontSize=11.5, leading=14.5, textColor=INK2))], X(a) + 2, y - 8,
              X(b) - X(a) - 10, 40, name)
    c.setStrokeColor(INK2)
    c.setLineWidth(0.8)
    for b, lab in [(16, "16 B"), (128, "128 B"), (1024, "1 KiB"), (8 * 1024, "8 KiB"), (128 * 1024, "128 KiB"),
                   (2 * 2 ** 20, "2 MiB"), (32 * 2 ** 20, "32 MiB"), (2 ** 30, "1 GiB")]:
        c.line(X(b), y + 40, X(b), y + 47)
        c.setFont("Lato", 11)
        c.setFillColor(INK2)
        c.drawCentredString(X(b), y + 52, lab)
    c.setFont("Lato", 10)
    c.setFillColor(MUTED)
    c.drawString(x0, y + 70, "block size (log scale)")
    # fast path range
    c.setStrokeColor(SOHAM)
    c.setLineWidth(2)
    c.line(X(16), y - 52, X(128 * 1024), y - 52)
    c.setFont("Lato-Bold", 11.5)
    c.setFillColor(ACCENT)
    c.drawString(X(16), y - 68, "malloc fast path: every class up to 128 KiB (classes 0–59)")
    data = [
        ["Range", "Class spacing", "Rounding loss per block"],
        ["16 B – 128 B", "16-byte steps (8 classes)", "at most 15 bytes"],
        ["128 B – 1 KiB", "8 classes per doubling", "under 12.5%"],
        ["1 KiB – 256 KiB", "4 classes per doubling", "under 25%"],
        ["> 256 KiB", "page-heap runs, exact slices", "under one 64 KiB slice"],
    ]
    t = table(data, [190, 260, 230], font=12)
    place(c, [t], M, 202, 680, 150, "classes-t")
    place(c, [P("The 192 KiB and 256 KiB classes are served as runs. 64 classes in all; size → class is a "
                "table lookup up to 1 KiB and a few shifts above.", S_NOTE)], M + 704, 196, 160, 140, "classes-n")


def s_malloc(c, n, N):
    frame(c, n, N, "Fast paths", "malloc fast path: no locks, no atomic read-modify-write", "fast paths")
    src = """
void *soham_malloc(size_t n) {
  tls_t *t = &t_tls;                     // initial-exec TLS
  if (t->busy == 0 && n <= FAST_MAX) {   // up to 128 KiB
    unsigned c = size_class(n);          // table lookup to 1 KiB
    t->busy = BUSY_IN;                   // a signal now sees "inside"
    rec_t *r = t->heap->bins[c].cur;     // current page of class c
    uintptr_t b = r->fhead;              // first free block (line O)
    if (b) {
      uintptr_t nx = link_dec(r->key, *(uintptr_t *)b, b);
      if (nx && !exact_block(r, nx))     // must be a block boundary
        fatal("free-list link overwritten");
      *(uintptr_t *)b = 0;               // aids double-free detection
      r->fhead = nx;  r->fcnt--;
    }
    t->busy = 0;
    if (b) return (void *)b;
  }
  return malloc_slow(n);  // refill, large sizes, handler context
}"""
    place(c, [code_block(src, 540, s=S_CODE_L)], M, TOP, 540, 384, "malloc-code")
    steps = [
        ("1", "One TLS byte decides", "busy = 0: a normal call. Set: this call interrupted the allocator, so it "
                                      "takes the handler path."),
        ("2", "Class lookup", "a table for ≤ 1 KiB, a few shifts above"),
        ("3", "Pop the current page's list", "owner-only cache line, plain loads and stores"),
        ("4", "Validate the next link", "decoded link must be an exact block of this page, or abort"),
        ("5", "Clear the link word", "makes double-free detection on free cheap"),
    ]
    y = TOP
    for num, t, d in steps:
        c.setFillColor(ACCENT)
        c.circle(616, y - 9, 10, stroke=0, fill=1)
        c.setFillColor(white)
        c.setFont("Lato-Bold", 11.5)
        c.drawCentredString(616, y - 13, num)
        fl = [P(f"<b>{t}</b>", style("st", fontSize=14, leading=17.5)),
              P(d, style("sd", fontSize=12.5, leading=16, textColor=INK2))]
        hh = height_of(fl, 272)
        place(c, fl, 636, y, 276, hh + 4, "step" + num)
        y -= hh + 14
    place(c, [P("About 45 instructions in all (callgrind); mimalloc's is about 18. The difference buys the checks "
                "and the busy byte.", S_NOTE)], 600, y - 4, 312, 60, "malloc-note")


def s_free(c, n, N):
    frame(c, n, N, "Fast paths", "free fast path: is the pointer ours, and is the page mine?", "fast paths")
    src = """
void soham_free(void *p) {
  uintptr_t a = (uintptr_t)p;
  if (gmap[(a >> 32) & 0xffff] == GM_WINDOW   // one of our windows?
      && t_tls.busy == 0) {
    rec_t *r = rec_of(a);                     // address arithmetic
    t_tls.busy = BUSY_IN;
    if (r->head_delta) r = rec_head(r);       // multi-slice page
    if (r->owner == t_tls.tag) {              // my page: local free
      owned_free(t_tls.heap, r, a);           // checks, push on line O
      t_tls.busy = 0;
      return;
    }
    t_tls.busy = 0;
  }
  free_slow(p);  // remote free (1 CAS), runs, solos, foreign pointers
}"""
    place(c, [code_block(src, 556, s=S_CODE_L)], M, TOP, 556, 384, "free-code")
    fl = [
        P("<b>Local free</b> (my page)", style("h", fontSize=15.5, leading=20)),
        P("Checks the exact block boundary, then one cheap test: does the block's first word decode to 0 or to an "
          "address in its page? Only then a cold path walks the lists and aborts on a double free.", S_TXT),
        Spacer(1, 10),
        P("<b>Remote free</b> (another thread's page)", style("h2", fontSize=15.5, leading=20)),
        P("One compare-and-swap on the page's remote line; the owner is notified only when it pays (next slides).",
          S_TXT),
        Spacer(1, 10),
        P("<b>Not ours</b>", style("h3", fontSize=15.5, leading=20)),
        P("Stack, global or foreign pointer: leaked with a warning (aborted in secure mode), never written to.",
          S_TXT),
        Spacer(1, 10),
        P("About 79 instructions on average (callgrind, glibc-simple); mimalloc's free is about 29.", S_NOTE),
    ]
    place(c, fl, 626, TOP, 286, 384, "free-r")


def s_hardening(c, n, N):
    frame(c, n, N, "Security", "Hardening that is on by default", "security")
    colw = (CW - 24) / 2
    l = [
        P("<b>Encoded free-list links</b>", style("h", fontSize=16, leading=20)),
        Spacer(1, 4),
        code_block("stored = bswap(next ^ key ^ (addr >> 12))", colw, pad=10, s=S_CODE_M),
        Spacer(1, 6),
        P("Per-page random key. A partial overwrite scrambles the high bits, the decoded link fails the boundary "
          "check, and the allocator aborts.", S_TXT),
        Spacer(1, 14),
        P("<b>Exact block check, no division</b>", style("h", fontSize=16, leading=20)),
        Spacer(1, 4),
        code_block("divm = UINT64_MAX / bsize + 1;\nok   = off * divm < divm;  // off % bsize == 0", colw, pad=10, s=S_CODE_M),
        Spacer(1, 6),
        P("Lemire, Kaser and Kurz (2019). Applied to every decoded link and every free.", S_TXT),
    ]
    r = [
        P("<b>Invalid and double frees abort</b>", style("h", fontSize=16, leading=20)),
        Spacer(1, 4),
        bul("interior pointers, and non-adjacent double frees (<font name='Mono'>free(a); free(b); free(a)</font>)",
            S_BUL),
        bul("double frees into another thread's remote list, and into pages of exited threads", S_BUL),
        bul("foreign pointers are leaked with a warning, never written to", S_BUL),
        Spacer(1, 8),
        P("<b>Cost: 1–7%</b> on allocation-heavy microbenchmarks (glibc-simple 7%, cfrac 4%, alloc-test and "
          "espresso 1%).", S_TXT),
        Spacer(1, 8),
        P("<b>Secure tier S1</b> (opt-in): shuffled block order, zeroing on free, abort on foreign pointers.",
          S_TXT),
        Spacer(1, 8),
        P("Honest limit: someone who can <i>read</i> free blocks can recover keys. Encoding raises the bar; it is "
          "not a guarantee.", S_NOTE),
    ]
    place(c, l, M, TOP, colw, 384, "hard-l")
    place(c, r, M + colw + 24, TOP, colw, 384, "hard-r")


S_NT = style("nt", fontName="Lato-Bold", fontSize=12.5, leading=15)
S_ND = style("nd", fontSize=11, leading=13.5, textColor=INK2)


def node(c, x, y, w, h, title, desc=None, fill=PANEL, stroke=RULE):
    box_text(c, x, y, w, h, title, desc, fill=fill, stroke=stroke, pad=9, tstyle=S_NT, dstyle=S_ND)


def poly_arrow(c, pts, color=INK2, lw=1.4, dash=None):
    if len(pts) > 2:
        c.saveState()
        c.setStrokeColor(color)
        c.setLineWidth(lw)
        if dash:
            c.setDash(*dash)
        p = c.beginPath()
        p.moveTo(*pts[0])
        for q in pts[1:-1]:
            p.lineTo(*q)
        c.drawPath(p, stroke=1, fill=0)
        c.restoreState()
    arrow(c, pts[-2][0], pts[-2][1], pts[-1][0], pts[-1][1], color=color, lw=lw, dash=dash)


def lab(c, x, y, text, align="l", size=10.5, color=INK2, font="Lato"):
    c.setFont(font, size)
    c.setFillColor(color)
    for k, ln in enumerate(text.split("\n")):
        yy = y - k * (size + 2.5)
        if align == "l":
            c.drawString(x, yy, ln)
        elif align == "r":
            c.drawRightString(x, yy, ln)
        else:
            c.drawCentredString(x, yy, ln)


def s_remote(c, n, N):
    frame(c, n, N, "Threads", "Cross-thread frees: one CAS, and the owner is told when it pays", "threads")
    src = """
do {
  old = page->xfree;               // count | head | flags (line X)
  *block = enc(old.head);          // link into the remote chain
  new = { old.count + 1, block, old.flags };
  if (WATCH && old.count + 1 >= cap/8)
    new.flags &= ~WATCH;           // this freer notifies the owner
} while (!CAS(&page->xfree, old, new));"""
    place(c, [code_block(src, 492, s=style("c115", fontName="Mono", fontSize=11.3, leading=15))], M, TOP, 492, 150,
          "remote-code")
    fl = [
        bul("<b>Checks on every remote free:</b> head == block (an immediate double free) and count > capacity.",
            S_BUL_S),
        bul("<b>What did not work first:</b> notifying the owner at the <i>first</i> free. Each refill then got a "
            "single block, and 28% of mallocs took the slow path (alloc-test).", S_BUL_S),
        bul("<b>Fix:</b> a threshold of cap/8, for remote and for local frees. Slow path 28% → 0.07%; "
            "alloc-test 2,228 → 1,966 ms. Cost: up to cap/8 − 1 idle blocks per full page.", S_BUL_S),
        Spacer(1, 2),
        P("Built on mimalloc's free-list sharding and delayed free (APLAS 2019); the threshold follows Hoard's "
          "emptiness hysteresis and mimalloc v3's 1/8 rule.", S_NOTE),
    ]
    place(c, fl, M, 262, 492, 220, "remote-b")
    # the page's life on its owner's lists
    x0, bw, bh = 600, 214, 46
    cx = x0 + bw / 2
    c.setFont("Lato-Bold", 11)
    c.setFillColor(INK2)
    c.drawString(x0 - 32, TOP - 8, "A PAGE ON ITS OWNER'S LISTS", charSpace=1)
    ys = [362, 262, 162, 62]
    boxes = [("current page", "malloc pops blocks here", ACCENT_SOFT, ACCENT_LINE),
             ("full list, WATCH armed", "no free blocks left", PANEL, RULE),
             ("owner's notify stack", "lock-free; pushed by a freer", PANEL, RULE),
             ("avail list", "pages with free blocks", PANEL, RULE)]
    for (t, d, f, s_), y in zip(boxes, ys):
        node(c, x0, y, bw, bh, t, d, fill=f, stroke=s_)
    labels = ["exhausted", "remote frees reach cap/8:\nthat freer's CAS clears\nWATCH and pushes the page",
              "the owner's next refill\ndrains the stack"]
    for k in range(3):
        arrow(c, cx, ys[k], cx, ys[k + 1] + bh + 2, lw=1.6)
        lab(c, cx + 10, ys[k] - 20, labels[k], size=10.5)
    poly_arrow(c, [(x0, ys[3] + bh / 2), (x0 - 18, ys[3] + bh / 2), (x0 - 18, ys[0] + bh / 2), (x0 - 2, ys[0] + bh / 2)],
               color=SOHAM, lw=1.6)
    c.saveState()
    c.translate(x0 - 26, (ys[3] + ys[0] + bh) / 2)
    c.rotate(90)
    c.setFont("Lato-Bold", 10.5)
    c.setFillColor(ACCENT)
    c.drawCentredString(0, 0, "a refill makes it current again")
    c.restoreState()


def s_orphan(c, n, N):
    frame(c, n, N, "Threads", "Threads that exit leave no stranded memory", "threads")
    y, h = 300, 54
    xs = [(48, 128), (229, 118), (400, 150), (603, 128), (784, 128)]
    nodes = [("owned by a", "live thread"), ("ORPHAN", "no owner"), ("PENDING", "one thread responsible"),
             ("QUEUED", "in the class pool"), ("owned by", "another thread")]
    fills = [(ACCENT_SOFT, ACCENT_LINE), (PANEL, RULE), (PANEL, RULE), (PANEL, RULE), (ACCENT_SOFT, ACCENT_LINE)]
    for (x, w), (t, d), (f, s_) in zip(xs, nodes, fills):
        node(c, x, y, w, h, t, d, fill=f, stroke=s_)
    elabels = ["thread exits:\ndetach", "frees reach cap/8\n(the crossing CAS)", "resolve:\nenough free",
               "a refill\nclaims it"]
    for k in range(4):
        x1 = xs[k][0] + xs[k][1]
        x2 = xs[k + 1][0]
        arrow(c, x1, y + h / 2, x2 - 1, y + h / 2, lw=1.6)
        lab(c, (x1 + x2) / 2, y + h + 18, elabels[k], align="c")
    # PENDING -> released (down), QUEUED -> PENDING (back edge below)
    pcx = xs[2][0] + xs[2][1] / 2
    node(c, xs[2][0], 182, xs[2][1], 52, "released to", "the page heap", fill=GOOD_SOFT, stroke=HexColor("#9fcca3"))
    arrow(c, pcx, y, pcx, 236, lw=1.6)
    lab(c, pcx - 8, 274, "resolve: all blocks free\n(chain walked, lists checked)", align="r")
    qcx = xs[3][0] + xs[3][1] / 2
    bx = xs[2][0] + xs[2][1] - 20
    poly_arrow(c, [(qcx, y), (qcx, 272), (bx, 272), (bx, y - 1)], lw=1.4)
    lab(c, (qcx + bx) / 2, 258, "last block freed", align="c")
    place(c, [P("Prior art: Hoard's global heap (ASPLOS 2000); mimalloc v3's abandoned pages, with the same 1/8 rule.",
                S_NOTE)], 600, 236, 312, 40, "orph-n")
    fl_l = [
        bul("Every state change is a CAS on the page's remote-free word. The thread whose CAS makes the change "
            "becomes responsible, so no page is lost or handled twice.", S_BUL_S),
        bul("Detach clears the owner <i>before</i> publishing ORPHAN, so a later claimer's owner store can never "
            "be overwritten.", S_BUL_S),
    ]
    fl_r = [
        bul("Before a page is released or claimed, its remote chain is walked exactly and checked against its local "
            "list: a double free split across both lists aborts (review round 4).", S_BUL_S),
        bul("Signal handlers never resolve pages: they push them on a pending stack for the next slow path.", S_BUL_S),
    ]
    colw = (CW - 30) / 2
    place(c, fl_l, M, 166, colw, 124, "orph-l")
    place(c, fl_r, M + colw + 30, 166, colw, 124, "orph-r")


def s_pageheap(c, n, N):
    frame(c, n, N, "Memory return", "Page heap: hugepage-aware, and it gives memory back", "memory return")
    fl = [
        bul("Each 2 MiB hugeblock keeps four slice bitmaps: <b>used, dirty</b> (free and resident), <b>zero, "
            "releasing</b>.", S_BUL),
        bul("Placement: best-fit up to 32 slices. Large runs come from per-window bitmaps of free (and resident) "
            "hugeblocks: a 4 MiB malloc + free went from 10 µs to 0.19 µs.", S_BUL),
        bul("Release modes: <b>decay</b> (dirty for ≥ 1 s), <b>excess</b> (free resident memory above a limit), "
            "<b>all</b> (malloc_trim).", S_BUL),
        bul("Limit = max(32 MiB, used/8, recent swing): a program that frees and re-allocates big buffers keeps "
            "what it is about to reuse.", S_BUL),
        bul("Release system calls run <b>outside the lock</b>: ranges are marked “releasing”, then one "
            "batched process_madvise covers up to 64 ranges.", S_BUL),
    ]
    place(c, fl, M, TOP, 452, 384, "ph-l")
    # chart: RSS after freeing ~1 GiB
    cx, cy, cwid = 540, 140, 372
    c.setFont("Lato-Bold", 13)
    c.setFillColor(INK)
    c.drawString(cx, TOP - 10, "Resident memory after freeing ≈ 1 GiB (MB)")
    c.setFont("Lato", 10.5)
    c.setFillColor(INK2)
    c.drawString(cx, TOP - 26, "allocate ≈ 1 GiB, free all of it, measure RSS; no malloc_trim")
    lab_w, bar_x = 92, cx + 92
    maxv, bar_w, rh = 1100.0, cwid - 92 - 52, 40
    y = TOP - 70
    for name, v in RSS_AFTER_FREE:
        c.setFont("Lato-Bold" if name == "sohamalloc" else "Lato", 12)
        c.setFillColor(INK)
        c.drawRightString(bar_x - 10, y - 4, name)
        bw = max(3, v / maxv * bar_w)
        c.setFillColor(SOHAM if name == "sohamalloc" else OTHER)
        c.roundRect(bar_x, y - 9, bw, 18, 3, stroke=0, fill=1)
        c.setFont("Lato-Bold" if name == "sohamalloc" else "Lato", 11.5)
        c.setFillColor(INK)
        c.drawString(bar_x + bw + 6, y - 4, f"{v:,}")
        y -= rh
    c.setStrokeColor(INK2)
    c.setLineWidth(0.8)
    c.line(bar_x, TOP - 50, bar_x, y + rh - 18)
    place(c, [P("After malloc_trim: glibc 9 MB and sohamalloc 14 MB; jemalloc, mimalloc and tcmalloc unchanged. "
                "Peak for all: 1,017–1,070 MB.", S_NOTE)], cx, y + 8, cwid, 50, "ph-note")
    place(c, [P("Prior art: TCMalloc Temeraire (OSDI 2021) and skip-subrelease; jemalloc's process_madvise "
                "batching (2025).", S_NOTE)], M, 66, CW, 24, "ph-n")


def s_signal1(c, n, N):
    frame(c, n, N, "Signal safety", "The busy byte routes a re-entrant call to a safe path", "signal safety")
    # row 1: entry, decision, normal path
    node(c, M, 384, 150, 40, "malloc / free", "new / delete", fill=white, stroke=INK2)
    dx, dy, hw, hh = 290, 404, 56, 24
    p = c.beginPath()
    p.moveTo(dx - hw, dy)
    p.lineTo(dx, dy + hh)
    p.lineTo(dx + hw, dy)
    p.lineTo(dx, dy - hh)
    p.close()
    c.setFillColor(white)
    c.setStrokeColor(INK2)
    c.setLineWidth(1.2)
    c.drawPath(p, stroke=1, fill=1)
    lab(c, dx, dy + 2, "busy byte\nset?", align="c", size=11, color=INK, font="Lato-Bold")
    arrow(c, M + 150, dy, dx - hw - 1, dy, lw=1.6)
    node(c, 420, 384, 210, 40, "normal path", "thread heap; sets busy itself", fill=ACCENT_SOFT, stroke=ACCENT_LINE)
    arrow(c, dx + hw, dy, 419, dy, lw=1.6)
    lab(c, dx + hw + 10, dy + 6, "no", size=11, font="Lato-Bold")
    # handler context
    node(c, 185, 296, 210, 52, "handler context (trymode)", "no blocking lock, ever", fill=HexColor("#fdf0e6"),
         stroke=HexColor("#f0b48f"))
    arrow(c, dx, dy - hh, dx, 349, lw=1.6)
    lab(c, dx + 8, 360, "yes", size=11, font="Lato-Bold")
    # allocation chain (left)
    ax, aw, ah = 84, 230, 42
    chain = [(236, "shared heap", "lock taken by trylock"),
             (164, "static arena", "requests ≤ 16 KiB; 384 KiB in .bss; lock-free"),
             (92, "own mapping (solo)", "solo table lock taken by trylock")]
    for y, t, d in chain:
        node(c, ax, y, aw, ah, t, d)
    poly_arrow(c, [(230, 296), (230, 280), (ax + aw / 2, 280), (ax + aw / 2, 236 + ah + 1)], lw=1.6)
    lab(c, 236, 286, "allocate", size=11, font="Lato-Bold")
    arrow(c, ax + aw / 2, 236, ax + aw / 2, 164 + ah + 1, lw=1.4)
    lab(c, ax + aw / 2 + 8, 219, "lock busy, ≤ 16 KiB", size=10.5)
    arrow(c, ax + aw / 2, 164, ax + aw / 2, 92 + ah + 1, lw=1.4)
    lab(c, ax + aw / 2 + 8, 147, "arena full", size=10.5)
    poly_arrow(c, [(ax, 236 + ah / 2), (ax - 20, 236 + ah / 2), (ax - 20, 92 + ah / 2), (ax - 1, 92 + ah / 2)], lw=1.4)
    c.saveState()
    c.translate(ax - 26, 92 + (236 - 92 + ah) / 2)
    c.rotate(90)
    c.setFont("Lato", 10.5)
    c.setFillColor(INK2)
    c.drawCentredString(0, 0, "lock busy, > 16 KiB")
    c.restoreState()
    for y, word in [(236, "lock taken"), (164, "fits"), (92, "mapped")]:
        arrow(c, ax + aw, y + ah / 2, ax + aw + 46, y + ah / 2, color=GOOD, lw=1.4)
        rbox(c, ax + aw + 48, y + ah / 2 - 11, 52, 22, fill=GOOD_SOFT, stroke=HexColor("#9fcca3"), r=11)
        lab(c, ax + aw + 74, y + ah / 2 - 3.5, "block", align="c", size=10.5, color=GOOD, font="Lato-Bold")
        lab(c, ax + aw + 6, y + ah / 2 + 5, word, size=9.5, color=MUTED)
    rbox(c, ax, 50, aw, 26, fill=BAD_SOFT, stroke=HexColor("#e9b4b0"), r=13)
    lab(c, ax + aw / 2, 59, "NULL (ENOMEM): a documented limit", align="c", size=10.5, color=BAD, font="Lato-Bold")
    arrow(c, ax + aw / 2, 92, ax + aw / 2, 77, lw=1.4)
    lab(c, ax + aw / 2 + 8, 80, "lock busy or mmap fails", size=10)
    # free lane (right)
    fx = 470
    node(c, fx, 296, W - M - fx, 44, "small block: lock-free remote free", "one CAS on the page's remote line")
    node(c, fx, 236, W - M - fx, 50, "run or solo: freed at once if its lock is free (trylock)",
         "otherwise deferred, and drained when the interrupted call returns")
    poly_arrow(c, [(395, 322), (440, 322), (440, 318), (fx - 1, 318)], lw=1.6)
    poly_arrow(c, [(440, 322), (440, 261), (fx - 1, 261)], lw=1.6)
    lab(c, 404, 330, "free", size=11, font="Lato-Bold")
    fl = [
        bul("The busy byte is a plain thread-local byte, set and cleared around every allocator operation with "
            "compiler fences: no system call, no atomic read-modify-write.", S_BUL_S),
        bul("A handler that did <i>not</i> interrupt the allocator takes the normal path.", S_BUL_S),
    ]
    place(c, fl, fx, 210, W - M - fx, 160, "sig1-b")


def s_signal2(c, n, N):
    frame(c, n, N, "Signal safety", "Why it cannot deadlock or corrupt the heap", "signal safety")
    cards = [
        ("H1", "One holder per heap", "Only the thread running a heap changes its pages' owner words, so the "
                                      "local-free check owner == my tag cannot go stale."),
        ("H2", "Busy byte around everything", "Set around every operation that touches the thread's heap or takes "
                                              "any lock: fast and slow paths, init, thread attach and exit, stats, "
                                              "collect, check."),
        ("H3", "No blocking lock in handler context", "Every lock is a trylock; work that needs a real lock is "
                                                      "skipped. Debug builds abort if H3 is ever violated."),
    ]
    cw_, gap = (CW - 2 * 18) / 3, 18
    for i, (tag, t, d) in enumerate(cards):
        x = M + i * (cw_ + gap)
        rbox(c, x, 252, cw_, 176, fill=ACCENT_SOFT, stroke=ACCENT_LINE)
        c.setFont("Lato-Black", 26)
        c.setFillColor(ACCENT)
        c.drawString(x + 16, 392, tag)
        place(c, [P(f"<b>{t}</b>", style("ct", fontSize=15.5, leading=19)), Spacer(1, 4),
                  P(d, style("cd", fontSize=13.5, leading=17.5, textColor=INK2))], x + 16, 380, cw_ - 32, 124, tag)
    fl_l = [
        P("<b>Consequence</b>", style("h", fontSize=14.5, leading=18)),
        P("A handler never waits for a lock its own thread holds, and never waits for other threads either: a "
          "taken lock makes the request fall back. Lock-free, not wait-free.", S_TXT),
    ]
    fl_r = [
        P("<b>fork() and initialisation</b>", style("h", fontSize=14.5, leading=18)),
        P("Fork handlers are registered at load; prepare finishes init and takes every lock in a fixed order "
          "(heap → shared → pool → page heap → solo → meta). Init is owned by a process id, "
          "so a child can finish an init that fork cut off. The busy byte is saved per thread across fork.", S_TXT),
    ]
    colw = (CW - 30) / 2
    place(c, fl_l, M, 230, colw, 180, "sig2-l")
    place(c, fl_r, M + colw + 30, 230, colw, 180, "sig2-r")


def s_storm_method(c, n, N):
    frame(c, n, N, "Evaluation", "Evaluation 1: signal storms (our tool, sigstorm)", "evaluation")
    # diagram
    sx, sy = M, 250
    box_text(c, sx, sy + 40, 190, 92, "Signaller thread", "SIGUSR1 to each worker in turn, every 0–40 µs",
             fill=HexColor("#fdf0e6"), stroke=HexColor("#f0b48f"))
    wx = sx + 250
    for i in range(3):
        yy = sy + 108 - i * 58
        box_text(c, wx, yy, 250, 48, f"Worker {i + 1}: malloc, write, verify, free",
                 None, fill=PANEL, tstyle=style("wt", fontName="Lato-Bold", fontSize=12, leading=15))
        arrow(c, sx + 190, sy + 86, wx - 2, yy + 24, color=STALL, lw=1.4)
    c.setFont("Lato", 11)
    c.setFillColor(INK2)
    c.drawString(wx, sy - 22, "1, 2 or 4 workers; the handler calls the allocator")
    fl = [
        P("<b>Handler kinds</b>", style("h", fontSize=15, leading=19)),
        P("malloc · free · realloc · calloc · large (300 KB) · churn (bursts) · aligned "
          "(32 B – 64 KiB)", S_TXT),
        Spacer(1, 8),
        P("<b>Controls</b>", style("h", fontSize=15, leading=19)),
        P("no signals; signals with a handler that does not allocate", S_TXT),
        Spacer(1, 8),
        P("<b>Outcome of each 5 s trial</b>", style("h", fontSize=15, leading=19)),
        P("survived · <b>stall</b> (a thread makes no progress for 2 s) · <b>corruption</b> (contents "
          "changed, blocks overlap) · <b>crash</b>", S_TXT),
    ]
    place(c, fl, 620, TOP, 292, 384, "storm-r")
    data = [["Campaign", "Allocators", "Trials"],
            ["Campaign 1 (2026-10-21)", "9, incl. glibc, jemalloc, mimalloc, tcmalloc", "6,480"],
            ["aligned kind for the other eight", "8", "720"],
            ["sohamalloc on the final code", "1", "810"]]
    place(c, [table(data, [210, 250, 70], font=11.5, align_right_from=2)], M, 196, 540, 120, "storm-t")
    place(c, [P("30 trials per cell; 95% Wilson intervals; frozen library builds with sha256; one machine "
                "(Intel Core Ultra 9 185H, 22 CPUs).", S_NOTE)], M, 86, 560, 40, "storm-n")


def s_storm_results(c, n, N):
    frame(c, n, N, "Evaluation", "Results: only sohamalloc survives the storm", "evaluation")
    lx, bx, bw = M, M + 132, 470
    ax = bx + bw + 24
    y = TOP - 34
    # legend
    lgx = bx
    for lab, col in [("stall", STALL), ("crash", CRASH), ("corruption", CORRUPT), ("survived", SURVIVED)]:
        c.setFillColor(col)
        c.roundRect(lgx, TOP - 12, 11, 11, 2, stroke=0, fill=1)
        c.setFillColor(INK2)
        c.setFont("Lato", 11.5)
        c.drawString(lgx + 16, TOP - 10.5, lab)
        lgx += 34 + stringWidth(lab, "Lato", 11.5)
    c.setFont("Lato-Bold", 11)
    c.setFillColor(INK2)
    c.drawString(ax, TOP - 10.5, "P(fail)  [95% CI]")
    rh = 33
    for name, nt, st, cr, co, p, lo, hi in STORM:
        me = name == "sohamalloc"
        if me:
            c.setFillColor(ACCENT_SOFT)
            c.rect(M - 6, y - 14, CW + 12, rh - 4, fill=1, stroke=0)
        c.setFont("Lato-Bold" if me else "Lato", 12.5)
        c.setFillColor(INK)
        c.drawRightString(bx - 10, y - 4, name)
        x = bx
        for v, col in [(st, STALL), (cr, CRASH), (co, CORRUPT), (nt - st - cr - co, SURVIVED)]:
            if v == 0:
                continue
            w_ = v / nt * bw
            c.setFillColor(col)
            c.rect(x, y - 9, max(w_ - 2, 1), 18, fill=1, stroke=0)
            x += w_
        c.setFont("Lato-Bold" if me else "Lato", 12)
        c.setFillColor(INK)
        c.drawString(ax, y - 4, f"{p:.3f}  [{lo:.3f}, {hi:.3f}]")
        c.setFont("Lato", 10.5)
        c.setFillColor(MUTED)
        c.drawRightString(W - M, y - 4, f"{nt - st - cr - co}/{nt} survived")
        y -= rh
    place(c, [P("Others: campaign 1 (540 signal trials each). sohamalloc: final code d2add24, 630 signal trials, "
                "140 million handler calls, 2.7 million of them inside an interrupted allocator call. Controls: 1,800 "
                "trials, 0 failures. With the aligned kind, the others failed 39–90 of 90 trials.", S_NOTE)],
          M, y + 6, CW, 50, "storm-note")


def dumbbell(c, x, y_top, w, rows, xmin=1.0, xmax=2.0):
    lab_w = 112
    px0, px1 = x + lab_w, x + w - 10

    def X(v):
        return px0 + (min(v, xmax) - xmin) / (xmax - xmin) * (px1 - px0)

    rh = 21
    y = y_top
    c.setStrokeColor(RULE)
    c.setLineWidth(0.6)
    nrows = len(rows)
    y_bottom = y_top - nrows * rh + 6
    for v in (1.0, 1.25, 1.5, 1.75, 2.0):
        c.line(X(v), y_top + 10, X(v), y_bottom)
        c.setFont("Lato", 10)
        c.setFillColor(MUTED)
        c.drawCentredString(X(v), y_bottom - 12, f"{v:.2f}")
    for r in rows:
        if r is None:
            c.setFont("Lato-Italic", 10.5)
            c.setFillColor(MUTED)
            c.drawString(x, y - 4, "got worse:")
            y -= rh
            continue
        name, a, b = r
        c.setFont("Lato", 11.5)
        c.setFillColor(INK)
        c.drawRightString(px0 - 8, y - 4, name)
        c.setStrokeColor(OTHER)
        c.setLineWidth(2)
        c.line(X(a), y, X(b), y)
        c.setFillColor(OTHER)
        c.circle(X(a), y, 4.5, stroke=0, fill=1)
        c.setFillColor(SOHAM)
        c.circle(X(b), y, 4.5, stroke=0, fill=1)
        if a > xmax:
            c.setFont("Lato", 10)
            c.setFillColor(INK2)
            c.drawString(X(a) - 30, y + 6, f"{a:.2f} →")
        y -= rh
    return y_bottom


def s_bench(c, n, N):
    frame(c, n, N, "Evaluation", "Evaluation 2: speed and memory on 24 benchmarks", "evaluation")
    data = [["Allocator", "Time", "RSS", "Score"],
            ["sohamalloc", "1.124", "<b>1.162</b>", "<b>1.142</b>"],
            ["mimalloc 2.1.2", "<b>1.063</b>", "1.227", "<b>1.142</b>"],
            ["glibc 2.39", "1.491", "1.092", "1.276"],
            ["jemalloc 5.3.0", "1.333", "1.355", "1.344"],
            ["tcmalloc 2.15", "1.370", "1.537", "1.451"]]
    t = table(data, [150, 76, 76, 76], hl_rows=(1,), font=12.5, align_right_from=1)
    fl = [t, Spacer(1, 10),
          P("Geometric means over 24 workloads (mimalloc-bench subset plus python, gcc, sqlite and redis) of time and "
            "peak RSS, each normalised to the best allocator per workload; score = geometric mean of the two. Lowest "
            "RSS among the two leaders; mimalloc is faster.", S_SMALL),
          Spacer(1, 10),
          P("Caveats: 4-vCPU cloud VM, 3 repetitions, distribution builds of the other allocators, code of "
            "2026-10-19. The tuned campaign is gate 4.", S_NOTE)]
    place(c, fl, M, TOP, 400, 384, "bench-l")
    cx = M + 440
    c.setFont("Lato-Bold", 13)
    c.setFillColor(INK)
    c.drawString(cx, TOP - 10, "sohamalloc vs the best allocator, B1 → B4")
    c.setFont("Lato", 10.5)
    c.setFillColor(INK2)
    c.drawString(cx, TOP - 25, "factor (lower is better); score 1.241 → 1.142")
    c.setFillColor(OTHER)
    c.circle(cx + 6, TOP - 42, 4.5, stroke=0, fill=1)
    c.setFillColor(INK2)
    c.drawString(cx + 15, TOP - 46, "B1 (first campaign)")
    c.setFillColor(SOHAM)
    c.circle(cx + 136, TOP - 42, 4.5, stroke=0, fill=1)
    c.setFillColor(INK2)
    c.drawString(cx + 145, TOP - 46, "B4 (after the tuning rounds)")
    dumbbell(c, cx, TOP - 72, W - M - cx, B1B4)


def s_others_better(c, n, N):
    frame(c, n, N, "Honest comparison", "Where other allocators are still better, and why", "comparison")
    rows = [
        ["Where", "Better", "Why"],
        ["Raw fast-path speed", "mimalloc", "~18 / 29 instructions for malloc / free against our ~45 / 79: we pay for "
                                            "link checks, double-free detection, the foreign-pointer test, the busy byte"],
        ["Producer / consumer (larson)", "mimalloc, 1.2×", "remote frees, notification and chain validation cost "
                                                               "more; open item"],
        ["Random-order frees", "mimalloc", "alloc-test1 1.21×, xmalloc-testN 1.23×: the cap/8 threshold "
                                           "trades refills for idle blocks"],
        ["barnes, cache-thrash/scratch", "glibc, mimalloc", "1.20–1.32×; runs of about 0.5 s near timer "
                                                             "resolution, spread up to 117%: not yet explained"],
        ["Tiny processes' memory", "glibc", "+1.6–3.2 MiB RSS from windows, records and the granule map"],
        ["Tight RLIMIT_AS", "glibc", "at 128 MiB: 60 MiB usable against 100 MiB (reserved windows count)"],
        ["Hardening depth", "snmalloc, hardened_malloc", "signed back-pointers, stronger isolation; our keys come "
                                                         "from one stream per heap"],
        ["Progress guarantee", "Michael (2004), LRMalloc", "fully lock-free; our handler path is lock-free but may "
                                                           "return NULL when locks are held"],
    ]
    t = table(rows, [196, 176, 492], font=13, pad=7)
    place(c, [t], M, TOP, CW, 384, "others")


def s_verify(c, n, N):
    frame(c, n, N, "Verification", "How we know: tests, sanitizers, platforms, reviews", "verification")
    cells = [("native", "26/26"), ("debug assertions", "26/26"), ("prefixed API", "11/11"),
             ("ThreadSanitizer", "9/9"), ("AddressSanitizer", "9/9"), ("UBSan", "9/9"),
             ("real programs (preload)", "9/9"), ("aarch64 (qemu)", "26/26"), ("armhf 32-bit (qemu)", "26/26"),
             ("musl", "25/25"), ("Windows (mingw, wine)", "9/9"), ("sigstorm, debug build", "66/66")]
    gx, gw, gh, cols = M, 136, 54, 3
    for i, (name, res) in enumerate(cells):
        col, row = i % cols, i // cols
        x = gx + col * (gw + 8)
        y = TOP - (row + 1) * (gh + 8) + 8
        rbox(c, x, y, gw, gh, fill=GOOD_SOFT, stroke=HexColor("#b9dcbc"), r=6)
        c.setFont("Lato-Bold", 17)
        c.setFillColor(GOOD)
        c.drawString(x + 10, y + 28, res)
        c.setFont("Lato", 10.5)
        c.setFillColor(INK2)
        c.drawString(x + 10, y + 11, name)
    place(c, [P("Latest full matrix, 2026-10-23, on the round-4 fixes. Skipped by design: 2 sanitizer cases, 1 C++ "
                "case on musl, 2 on Windows. Real programs: python3, gcc, g++, git, perl, sort, find, tar, bash.",
                S_NOTE)], M, 160, 3 * gw + 16, 80, "ver-n")
    rx = M + 3 * gw + 16 + 28
    data = [["Review round", "Confirmed", "Status"],
            ["Round 2 (10-21)", "2 critical, 1 major, 2 minor", "fixed"],
            ["Round 3 (10-21)", "1 critical, 3 major, 2 minor", "fixed; minors documented"],
            ["Storm campaign 1", "1 major (C1-1)", "fixed"],
            ["Round 4 (10-22/23)", "2 critical, 4 major, 10 minor", "fixed or documented"],
            ["Round 5", "—", "not yet completed"]]
    t = table(data, [112, 168, 132], font=11)
    fl = [t, Spacer(1, 12),
          P("<b>Rule:</b> every defect gets a regression test that fails on the old code and passes on the fix, run "
            "before any allocator commit.", S_SMALL),
          Spacer(1, 6),
          P("Reviews: independent reviewer agents with separate lenses (signals, concurrency, lifecycle, API, "
            "portability), plus refuters that try to disprove each finding; a finding counts once reproduced.",
            S_SMALL)]
    place(c, fl, rx, TOP, W - M - rx, 384, "ver-r")


def s_novelty(c, n, N):
    frame(c, n, N, "Related work", "What we build on, and what is new", "related work")
    colw = (CW - 30) / 2
    l = [
        P("<b>Built on published ideas</b>", style("h", fontSize=16, leading=20, textColor=INK2)),
        Spacer(1, 6),
        bul("Free-list sharding, one-CAS remote frees, abandoned-page reclaim: mimalloc (APLAS 2019; v2, v3), Hoard "
            "(ASPLOS 2000), rpmalloc", S_BUL),
        bul("Address-to-metadata byte map: snmalloc (ISMM 2019)", S_BUL),
        bul("Hugepage-aware page heap, demand-based release: TCMalloc Temeraire (OSDI 2021)", S_BUL),
        bul("Link encoding: glibc Safe-Linking (2020), PartitionAlloc, mimalloc secure mode", S_BUL),
        bul("Batched process_madvise: jemalloc (2025)", S_BUL),
    ]
    r = [
        P("<b>What is new</b>", style("h", fontSize=16, leading=20, textColor=ACCENT)),
        Spacer(1, 6),
        bul("A <b>documented async-signal-safety guarantee for the whole malloc API</b> in a user-space, "
            "thread-caching, general-purpose allocator, with no system call on the fast path.", S_BUL),
        bul("Together with default-on hardening and prompt memory return, at mimalloc-class speed.", S_BUL),
        bul("Closest prior work: lock-free allocators (Michael, PLDI 2004; LRMalloc 2018); Linux kernel any-context "
            "allocators (bpf_mem_alloc; kmalloc_nolock, 2025); special-purpose pools (AO_malloc, folly, Abseil "
            "LowLevelAlloc).", S_BUL),
        Spacer(1, 4),
        P("Framing: an integration and experience paper with a strong evaluation, not one new mechanism.", S_NOTE),
    ]
    rbox(c, M + colw + 14, BOT + 20, colw + 16, TOP - BOT - 12, fill=ACCENT_SOFT, stroke=ACCENT_LINE)
    place(c, l, M, TOP - 8, colw - 10, 370, "nov-l")
    place(c, r, M + colw + 30, TOP - 8, colw - 16, 370, "nov-r")


def s_limits(c, n, N):
    frame(c, n, N, "Honest limits", "Limits of the guarantee: stated, not hidden", "limits")
    items = [
        ("A handler's allocation can return NULL while memory is available",
         "when the locks it needs are held and both fallbacks fail. Measured on the final code: 31 of 19.9 million "
         "large-block handler calls, 2 of 20.1 million churn calls, 0 for the other kinds."),
        ("Frees of large blocks in a handler may be deferred", "until the interrupted call returns."),
        ("The handler must return normally", "no siglongjmp or exception out of an interrupted allocator call."),
        ("fork() from such a handler is not covered", "POSIX.1-2024 no longer lists fork as async-signal-safe."),
        ("The very first allocator call made from a handler", "uses pthread and mmap functions that are not on "
                                                               "POSIX's list (safe on glibc and musl in practice)."),
        ("Evidence, not proof", "regression tests, the debug check of H3, adversarial reviews and storms. Storms "
                                "alone missed a real deadlock (R2-1): model checking is future work."),
    ]
    colw = (CW - 24) / 2
    for i, (t, d) in enumerate(items):
        col, row = i % 2, i // 2
        x = M + col * (colw + 24)
        y = TOP - row * 124
        c.setFillColor(ACCENT)
        c.circle(x + 11, y - 11, 11, stroke=0, fill=1)
        c.setFillColor(white)
        c.setFont("Lato-Bold", 12)
        c.drawCentredString(x + 11, y - 15.5, str(i + 1))
        place(c, [P(f"<b>{t}</b>", style("lt", fontSize=15, leading=19)), Spacer(1, 4),
                  P(d, style("ld", fontSize=13.5, leading=17.5, textColor=INK2))], x + 32, y, colw - 32, 116, f"lim{i}")


def s_status(c, n, N):
    frame(c, n, N, "Status", "Status: the four gates before a paper", "status")
    gates = [
        (True, "1. Every known bug fixed, each with a fail-before / pass-after test",
         "Met. Every confirmed critical and major defect has its test; the full matrix passes on d2add24."),
        (False, "2. A review round with no critical or major finding",
         "Open. Round 5 did not complete (usage limits); it must be relaunched."),
        (True, "3. Signal-storm campaign: ≥ 30 trials per cell, CIs, controls, limits",
         "Met. 0 of 630 signal trials failed on the final code (95% CI 0–0.6%); limits documented."),
        (False, "4. Tuned benchmark campaign: ≥ 10 repetitions with CIs",
         "Open. Not started: it needs a quiet machine tuned for benchmarking."),
    ]
    y = TOP
    for ok, t, d in gates:
        rbox(c, M, y - 62, 560, 62, fill=GOOD_SOFT if ok else WARN_SOFT,
             stroke=HexColor("#b9dcbc") if ok else HexColor("#ecc98f"), r=6)
        c.setFont("DejaVu-Bold", 17)
        c.setFillColor(GOOD if ok else HexColor("#9a5b00"))
        c.drawString(M + 12, y - 38, "✓" if ok else "○")
        place(c, [P(f"<b>{t}</b>", style("gt", fontSize=13.5, leading=17)),
                  P(d, style("gd", fontSize=12, leading=15, textColor=INK2))],
              M + 38, y - 8, 514, 58, t)
        y -= 74
    fl = [
        P("<b>Next steps</b>", style("h", fontSize=15, leading=19)),
        Spacer(1, 6),
        bul("Review round 5; fix and repeat until a round is clean", S_BUL),
        bul("Measure the busy byte's cost (ablation, with CIs)", S_BUL),
        bul("Tuned benchmarks on the final code, with layout randomisation", S_BUL),
        bul("Close the larson gap; return idle threads' memory; keyed per-page keys", S_BUL),
        bul("Model-check the lock-free page protocols", S_BUL),
        bul("Then write the paper", S_BUL),
    ]
    place(c, fl, M + 590, TOP, CW - 590, 384, "status-r")


def s_method(c, n, N):
    frame(c, n, N, "Method", "How the work was done", "method")
    fl = [
        bul("<b>Directed by me:</b> the goals, the thesis, priorities and the four acceptance gates. Key choices "
            "were taken as explicit decisions and logged."),
        bul("<b>AI-assisted tooling did most of the hands-on work:</b> literature research, competing designs and "
            "their critique (a 101-fault critique, a red team), implementation, tests, benchmarks, and adversarial "
            "code reviews run as multi-agent workflows."),
        bul("<b>Trust comes from evidence, not from the tool:</b> every claim is backed by tests, raw data and "
            "before/after records in the repository; a review finding counts only once a reproducer confirms it."),
        bul("<b>Research log:</b> a timeline of every phase, a defect ledger with root causes and fixes, and a "
            "novelty analysis against 50 references."),
    ]
    place(c, fl, M, TOP, 760, 384, "method")


def s_discussion(c, n, N):
    frame(c, n, N, "Discussion", "Where I would value your advice", "discussion")
    qs = [
        ("Venue and framing", "An integration and experience paper (ISMM, or a systems venue)? What would make the "
                              "signal-safety contribution stand out?"),
        ("Evaluation", "Which real-world workloads would convince reviewers: crash reporters, profilers, logging "
                       "libraries, language runtimes?"),
        ("Verification", "Is model checking the lock-free page protocols (TLA+, GenMC) worth the time before "
                         "submission?"),
        ("Cost of safety", "How best to isolate the busy byte's cost: ablation builds, hardware counters, "
                           "layout-randomised runs?"),
    ]
    colw = (CW - 24) / 2
    for i, (t, d) in enumerate(qs):
        col, row = i % 2, i // 2
        x = M + col * (colw + 24)
        y = TOP - row * 190
        rbox(c, x, y - 172, colw, 172, fill=PANEL, stroke=RULE)
        place(c, [P(f"<b>{t}</b>", style("qt", fontSize=17, leading=21, textColor=ACCENT)), Spacer(1, 8),
                  P(d, style("qd", fontSize=15, leading=20, textColor=INK))], x + 20, y - 20, colw - 40, 146, t)


SLIDES = [s_title, s_problem, s_thesis, s_arch, s_address, s_records, s_classes, s_malloc, s_free, s_hardening,
          s_remote, s_orphan, s_pageheap, s_signal1, s_signal2, s_storm_method, s_storm_results, s_bench,
          s_others_better, s_verify, s_novelty, s_limits, s_status, s_method, s_discussion]


def build():
    c = canvas.Canvas(OUT, pagesize=(W, H), pageCompression=1)
    c.setTitle("sohamalloc: how it works")
    c.setSubject("Architecture, signal-safety mechanism and evaluation of the sohamalloc memory allocator")
    c.setAuthor("")
    c.setCreator("")
    N = len(SLIDES)
    for i, fn in enumerate(SLIDES, 1):
        fn(c, i, N)
        c.showPage()
    c.save()
    print(OUT)


if __name__ == "__main__":
    build()

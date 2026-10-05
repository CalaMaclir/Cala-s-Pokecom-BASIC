#!/usr/bin/env python3
"""Generate versioned Japanese CPB manuals from Markdown without network access."""

from __future__ import annotations

import argparse
import html
import os
import json
import hashlib
import reportlab
import importlib.util
import re
from contextvars import ContextVar
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus.tableofcontents import TableOfContents
from reportlab.pdfgen.canvas import Canvas
from reportlab.platypus import (
    Flowable, HRFlowable, Image, KeepTogether, PageBreak, Paragraph, Preformatted,
    SimpleDocTemplate, Spacer, Table, TableStyle,
)

ROOT = Path(__file__).resolve().parents[1]
PAGE_W, PAGE_H = A4
MARGIN_X = 19 * mm
BODY_W = PAGE_W - 2 * MARGIN_X
MANUAL_NAMES = (
    ("Install-Manual", "Install Manual / 導入マニュアル", "install-manual-ja.md"),
    ("System-Manual", "System Manual / システムマニュアル", "system-manual-ja.md"),
    ("Programming-Reference", "Programming Reference Manual / プログラミング・リファレンスマニュアル", "programming-reference-ja.md"),
)
FONT_NAME = "CpbIPAexGothic"
FONT_SHA256 = ""
SOURCE_PATH = ContextVar("manual_source_path", default=None)


def register_fonts(font_path):
    global FONT_SHA256
    if not font_path.is_file():
        spec = importlib.util.find_spec("japanize_matplotlib")
        if spec is None:
            raise SystemExit("Install tools/requirements-manuals.txt: embedded Japanese font is required")
        font_path = Path(spec.origin).parent / "fonts/ipaexg.ttf"
    if not font_path.is_file():
        raise SystemExit("Japanese TrueType font missing")
    FONT_SHA256 = hashlib.sha256(font_path.read_bytes()).hexdigest()
    pdfmetrics.registerFont(TTFont(FONT_NAME, str(font_path)))


def make_styles():
    base = getSampleStyleSheet()
    gothic, serif = FONT_NAME, FONT_NAME
    return {
        "body": ParagraphStyle("body", parent=base["BodyText"], fontName=serif,
            wordWrap="CJK", splitLongWords=True, fontSize=9.2, leading=15, spaceAfter=4.5, textColor=colors.HexColor("#20242A")),
        "title": ParagraphStyle("title", parent=base["Title"], fontName=gothic,
            fontSize=26, leading=35, alignment=TA_CENTER, textColor=colors.HexColor("#153F61")),
        "subtitle": ParagraphStyle("subtitle", parent=base["Heading2"], fontName=gothic,
            fontSize=15, leading=22, alignment=TA_CENTER, textColor=colors.HexColor("#1C6A8D")),
        "h1": ParagraphStyle("h1", parent=base["Heading1"], fontName=gothic,
            wordWrap="CJK", fontSize=17, leading=23, spaceBefore=14, spaceAfter=8, textColor=colors.HexColor("#153F61")),
        "h2": ParagraphStyle("h2", parent=base["Heading2"], fontName=gothic,
            wordWrap="CJK", keepWithNext=True, fontSize=12.5, leading=18, spaceBefore=12, spaceAfter=5, textColor=colors.HexColor("#1C6A8D")),
        "h3": ParagraphStyle("h3", parent=base["Heading3"], fontName=gothic,
            wordWrap="CJK", keepWithNext=True, fontSize=10.5, leading=15, spaceBefore=9, spaceAfter=4, textColor=colors.HexColor("#273C52")),
        "bullet": ParagraphStyle("bullet", parent=base["BodyText"], fontName=serif,
            wordWrap="CJK", splitLongWords=True, fontSize=9.2, leading=14, leftIndent=13, firstLineIndent=-9, spaceAfter=2),
        "number": ParagraphStyle("number", parent=base["BodyText"], fontName=serif,
            wordWrap="CJK", splitLongWords=True, fontSize=9.2, leading=14, leftIndent=16, firstLineIndent=-13, spaceAfter=2),
        "code": ParagraphStyle("code", parent=base["Code"], fontName="Courier",
            fontSize=8.5, leading=12, leftIndent=7, rightIndent=7, textColor=colors.HexColor("#1D2A36")),
        "table": ParagraphStyle("table", parent=base["BodyText"], fontName=serif,
            fontSize=7.5, leading=10.2, wordWrap="CJK"),
        "table_head": ParagraphStyle("table_head", parent=base["BodyText"], fontName=gothic,
            fontSize=7.5, leading=10.2, textColor=colors.white, wordWrap="CJK"),
        "cover_meta": ParagraphStyle("cover_meta", parent=base["BodyText"], fontName=serif,
            fontSize=10, leading=17, alignment=TA_CENTER, textColor=colors.HexColor("#4A5966")),
        "caption": ParagraphStyle("caption", parent=base["BodyText"], fontName=serif,
            fontSize=7.5, leading=11, alignment=TA_CENTER, textColor=colors.HexColor("#4A5966")),
        "compat_mode": ParagraphStyle("compat_mode", parent=base["BodyText"], fontName=gothic,
            fontSize=8.2, leading=11, textColor=colors.HexColor("#273C52")),
        "compat_value": ParagraphStyle("compat_value", parent=base["BodyText"], fontName=gothic,
            fontSize=8.2, leading=11, textColor=colors.HexColor("#153F61")),
    }


def inline(text):
    value = html.escape(text, quote=False)
    def link(match):
        label,target=match.groups()
        if not target.startswith(("http://","https://")):
            source = SOURCE_PATH.get()
            if source is not None:
                path, separator, fragment = target.partition("#")
                if path:
                    target = str((source.parent / path).resolve().relative_to(ROOT))
                    if separator:
                        target += "#" + fragment
                else:
                    target = str(source.relative_to(ROOT)) + target
            target="https://github.com/CalaMaclir/Cala-s-Pokecom-BASIC/blob/main/"+target
        return '<link href="'+html.escape(target,quote=True)+'" color="#1C6A8D">'+label+'</link>'
    value=re.sub(r"\[([^]]+)\]\(([^)]+)\)",link,value)
    value = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", value)
    delimiter = chr(96)
    value = re.sub(
        delimiter + "([^" + delimiter + "]+)" + delimiter,
        r'<font color="#1C6A8D">\1</font>',
        value,
    )
    return value.replace("  ", "&nbsp; ")


def paragraph(text, style):
    return Paragraph(inline(text.strip()), style)


def markdown_table(lines, st):
    rows = []
    for index, line in enumerate(lines):
        fields = [field.strip() for field in line.strip().strip("|").split("|")]
        if index == 1 and all(re.fullmatch(r":?-{3,}:?", field) for field in fields):
            continue
        style = st["table_head"] if not rows else st["table"]
        rows.append([paragraph(field, style) for field in fields])
    columns = max(len(row) for row in rows)
    table = Table(rows, colWidths=[BODY_W / columns] * columns, repeatRows=1, hAlign="LEFT")
    table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1C6A8D")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("GRID", (0, 0), (-1, -1), 0.25, colors.HexColor("#B6C8D5")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#F1F6F9")]),
    ]))
    return table


def compatibility_box(classic, structured, st):
    table = Table(
        [
            [Paragraph("Classic", st["compat_mode"]), Paragraph(classic, st["compat_value"])],
            [Paragraph("Structured", st["compat_mode"]), Paragraph(structured, st["compat_value"])],
        ],
        colWidths=[37 * mm, 48 * mm],
        hAlign="LEFT",
    )
    table.setStyle(TableStyle([
        ("BOX", (0, 0), (-1, -1), 0.8, colors.HexColor("#D87819")),
        ("INNERGRID", (0, 0), (-1, -1), 0.35, colors.HexColor("#ECC49A")),
        ("BACKGROUND", (0, 0), (0, -1), colors.HexColor("#FFF1E2")),
        ("BACKGROUND", (1, 0), (1, -1), colors.white),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 6),
        ("RIGHTPADDING", (0, 0), (-1, -1), 6),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]))
    table.keepWithNext = True
    return table



COMPATIBILITY_MARKER = re.compile(
    r"> \*\*対応モード:\*\* Classic=(対応|非対応|共通|利用可) / "
    r"Structured=(対応|非対応|共通|利用可)"
)


class ReferenceHeading(Flowable):
    """One indivisible title with two independently framed inline mode badges."""
    keepWithNext = True
    spaceBefore = 9
    spaceAfter = 4
    _toc_level = 1

    def __init__(self, title, classic, structured):
        super().__init__()
        self.title = title
        self.badges = (f"Classic {classic}", f"Structured {structured}")
        self.badge_size = 8.2
        self.height = 19

    def getPlainText(self):
        return self.title

    def wrap(self, availWidth, availHeight):
        self.width = min(BODY_W, availWidth)
        self.badge_widths = [pdfmetrics.stringWidth(s, FONT_NAME, self.badge_size) + 12
                             for s in self.badges]
        available = self.width - sum(self.badge_widths) - 18
        nominal = pdfmetrics.stringWidth(self.title, FONT_NAME, 10.5)
        self.title_size = min(10.5, 10.5 * available / max(nominal, 1))
        if self.title_size < 7:
            raise ValueError(f"Reference heading cannot fit one line: {self.title}")
        return self.width, self.height

    def draw(self):
        canvas = self.canv
        canvas.saveState()
        baseline = 5
        canvas.setFillColor(colors.HexColor("#273C52"))
        canvas.setFont(FONT_NAME, self.title_size)
        canvas.drawString(0, baseline, self.title)
        x = pdfmetrics.stringWidth(self.title, FONT_NAME, self.title_size) + 10
        for label, width in zip(self.badges, self.badge_widths):
            canvas.setStrokeColor(colors.HexColor("#D87819"))
            canvas.setFillColor(colors.HexColor("#FFF1E2"))
            canvas.setLineWidth(0.8)
            canvas.roundRect(x, 1, width, 16, 2, stroke=1, fill=1)
            canvas.setFillColor(colors.HexColor("#273C52"))
            canvas.setFont(FONT_NAME, self.badge_size)
            canvas.drawString(x + 6, baseline, label)
            x += width + 8
        canvas.restoreState()


def flush(buffer, story, style):
    if buffer:
        story.append(paragraph(" ".join(line.strip() for line in buffer), style))
        buffer.clear()


def markdown_story(source, st, source_path, *, legacy=False):
    story, buffer = [], []
    lines, index = source.splitlines(), 0
    # First three headings are rendered on the cover, not again in the body.
    lines=[line for i,line in enumerate(lines) if not (i<3 and line.startswith("#"))]
    code_fence = chr(96) * 3
    while index < len(lines):
        line = lines[index]
        if line.startswith(code_fence):
            flush(buffer, story, st["body"])
            block, index = [], index + 1
            while index < len(lines) and not lines[index].startswith(code_fence):
                block.append(lines[index])
                index += 1
            story += [KeepTogether([Spacer(1, 2), Preformatted("\n".join(block), st["code"], maxLineLength=90), Spacer(1, 4)])]
        elif line.startswith("|"):
            flush(buffer, story, st["body"])
            table_lines = [line]
            index += 1
            while index < len(lines) and lines[index].startswith("|"):
                table_lines.append(lines[index])
                index += 1
            story += [markdown_table(table_lines, st), Spacer(1, 7)]
            continue
        elif re.fullmatch(
            r"> \*\*対応モード:\*\* Classic=(対応|非対応|共通|利用可) / Structured=(対応|非対応|共通|利用可)",
            line.strip(),
        ):
            flush(buffer, story, st["body"])
            match = re.fullmatch(
                r"> \*\*対応モード:\*\* Classic=(対応|非対応|共通|利用可) / Structured=(対応|非対応|共通|利用可)",
                line.strip(),
            )
            gap = Spacer(1, 5)
            gap.keepWithNext = True
            story += [compatibility_box(match.group(1), match.group(2), st), gap]
        elif re.match(r"^!\[(.*?)\]\((.*?)\)$", line.strip()):
            flush(buffer, story, st["body"])
            match = re.match(r"^!\[(.*?)\]\((.*?)\)$", line.strip())
            caption, relative = match.groups()
            image_path = (source_path.parent / relative).resolve()
            if not image_path.is_file():
                raise SystemExit(f"Missing image: {image_path}")
            width = min(82 * mm, BODY_W)
            figure = Image(str(image_path), width=width, height=width)
            figure.hAlign = "CENTER"
            story += [Spacer(1, 4), figure, Paragraph(inline(caption), st["caption"]), Spacer(1, 7)]
        elif line.startswith("### "):
            flush(buffer, story, st["body"])
            marker_index = index + 1
            while marker_index < len(lines) and not lines[marker_index].strip():
                marker_index += 1
            marker = (COMPATIBILITY_MARKER.fullmatch(lines[marker_index].strip())
                      if marker_index < len(lines) else None)
            if marker and not legacy:
                heading = ReferenceHeading(line[4:], marker.group(1), marker.group(2))
                index = marker_index
            else:
                heading=paragraph(line[4:],st["h3"])
                heading._toc_level=1
            story.append(heading)
        elif line.startswith("## "):
            flush(buffer, story, st["body"])
            heading=paragraph(line[3:],st["h2"])
            heading._toc_level=0
            story.append(heading)
        elif line.startswith("# "):
            flush(buffer, story, st["body"])
        elif line.strip() == "---":
            flush(buffer, story, st["body"])
            story += [Spacer(1, 3), HRFlowable(width="100%", thickness=0.6,
                color=colors.HexColor("#7FA5BA"), spaceAfter=5)]
        elif re.match(r"^[-*] ", line):
            flush(buffer, story, st["body"])
            story.append(Paragraph("• " + inline(line[2:]), st["bullet"]))
        elif re.match(r"^\d+\. ", line):
            flush(buffer, story, st["body"])
            story.append(paragraph(line, st["number"]))
        elif not line.strip():
            flush(buffer, story, st["body"])
        else:
            buffer.append(line)
        index += 1
    flush(buffer, story, st["body"])
    return story


def cover(title, version, st):
    return [
        Spacer(1, 48 * mm),
        Paragraph("Cala's Pokecom BASIC System", st["title"]),
        Spacer(1, 9 * mm),
        Paragraph(f"Version {version}", st["subtitle"]),
        Spacer(1, 18 * mm),
        HRFlowable(width="66%", thickness=1.5, color=colors.HexColor("#1C6A8D"), hAlign="CENTER"),
        Spacer(1, 12 * mm),
        Paragraph(title, st["subtitle"]),
        Spacer(1, 29 * mm),
        Paragraph("for ClockworkPi PicoCalc<br/>with Raspberry Pi Pico 2 W<br/><br/>Copyright (C) 2026 Cala Maclir", st["cover_meta"]),
        PageBreak(),
    ]


def page_decor(canvas, doc):
    canvas.saveState()
    canvas.setStrokeColor(colors.HexColor("#9AB4C3"))
    canvas.setLineWidth(0.45)
    canvas.line(MARGIN_X, PAGE_H - 12 * mm, PAGE_W - MARGIN_X, PAGE_H - 12 * mm)
    canvas.setFont(FONT_NAME, 7)
    canvas.setFillColor(colors.HexColor("#496779"))
    canvas.drawString(MARGIN_X, PAGE_H - 9 * mm, f"Cala's Pokecom BASIC System  Version {doc.cpb_version}")
    canvas.setFont(FONT_NAME, 7)
    canvas.drawRightString(PAGE_W - MARGIN_X, 10 * mm, f"CPB v{doc.cpb_version}  |  {canvas.getPageNumber()}")
    canvas.restoreState()


class ManualDoc(SimpleDocTemplate):
    def beforeDocument(self):
        self._cpb_heading_index = 0

    def afterFlowable(self,flowable):
        if hasattr(flowable,"_toc_level"):
            title=flowable.getPlainText()
            key="h-"+hashlib.sha256(title.encode()).hexdigest()[:20]
            if self.cpb_version != "0.92":
                # Overview and detailed reference entries can share a title.
                # Each link must resolve to its own heading on every build pass.
                key += "-" + str(self._cpb_heading_index)
                self._cpb_heading_index += 1
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(title,key,level=flowable._toc_level,closed=False)
            self.notify("TOCEntry",(flowable._toc_level,title,self.page,key))

def fixed_canvas(*args,**kwargs):
    kwargs["invariant"]=1
    return Canvas(*args,**kwargs)

def build(source, output, title, version):
    st = make_styles()
    doc = ManualDoc(
        str(output), pagesize=A4, leftMargin=MARGIN_X, rightMargin=MARGIN_X,
        topMargin=20 * mm, bottomMargin=18 * mm,
        title=f"Cala's Pokecom BASIC System Version {version} - {title}",
        author="Cala Maclir", subject=f"Cala's Pokecom BASIC System Version {version}",
    )
    doc.cpb_version = version
    toc=TableOfContents()
    toc.levelStyles=[
        ParagraphStyle("toc0",parent=st["body"],fontSize=9.2,leading=15,spaceBefore=4),
        ParagraphStyle("toc1",parent=st["body"],fontSize=8,leading=12,leftIndent=12)]
    story=cover(title,version,st)+[paragraph("目次",st["h1"]),Spacer(1,8),toc,PageBreak()]
    token = SOURCE_PATH.set(source if version != "0.92" else None)
    try:
        story+=markdown_story(source.read_text(encoding="utf-8"),st,source, legacy=version == "0.92")
        doc.multiBuild(story,onFirstPage=lambda canvas,document:None,onLaterPages=page_decor,canvasmaker=fixed_canvas)
    finally:
        SOURCE_PATH.reset(token)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check input paths without producing PDFs.")
    parser.add_argument("--version", choices=("0.92", "0.94"), default="0.94",
                        help="Manual version to generate (default: 0.94).")
    parser.add_argument("--output-dir", default="",
                        help="Optional output directory for reproducibility checks; no manifest is written.")
    parser.add_argument(
        "--font",
        default=os.environ.get("CPB_JAPANESE_FONT", ""),
        help="Optional Japanese TrueType font. Default: IPAex Gothic from pinned japanize-matplotlib package.",
    )
    args = parser.parse_args()
    register_fonts(Path(args.font).expanduser())
    source_dir = ROOT / "docs" if args.version == "0.94" else ROOT / "docs/archive/v0.92"
    output_dir = Path(args.output_dir).resolve() if args.output_dir else ROOT / "docs"
    manuals = tuple(
        (
            source_dir / source_name,
            output_dir / f"Cala-Pokecom-BASIC-v{args.version}-{slug}-ja.pdf",
            title,
        )
        for slug, title, source_name in MANUAL_NAMES
    )
    manifest=[]
    for source, output, title in manuals:
        if not source.exists():
            raise SystemExit(f"Missing manual source: {source}")
        if args.check:
            print(f"OK: {source.relative_to(ROOT)} -> {output.relative_to(ROOT)}")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            build(source, output, title, args.version)
            shown = output.relative_to(ROOT) if output.is_relative_to(ROOT) else output
            print(f"Wrote {shown}")
            manifest.append({"source":str(source.relative_to(ROOT)),"output":str(output.relative_to(ROOT)),
                "source_sha256":hashlib.sha256(source.read_bytes()).hexdigest(),
                "output_sha256":hashlib.sha256(output.read_bytes()).hexdigest()})
    if not args.check and not args.output_dir:
        (ROOT/"docs/manuals-manifest.json").write_text(json.dumps({
            "version":args.version,"generator":"tools/generate_manual_pdfs.py",
            "generator_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "reportlab":reportlab.Version,"font":FONT_NAME,"font_sha256":FONT_SHA256,"font_package":"japanize-matplotlib==1.1.3","canvas_invariant":True,"manuals":manifest},
            ensure_ascii=False,indent=2)+"\n")


if __name__ == "__main__":
    main()

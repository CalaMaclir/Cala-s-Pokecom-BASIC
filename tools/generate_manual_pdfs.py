#!/usr/bin/env python3
"""Generate Version 0.91 Japanese CPB manuals from Markdown without network access."""

from __future__ import annotations

import argparse
import html
import os
import re
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    HRFlowable, Image, KeepTogether, PageBreak, Paragraph, Preformatted,
    SimpleDocTemplate, Spacer, Table, TableStyle,
)

ROOT = Path(__file__).resolve().parents[1]
PAGE_W, PAGE_H = A4
MARGIN_X = 19 * mm
BODY_W = PAGE_W - 2 * MARGIN_X
MANUALS = (
    (ROOT / "docs/install-manual-ja.md",
     ROOT / "docs/Cala-Pokecom-BASIC-v0.91-Install-Manual-ja.pdf",
     "Install Manual / 導入マニュアル"),
    (ROOT / "docs/system-manual-ja.md",
     ROOT / "docs/Cala-Pokecom-BASIC-v0.91-System-Manual-ja.pdf",
     "System Manual / システムマニュアル"),
    (ROOT / "docs/programming-reference-ja.md",
     ROOT / "docs/Cala-Pokecom-BASIC-v0.91-Programming-Reference-ja.pdf",
     "Programming Reference Manual / プログラミング・リファレンスマニュアル"),
)
FONT_NAME = "CpbNotoJP"


def register_fonts(font_path):
    global FONT_NAME
    if font_path.is_file():
        pdfmetrics.registerFont(TTFont(FONT_NAME, str(font_path)))
        return

    # Reproducible no-network fallback. ReportLab's Japanese CID font keeps
    # PDF generation independent from a host-specific TTF installation.
    FONT_NAME = "HeiseiKakuGo-W5"
    pdfmetrics.registerFont(UnicodeCIDFont(FONT_NAME))


def make_styles():
    base = getSampleStyleSheet()
    gothic, serif = FONT_NAME, FONT_NAME
    return {
        "body": ParagraphStyle("body", parent=base["BodyText"], fontName=serif,
            fontSize=9.2, leading=15, spaceAfter=4.5, textColor=colors.HexColor("#20242A")),
        "title": ParagraphStyle("title", parent=base["Title"], fontName=gothic,
            fontSize=26, leading=35, alignment=TA_CENTER, textColor=colors.HexColor("#153F61")),
        "subtitle": ParagraphStyle("subtitle", parent=base["Heading2"], fontName=gothic,
            fontSize=15, leading=22, alignment=TA_CENTER, textColor=colors.HexColor("#1C6A8D")),
        "h1": ParagraphStyle("h1", parent=base["Heading1"], fontName=gothic,
            fontSize=17, leading=23, spaceBefore=14, spaceAfter=8, textColor=colors.HexColor("#153F61")),
        "h2": ParagraphStyle("h2", parent=base["Heading2"], fontName=gothic,
            fontSize=12.5, leading=18, spaceBefore=12, spaceAfter=5, textColor=colors.HexColor("#1C6A8D")),
        "h3": ParagraphStyle("h3", parent=base["Heading3"], fontName=gothic,
            fontSize=10.5, leading=15, spaceBefore=9, spaceAfter=4, textColor=colors.HexColor("#273C52")),
        "bullet": ParagraphStyle("bullet", parent=base["BodyText"], fontName=serif,
            fontSize=9.2, leading=14, leftIndent=13, firstLineIndent=-9, spaceAfter=2),
        "number": ParagraphStyle("number", parent=base["BodyText"], fontName=serif,
            fontSize=9.2, leading=14, leftIndent=16, firstLineIndent=-13, spaceAfter=2),
        "code": ParagraphStyle("code", parent=base["Code"], fontName="Courier",
            fontSize=7.3, leading=10, leftIndent=7, rightIndent=7, textColor=colors.HexColor("#1D2A36")),
        "table": ParagraphStyle("table", parent=base["BodyText"], fontName=serif,
            fontSize=7.5, leading=10.2, wordWrap="CJK"),
        "table_head": ParagraphStyle("table_head", parent=base["BodyText"], fontName=gothic,
            fontSize=7.5, leading=10.2, textColor=colors.white, wordWrap="CJK"),
        "cover_meta": ParagraphStyle("cover_meta", parent=base["BodyText"], fontName=serif,
            fontSize=10, leading=17, alignment=TA_CENTER, textColor=colors.HexColor("#4A5966")),
        "caption": ParagraphStyle("caption", parent=base["BodyText"], fontName=serif,
            fontSize=7.5, leading=11, alignment=TA_CENTER, textColor=colors.HexColor("#4A5966")),
    }


def inline(text):
    value = html.escape(text, quote=False)
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


def flush(buffer, story, style):
    if buffer:
        story.append(paragraph(" ".join(line.strip() for line in buffer), style))
        buffer.clear()


def markdown_story(source, st, source_path):
    story, buffer = [], []
    lines, index = source.splitlines(), 0
    code_fence = chr(96) * 3
    while index < len(lines):
        line = lines[index]
        if line.startswith(code_fence):
            flush(buffer, story, st["body"])
            block, index = [], index + 1
            while index < len(lines) and not lines[index].startswith(code_fence):
                block.append(lines[index])
                index += 1
            story += [Spacer(1, 2), Preformatted("\n".join(block), st["code"], maxLineLength=103), Spacer(1, 4)]
        elif line.startswith("|"):
            flush(buffer, story, st["body"])
            table_lines = [line]
            index += 1
            while index < len(lines) and lines[index].startswith("|"):
                table_lines.append(lines[index])
                index += 1
            story += [markdown_table(table_lines, st), Spacer(1, 7)]
            continue
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
            story.append(KeepTogether([
                paragraph(line[4:], st["h3"]),
                HRFlowable(width="100%", thickness=0.35, color=colors.HexColor("#B6C8D5"), spaceAfter=3),
            ]))
        elif line.startswith("## "):
            flush(buffer, story, st["body"])
            story.append(paragraph(line[3:], st["h2"]))
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


def cover(title, st):
    return [
        Spacer(1, 48 * mm),
        Paragraph("Cala's Pokecom BASIC System", st["title"]),
        Spacer(1, 9 * mm),
        Paragraph("Version 0.91", st["subtitle"]),
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
    canvas.drawString(MARGIN_X, PAGE_H - 9 * mm, "Cala's Pokecom BASIC System  Version 0.91")
    canvas.setFont(FONT_NAME, 7)
    canvas.drawRightString(PAGE_W - MARGIN_X, 10 * mm, f"{doc.title}  |  {canvas.getPageNumber()}")
    canvas.restoreState()


def build(source, output, title):
    st = make_styles()
    doc = SimpleDocTemplate(
        str(output), pagesize=A4, leftMargin=MARGIN_X, rightMargin=MARGIN_X,
        topMargin=20 * mm, bottomMargin=18 * mm,
        title=f"Cala's Pokecom BASIC System Version 0.91 - {title}",
        author="Cala Maclir", subject="Cala's Pokecom BASIC System Version 0.91",
    )
    doc.build(cover(title, st) + markdown_story(source.read_text(encoding="utf-8"), st, source),
              onFirstPage=lambda canvas, document: None, onLaterPages=page_decor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check input paths without producing PDFs.")
    parser.add_argument(
        "--font",
        default=os.environ.get("CPB_JAPANESE_FONT", ""),
        help="Optional Japanese TrueType font. Without one, use ReportLab HeiseiKakuGo-W5 CID fallback.",
    )
    args = parser.parse_args()
    register_fonts(Path(args.font).expanduser())
    for source, output, title in MANUALS:
        if not source.exists():
            raise SystemExit(f"Missing manual source: {source}")
        if args.check:
            print(f"OK: {source.relative_to(ROOT)} -> {output.relative_to(ROOT)}")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            build(source, output, title)
            print(f"Wrote {output.relative_to(ROOT)}")


if __name__ == "__main__":
    main()

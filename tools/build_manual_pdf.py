#!/usr/bin/env python3
"""Build the Japanese user manual PDF from docs/manual-ja.md."""

from __future__ import annotations

import html
import os
import re
import sys
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate,
    CondPageBreak,
    Frame,
    Image,
    KeepTogether,
    PageBreak,
    PageTemplate,
    Paragraph,
    Preformatted,
    Spacer,
    Table,
    TableStyle,
)


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "manual-ja.md"


def inline_markup(text: str) -> str:
    text = html.escape(text.strip())
    text = re.sub(r"`([^`]+)`", r'<font name="Courier">\1</font>', text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", text)
    return text


def make_styles():
    font_path = os.environ.get("JAPANESE_FONT")
    if not font_path:
        try:
            import japanize_matplotlib
            font_path = str(Path(japanize_matplotlib.__file__).parent / "fonts" / "ipaexg.ttf")
        except ImportError as exc:
            raise SystemExit(
                "Set JAPANESE_FONT to a Japanese TrueType font, or install japanize-matplotlib."
            ) from exc
    pdfmetrics.registerFont(TTFont("IPAGothic", font_path))
    base = getSampleStyleSheet()
    return {
        "title": ParagraphStyle(
            "TitleJP", parent=base["Title"], fontName="IPAGothic",
            fontSize=19.5, leading=26, textColor=colors.HexColor("#0B4778"),
            alignment=TA_CENTER, spaceAfter=10 * mm, wordWrap="CJK"
        ),
        "h1": ParagraphStyle(
            "H1JP", parent=base["Heading1"], fontName="IPAGothic",
            fontSize=17, leading=23, textColor=colors.HexColor("#0B4778"),
            spaceBefore=6 * mm, spaceAfter=3 * mm, keepWithNext=True, wordWrap="CJK"
        ),
        "h2": ParagraphStyle(
            "H2JP", parent=base["Heading2"], fontName="IPAGothic",
            fontSize=13, leading=18, textColor=colors.HexColor("#17699D"),
            spaceBefore=4 * mm, spaceAfter=2 * mm, keepWithNext=True, wordWrap="CJK"
        ),
        "h3": ParagraphStyle(
            "H3JP", parent=base["Heading3"], fontName="IPAGothic",
            fontSize=11.5, leading=16, textColor=colors.HexColor("#24566E"),
            spaceBefore=3 * mm, spaceAfter=1.5 * mm, keepWithNext=True, wordWrap="CJK"
        ),
        "body": ParagraphStyle(
            "BodyJP", parent=base["BodyText"], fontName="IPAGothic",
            fontSize=9.5, leading=15, textColor=colors.HexColor("#202830"),
            spaceAfter=1.8 * mm, wordWrap="CJK", splitLongWords=True
        ),
        "bullet": ParagraphStyle(
            "BulletJP", parent=base["BodyText"], fontName="IPAGothic",
            fontSize=9.3, leading=14, leftIndent=5 * mm, firstLineIndent=-3 * mm,
            spaceAfter=1 * mm, wordWrap="CJK"
        ),
        "quote": ParagraphStyle(
            "QuoteJP", parent=base["BodyText"], fontName="IPAGothic",
            fontSize=9.2, leading=14, leftIndent=6 * mm, rightIndent=6 * mm,
            borderColor=colors.HexColor("#7EB4D5"), borderWidth=1,
            borderPadding=5, backColor=colors.HexColor("#EEF7FC"),
            spaceAfter=3 * mm, wordWrap="CJK"
        ),
        "caption": ParagraphStyle(
            "CaptionJP", parent=base["BodyText"], fontName="IPAGothic",
            fontSize=8.2, leading=11, alignment=TA_CENTER,
            textColor=colors.HexColor("#4D6270"), spaceAfter=4 * mm, wordWrap="CJK"
        ),
        "code": ParagraphStyle(
            "Code", fontName="Courier", fontSize=7.8, leading=10.5,
            leftIndent=4 * mm, rightIndent=4 * mm, borderColor=colors.HexColor("#C7D4DD"),
            borderWidth=0.6, borderPadding=5, backColor=colors.HexColor("#F4F7F9"),
            spaceBefore=1 * mm, spaceAfter=3 * mm
        ),
        "table": ParagraphStyle(
            "TableJP", parent=base["BodyText"], fontName="IPAGothic",
            fontSize=8.3, leading=11, wordWrap="CJK"
        ),
    }


class ManualDoc(BaseDocTemplate):
    def __init__(self, filename: str, **kwargs):
        super().__init__(filename, **kwargs)
        frame = Frame(
            self.leftMargin, self.bottomMargin, self.width, self.height,
            id="normal", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0
        )
        self.addPageTemplates(PageTemplate(id="manual", frames=[frame], onPage=self.decorate))

    def decorate(self, canvas, doc):
        canvas.saveState()
        canvas.setFillColor(colors.HexColor("#0B4778"))
        canvas.rect(0, A4[1] - 12 * mm, A4[0], 12 * mm, fill=1, stroke=0)
        canvas.setFillColor(colors.white)
        canvas.setFont("Helvetica-Bold", 8)
        canvas.drawString(16 * mm, A4[1] - 7.5 * mm, "Cala's Pokecom BASIC Version 0.8")
        canvas.setFillColor(colors.HexColor("#60717D"))
        canvas.setFont("Helvetica", 8)
        canvas.drawRightString(A4[0] - 16 * mm, 10 * mm, f"{doc.page}")
        canvas.setStrokeColor(colors.HexColor("#B9C8D2"))
        canvas.line(16 * mm, 14 * mm, A4[0] - 16 * mm, 14 * mm)
        canvas.restoreState()


def parse_markdown(source: Path, styles: dict):
    lines = source.read_text(encoding="utf-8").splitlines()
    story = []
    i = 0
    first_title = True
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped.startswith("```"):
            code = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            story.append(Preformatted("\n".join(code), styles["code"]))
        elif stripped.startswith("!["):
            match = re.match(r"!\[([^]]*)\]\(([^)]+)\)", stripped)
            if match:
                caption, rel = match.groups()
                path = source.parent / rel
                img = Image(str(path), width=78 * mm, height=78 * mm)
                img.hAlign = "CENTER"
                story.append(KeepTogether([
                    img,
                    Spacer(1, 1.5 * mm),
                    Paragraph(inline_markup(caption), styles["caption"]),
                ]))
        elif stripped.startswith("|") and i + 1 < len(lines) and re.match(r"^\s*\|?[\s:|-]+\|\s*$", lines[i + 1]):
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                raw = [cell.strip() for cell in lines[i].strip().strip("|").split("|")]
                if not all(re.fullmatch(r"[: -]+", cell) for cell in raw):
                    rows.append([Paragraph(inline_markup(cell), styles["table"]) for cell in raw])
                i += 1
            i -= 1
            if rows:
                cols = len(rows[0])
                widths = [doc_width / cols] * cols
                table = Table(rows, colWidths=widths, repeatRows=1, hAlign="LEFT")
                table.setStyle(TableStyle([
                    ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#D8EDF8")),
                    ("TEXTCOLOR", (0, 0), (-1, 0), colors.HexColor("#0B4778")),
                    ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#AFC2CF")),
                    ("VALIGN", (0, 0), (-1, -1), "TOP"),
                    ("LEFTPADDING", (0, 0), (-1, -1), 4),
                    ("RIGHTPADDING", (0, 0), (-1, -1), 4),
                    ("TOPPADDING", (0, 0), (-1, -1), 4),
                    ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
                ]))
                story.extend([table, Spacer(1, 3 * mm)])
        elif stripped.startswith("# "):
            if not first_title:
                story.append(PageBreak())
            story.append(Paragraph(inline_markup(stripped[2:]), styles["title"]))
            first_title = False
        elif stripped.startswith("## "):
            if re.match(r"## (?:[4-9]|1[0-6])\.", stripped) and story:
                # Start major chapters on a new page, but do not create a
                # blank page when the previous flowable already filled the
                # frame and ReportLab has advanced automatically.
                story.append(CondPageBreak(doc_height - 5 * mm))
            story.append(Paragraph(inline_markup(stripped[3:]), styles["h1"]))
        elif stripped.startswith("### "):
            story.append(Paragraph(inline_markup(stripped[4:]), styles["h2"]))
        elif stripped.startswith("#### "):
            story.append(Paragraph(inline_markup(stripped[5:]), styles["h3"]))
        elif stripped.startswith("> "):
            story.append(Paragraph(inline_markup(stripped[2:]), styles["quote"]))
        elif re.match(r"^- ", stripped):
            story.append(Paragraph(inline_markup(stripped[2:]), styles["bullet"], bulletText="•"))
        elif re.match(r"^\d+\. ", stripped):
            number, body = stripped.split(". ", 1)
            story.append(Paragraph(inline_markup(body), styles["bullet"], bulletText=f"{number}."))
        elif stripped == "---":
            story.append(Spacer(1, 4 * mm))
        elif stripped:
            story.append(Paragraph(inline_markup(stripped), styles["body"]))
        else:
            story.append(Spacer(1, 1.2 * mm))
        i += 1
    return story


if __name__ == "__main__":
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "output/pdf/Cala-Pokecom-BASIC-v0.8-ja.pdf"
    output.parent.mkdir(parents=True, exist_ok=True)
    styles = make_styles()
    doc_width = A4[0] - 32 * mm
    doc_height = A4[1] - 36 * mm
    globals()["doc_width"] = doc_width
    globals()["doc_height"] = doc_height
    doc = ManualDoc(
        str(output), pagesize=A4,
        leftMargin=16 * mm, rightMargin=16 * mm,
        topMargin=18 * mm, bottomMargin=18 * mm,
        title="Cala's Pokecom BASIC Version 0.8 日本語マニュアル",
        author="Cala Maclir",
        subject="ClockworkPi PicoCalc user manual",
    )
    doc.build(parse_markdown(SOURCE, styles))
    print(output)

#!/usr/bin/env python3
"""Build the guide PDF from guide.md.

The Markdown subset used here is intentionally small: headings, paragraphs,
fenced code, bullet/numbered lists, pipe tables, block quotes, images, and a
handful of HTML comments used as layout directives. Keeping the source in
plain Markdown means it stays readable on its own and in a browser.
"""

from __future__ import annotations

import copy
import re
from pathlib import Path

from PIL import Image as PILImage
from pygments import lex
from pygments.lexer import RegexLexer
from pygments.lexers import get_lexer_by_name
from pygments.token import (
    Comment, Keyword, Name, Number, Operator, Punctuation, String, Text,
    Whitespace,
)
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate, CondPageBreak, Flowable, Frame, Image, KeepTogether,
    NextPageTemplate, PageBreak, PageTemplate, Paragraph, Spacer, Table,
    TableStyle,
)
from reportlab.platypus.tableofcontents import TableOfContents
from reportlab.platypus.flowables import HRFlowable

ROOT = Path(__file__).resolve().parent
GUIDE_MD = ROOT / "guide.md"
OUT_PDF = ROOT / "LibGUI-Wayland-Guide.pdf"

PAGE_W, PAGE_H = A4
MARGIN_L = 23 * mm
MARGIN_R = 21 * mm
MARGIN_T = 25 * mm
MARGIN_B = 21 * mm
TEXT_W = PAGE_W - MARGIN_L - MARGIN_R

NAVY = colors.HexColor("#123a5c")
NAVY_DARK = colors.HexColor("#0b1c2c")
INK = colors.HexColor("#1f2933")
MUTED = colors.HexColor("#5b6770")
TEAL = colors.HexColor("#2f8f8f")
AMBER = colors.HexColor("#c98a3c")
RULE = colors.HexColor("#d9e0e6")
CODE_BG = colors.HexColor("#f7f8fa")
CODE_BORDER = colors.HexColor("#e1e5ea")

FONT_DIR = Path("/usr/share/fonts/noto")

# ---------------------------------------------------------------------------
# Fonts


def register_fonts():
    fonts = {
        "Serif": "NotoSerif-Regular.ttf",
        "SerifBold": "NotoSerif-Bold.ttf",
        "SerifItalic": "NotoSerif-Italic.ttf",
        "SerifBoldItalic": "NotoSerif-BoldItalic.ttf",
        "Sans": "NotoSans-Regular.ttf",
        "SansBold": "NotoSans-Bold.ttf",
        "SansItalic": "NotoSans-Italic.ttf",
        "SansBoldItalic": "NotoSans-BoldItalic.ttf",
        "SansMedium": "NotoSans-Medium.ttf",
        "Mono": "NotoSansMono-Regular.ttf",
        "MonoBold": "NotoSansMono-Bold.ttf",
    }
    for name, filename in fonts.items():
        pdfmetrics.registerFont(TTFont(name, str(FONT_DIR / filename)))

    pdfmetrics.registerFontFamily(
        "Serif", normal="Serif", bold="SerifBold",
        italic="SerifItalic", boldItalic="SerifBoldItalic",
    )
    pdfmetrics.registerFontFamily(
        "Sans", normal="Sans", bold="SansBold",
        italic="SansItalic", boldItalic="SansBoldItalic",
    )
    pdfmetrics.registerFontFamily(
        "Mono", normal="Mono", bold="MonoBold",
        italic="Mono", boldItalic="MonoBold",
    )


# ---------------------------------------------------------------------------
# Styles


def make_styles():
    styles = {}

    styles["Body"] = ParagraphStyle(
        "Body", fontName="Serif", fontSize=10.1, leading=15.1,
        textColor=INK, alignment=TA_JUSTIFY, spaceAfter=6.2,
    )
    styles["Lead"] = ParagraphStyle(
        "Lead", parent=styles["Body"], fontSize=11.2, leading=17,
        textColor=colors.HexColor("#33414d"), spaceAfter=10,
    )
    styles["H1"] = ParagraphStyle(
        "H1", fontName="SansBold", fontSize=26, leading=31,
        textColor=NAVY, spaceBefore=0, spaceAfter=8,
    )
    styles["H2"] = ParagraphStyle(
        "H2", fontName="SansBold", fontSize=15, leading=19,
        textColor=NAVY, spaceBefore=15, spaceAfter=4.5,
        keepWithNext=1,
    )
    styles["H3"] = ParagraphStyle(
        "H3", fontName="SansBold", fontSize=11.2, leading=15,
        textColor=colors.HexColor("#2c3e50"), spaceBefore=11, spaceAfter=3,
        keepWithNext=1,
    )
    styles["List"] = ParagraphStyle(
        "List", parent=styles["Body"], leftIndent=15, bulletIndent=2,
        spaceAfter=2.6,
    )
    styles["ListNested"] = ParagraphStyle(
        "ListNested", parent=styles["List"], leftIndent=30, bulletIndent=17,
        fontSize=9.7, leading=14.3,
    )
    styles["FiguresHeading"] = ParagraphStyle(
        "FiguresHeading", fontName="SansBold", fontSize=13, leading=17,
        textColor=NAVY, spaceBefore=18, spaceAfter=6,
    )
    styles["FigureNo"] = ParagraphStyle(
        "FigureNo", fontName="SansBold", fontSize=8, leading=10.5, textColor=NAVY,
    )
    styles["FigureList"] = ParagraphStyle(
        "FigureList", fontName="Sans", fontSize=8, leading=10.5, textColor=INK,
    )
    styles["FigurePage"] = ParagraphStyle(
        "FigurePage", fontName="Sans", fontSize=8, leading=10.5, textColor=MUTED,
        alignment=2,
    )
    styles["Caption"] = ParagraphStyle(
        "Caption", fontName="Sans", fontSize=8.3, leading=11,
        textColor=MUTED, alignment=TA_CENTER, spaceBefore=3, spaceAfter=12,
    )
    styles["CalloutTitle"] = ParagraphStyle(
        "CalloutTitle", fontName="SansBold", fontSize=8.6, leading=11,
        textColor=NAVY, spaceAfter=1.5,
    )
    styles["Callout"] = ParagraphStyle(
        "Callout", fontName="Sans", fontSize=9.0, leading=13.0,
        textColor=INK, alignment=TA_LEFT,
    )
    styles["TableHead"] = ParagraphStyle(
        "TableHead", fontName="SansBold", fontSize=8.4, leading=10.6,
        textColor=colors.white,
    )
    styles["TableBody"] = ParagraphStyle(
        "TableBody", fontName="Sans", fontSize=8.35, leading=11.1,
        textColor=INK,
    )
    styles["TableMono"] = ParagraphStyle(
        "TableMono", fontName="Mono", fontSize=7.7, leading=10.4,
        textColor=INK,
    )
    styles["TOC0"] = ParagraphStyle(
        "TOC0", fontName="SansBold", fontSize=9.4, leading=11.8,
        textColor=NAVY, spaceBefore=1.5,
    )
    styles["TOC1"] = ParagraphStyle(
        "TOC1", fontName="Sans", fontSize=8.3, leading=9.7,
        textColor=INK, leftIndent=14, firstLineIndent=-2,
    )
    styles["TOCHeading"] = ParagraphStyle(
        "TOCHeading", fontName="SansBold", fontSize=25, leading=30,
        textColor=NAVY, spaceAfter=10,
    )
    styles["CoverTitle"] = ParagraphStyle(
        "CoverTitle", fontName="SansBold", fontSize=37, leading=43,
        textColor=colors.white,
    )
    styles["CoverSub"] = ParagraphStyle(
        "CoverSub", fontName="Sans", fontSize=12.5, leading=18,
        textColor=colors.HexColor("#c7d7e4"),
    )
    styles["FrontBody"] = ParagraphStyle(
        "FrontBody", parent=styles["Body"], fontSize=10.0, leading=15,
    )
    return styles


STYLES = make_styles()


# ---------------------------------------------------------------------------
# Inline Markdown


def esc(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


_CODE_TOKEN = re.compile(r"`([^`]+)`")


def inline(text: str) -> str:
    tokens: list[str] = []

    def stash(match):
        tokens.append(match.group(1))
        return f"\x00{len(tokens) - 1}\x00"

    text = _CODE_TOKEN.sub(stash, text)
    text = esc(text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"(?<!\*)\*([^*]+?)\*(?!\*)", r"<i>\1</i>", text)
    text = re.sub(
        r"\[([^\]]+)\]\(([^)]+)\)",
        r'<a href="\2" color="#1f6fb2">\1</a>',
        text,
    )

    def unstash(match):
        code = esc(tokens[int(match.group(1))])
        return f'<font name="Mono" size="8.7" color="#8a3b12">{code}</font>'

    return re.sub("\x00(\\d+)\x00", unstash, text)


# ---------------------------------------------------------------------------
# Syntax highlighting

GML_COLORS = {
    "comment": colors.HexColor("#7c8794"),
    "keyword": colors.HexColor("#9c2f6e"),
    "type": colors.HexColor("#2c6e9b"),
    "string": colors.HexColor("#2f7d4f"),
    "number": colors.HexColor("#a15c0a"),
    "operator": colors.HexColor("#44515e"),
    "punct": colors.HexColor("#5b6770"),
    "text": colors.HexColor("#1f2933"),
}


def token_style(ttype):
    if ttype in Comment:
        return GML_COLORS["comment"], True, False
    if ttype in String:
        return GML_COLORS["string"], False, False
    if ttype in Number:
        return GML_COLORS["number"], False, False
    if ttype in Keyword.Type or ttype in Name.Builtin or ttype in Name.Class:
        return GML_COLORS["type"], False, False
    if ttype in Keyword:
        return GML_COLORS["keyword"], False, True
    if ttype in Name.Function or ttype in Name.Attribute:
        return GML_COLORS["type"], False, False
    if ttype in Operator:
        return GML_COLORS["operator"], False, False
    if ttype in Punctuation:
        return GML_COLORS["punct"], False, False
    return GML_COLORS["text"], False, False


class GmlLexer(RegexLexer):
    name = "GML"
    aliases = ["gml"]
    filenames = ["*.gml"]
    tokens = {
        "root": [
            (r"//.*?$", Comment.Single),
            (r"#.*?$", Comment.Single),
            (r"@[A-Za-z_][\w:]*", Name.Class),
            (r'"[^"\n]*"', String),
            (r"\b(?:true|false)\b", Keyword.Constant),
            (r"-?\b\d+\b", Number.Integer),
            (r"[A-Za-z_][\w-]*(?=\s*:)", Name.Attribute),
            (r"[{}\[\]:,]", Punctuation),
            (r"\s+", Whitespace),
            (r".", Text),
        ]
    }


def lexer_for(language: str):
    language = (language or "text").lower()
    if language in {"gml"}:
        return GmlLexer()
    aliases = {
        "c++": "cpp", "cxx": "cpp", "sh": "bash", "shell": "bash",
        "desktop": "ini", "conf": "ini", "text": "text", "plain": "text",
    }
    try:
        return get_lexer_by_name(aliases.get(language, language))
    except Exception:
        return get_lexer_by_name("text")


# ---------------------------------------------------------------------------
# Code block flowable


class CodeBlock(Flowable):
    PAD = 8.5
    HEADER = 15.5
    SIZE = 8.15
    LEADING = 11.2

    def __init__(self, code: str, language: str = "text", title: str | None = None):
        super().__init__()
        self.code = code.rstrip("\n")
        self.language = language or "text"
        self.title = title
        self._lines = None
        self._precomputed = False
        self._continued = False

    # -- layout -----------------------------------------------------------
    def _logical_lines(self):
        lexer = lexer_for(self.language)
        lines: list[list[tuple[str, colors.Color, bool, bool]]] = [[]]
        for ttype, value in lex(self.code, lexer):
            color, italic, bold = token_style(ttype)
            pieces = value.split("\n")
            for index, piece in enumerate(pieces):
                if piece:
                    piece = piece.replace("\t", "    ")
                    lines[-1].append((piece, color, italic, bold))
                if index != len(pieces) - 1:
                    lines.append([])
        return lines

    def _font(self, italic: bool, bold: bool):
        if bold:
            return "MonoBold"
        return "Mono"

    def _line_width(self, spans):
        width = 0.0
        for text, _color, italic, bold in spans:
            width += pdfmetrics.stringWidth(text, self._font(italic, bold), self.SIZE)
        return width

    def _wrap_spans(self, spans, inner):
        if not spans or self._line_width(spans) <= inner:
            return [spans]

        # Flatten to (character, style) so a line can be broken at a space
        # without losing the colour of either side.
        chars = []
        for text, color, italic, bold in spans:
            for ch in text:
                chars.append((ch, color, italic, bold))

        result = []
        start = 0
        while start < len(chars):
            width = 0.0
            last_space = -1
            last_space_width = 0.0
            index = start
            while index < len(chars):
                ch, _c, italic, bold = chars[index]
                w = pdfmetrics.stringWidth(ch, self._font(italic, bold), self.SIZE)
                if width + w > inner and index > start:
                    break
                width += w
                if ch == " ":
                    last_space = index
                    last_space_width = width
                index += 1

            if index >= len(chars):
                end = len(chars)
                nxt = end
            elif last_space > start:
                end = last_space
                nxt = last_space + 1
            else:
                end = index
                nxt = index

            segment = chars[start:end]
            # Group adjacent characters with the same style back into spans.
            grouped = []
            for ch, color, italic, bold in segment:
                if grouped and grouped[-1][1] == color and grouped[-1][2] == italic and grouped[-1][3] == bold:
                    grouped[-1] = (grouped[-1][0] + ch, color, italic, bold)
                else:
                    grouped.append((ch, color, italic, bold))
            result.append(grouped)
            start = nxt

        return result

    def _build_lines(self, width):
        inner = width - 2 * self.PAD
        lines = []
        for logical in self._logical_lines():
            if not logical:
                lines.append([])
                continue
            wrapped = self._wrap_spans(logical, inner)
            for index, part in enumerate(wrapped):
                if index > 0:
                    # A small visual marker makes soft-wrapped code obvious.
                    part = [("\u00bb ", GML_COLORS["comment"], False, False)] + part
                lines.append(part)
        return lines

    def _header_height(self):
        return self.HEADER if (self.title or self.language != "text" or self._continued) else 0

    def _height_for(self, lines):
        return 2 * self.PAD + self._header_height() + len(lines) * self.LEADING + 1

    def wrap(self, availWidth, availHeight):
        self.width = availWidth
        if not self._precomputed:
            self._lines = self._build_lines(availWidth)
        self.height = self._height_for(self._lines)
        return self.width, self.height

    def split(self, availWidth, availHeight):
        self.wrap(availWidth, availHeight)
        if self.height <= availHeight or len(self._lines) < 4:
            return []

        room = availHeight - 2 * self.PAD - self._header_height()
        count = int(room // self.LEADING)
        if count < 3:
            return []
        count = min(count, len(self._lines) - 1)

        first = copy.copy(self)
        first._lines = self._lines[:count]
        first._precomputed = True
        first._continued = False
        first.height = first._height_for(first._lines)

        second = copy.copy(self)
        second._lines = self._lines[count:]
        second._precomputed = True
        second._continued = True
        second.height = second._height_for(second._lines)
        # If the remainder does not fit in the space left on this page, start
        # it on the next one. CondPageBreak does nothing when there is room.
        return [first, CondPageBreak(second.height + 3), second]

    # -- drawing ----------------------------------------------------------
    def draw(self):
        canvas = self.canv
        width = self.width

        canvas.saveState()
        canvas.setFillColor(CODE_BG)
        canvas.setStrokeColor(CODE_BORDER)
        canvas.setLineWidth(0.6)
        canvas.roundRect(0, 0, width, self.height, 3.5, fill=1, stroke=1)

        canvas.setFillColor(colors.HexColor("#c9d2db"))
        canvas.rect(0, 2, 1.8, self.height - 4, fill=1, stroke=0)

        y = self.height - self.PAD
        header_h = self._header_height()
        if header_h:
            canvas.setFillColor(colors.HexColor("#eef1f5"))
            canvas.rect(1.8, self.height - header_h, width - 1.8, header_h, fill=1, stroke=0)
            canvas.setFillColor(colors.HexColor("#d6dde5"))
            canvas.rect(1.8, self.height - header_h, width - 1.8, 0.5, fill=1, stroke=0)

            label = self.title or "CODE"
            canvas.setFillColor(colors.HexColor("#44515e"))
            canvas.setFont("Mono", 7.2)
            canvas.drawString(self.PAD + 1, self.height - header_h + 4.6, label)

            right = self.language.upper() if self.language != "text" else ""
            if self._continued:
                right = (right + "  \u00b7  continued").strip()
            if right:
                canvas.setFillColor(colors.HexColor("#7c8794"))
                canvas.setFont("Mono", 6.6)
                canvas.drawRightString(width - self.PAD, self.height - header_h + 4.8, right)

            y = self.height - header_h - self.PAD
        else:
            y = self.height - self.PAD

        for spans in self._lines:
            x = self.PAD + 1
            baseline = y - self.SIZE + 1.2
            for text, color, italic, bold in spans:
                if not text:
                    continue
                canvas.setFillColor(color)
                canvas.setFont(self._font(italic, bold), self.SIZE)
                canvas.drawString(x, baseline, text)
                x += pdfmetrics.stringWidth(text, self._font(italic, bold), self.SIZE)
            y -= self.LEADING

        canvas.restoreState()


# ---------------------------------------------------------------------------
# Headings, chapter markers


class SetChapter(Flowable):
    """Zero-size flowable that updates the running header before a page break."""

    def __init__(self, title: str):
        super().__init__()
        self.title = title
        self.width = 0
        self.height = 0

    def wrap(self, availWidth, availHeight):
        return 0, 0

    def draw(self):
        doc = getattr(self.canv, "_doctemplate", None)
        if doc is not None:
            doc.current_chapter = self.title


class ChapterHeading(Flowable):
    def __init__(self, title: str, label: str, toc_text: str | None = None):
        super().__init__()
        self.title = title
        self.label = label
        self.toc_text = toc_text or title

    def _layout_title(self, width):
        words = self.title.split()
        lines, current = [], ""
        for word in words:
            candidate = (current + " " + word).strip()
            if pdfmetrics.stringWidth(candidate, "SansBold", 25.5) <= width:
                current = candidate
            else:
                lines.append(current)
                current = word
        if current:
            lines.append(current)
        return lines

    def wrap(self, availWidth, availHeight):
        self.width = availWidth
        self.lines = self._layout_title(availWidth)
        self.height = 26 * mm + max(0, len(self.lines) - 1) * 30
        return self.width, self.height

    def draw(self):
        canvas = self.canv
        canvas.saveState()
        canvas.setFillColor(TEAL)
        canvas.setFont("SansBold", 8.2)
        canvas.drawString(0, self.height - 5, self.label.upper())

        canvas.setFillColor(NAVY)
        canvas.setFont("SansBold", 25.5)
        y = self.height - 32
        for line in self.lines:
            canvas.drawString(0, y, line)
            y -= 30

        rule_y = y + 30 - 12
        canvas.setStrokeColor(TEAL)
        canvas.setLineWidth(1.2)
        canvas.line(0, max(8, rule_y), 34 * mm, max(8, rule_y))
        canvas.restoreState()

    def getPlainText(self):
        return self.toc_text



class FigureCaption(Paragraph):
    """A caption that records the page on which its figure was drawn."""

    def __init__(self, number, alt, style):
        self.figure_number = number
        self.figure_alt = alt
        super().__init__(f"<b>Figure {number}.</b> {inline(alt)}", style)

    def draw(self):
        super().draw()
        doc = getattr(self.canv, "_doctemplate", None)
        if doc is not None:
            doc.figure_entries[self.figure_number] = (self.figure_alt, doc.page)


class ListOfFigures(Flowable):
    def __init__(self, styles):
        super().__init__()
        self.styles = styles
        self._table = None

    def _build(self, availWidth):
        doc = getattr(self.canv, "_doctemplate", None)
        entries = doc.figure_entries if doc is not None else {}
        rows = []
        for number in sorted(entries):
            alt, page = entries[number]
            rows.append([
                Paragraph(f"Figure {number}", self.styles["FigureNo"]),
                Paragraph(esc(alt), self.styles["FigureList"]),
                Paragraph(str(page), self.styles["FigurePage"]),
            ])
        if not rows:
            rows = [[Paragraph("", self.styles["FigureNo"]), Paragraph("", self.styles["FigureList"]), Paragraph("", self.styles["FigurePage"])]]
        table = Table(rows, colWidths=[18 * mm, availWidth - 32 * mm, 14 * mm], hAlign="LEFT")
        table.setStyle(TableStyle([
            ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ("LEFTPADDING", (0, 0), (-1, -1), 0),
            ("RIGHTPADDING", (0, 0), (-1, -1), 4),
            ("TOPPADDING", (0, 0), (-1, -1), 2),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
            ("LINEBELOW", (0, 0), (-1, -1), 0.3, colors.HexColor("#e6eaee")),
        ]))
        return table

    def wrap(self, availWidth, availHeight):
        self._table = self._build(availWidth)
        self.width, self.height = self._table.wrapOn(self.canv, availWidth, availHeight)
        return self.width, self.height

    def draw(self):
        self._table.drawOn(self.canv, 0, 0)

    def split(self, availWidth, availHeight):
        self.wrap(availWidth, availHeight)
        return self._table.split(availWidth, availHeight)


# ---------------------------------------------------------------------------
# Markdown parser


def parse_markdown(text: str, styles, doc):
    lines = text.splitlines()
    story = []
    i = 0
    first_heading_seen = False
    figure_no = 0
    after_h1 = False
    pending_widths = None

    def is_block_start(s: str) -> bool:
        return (
            s.startswith("#")
            or s.startswith("```")
            or s.startswith(">")
            or s.startswith("![")
            or s.startswith("|")
            or s.startswith("- ")
            or s.startswith("* ")
            or bool(re.match(r"^\d+\.\s", s))
            or s.strip() == "---"
            or s.startswith("<!--")
        )

    while i < len(lines):
        raw = lines[i]
        line = raw.rstrip()

        if not line.strip():
            i += 1
            continue

        # Layout directives.
        if line.startswith("<!-- toc -->"):
            story.append(SetChapter("Contents"))
            story.append(PageBreak())
            story.append(Spacer(1, 2))
            story.append(Paragraph("Contents", styles["TOCHeading"]))
            story.append(HRFlowable(width="100%", thickness=0.7, color=RULE,
                                    spaceBefore=2, spaceAfter=8))
            toc = TableOfContents()
            toc.levelStyles = [styles["TOC0"], styles["TOC1"]]
            toc.dotsMinLevel = 0
            toc.firstLevel = 0
            story.append(toc)
            story.append(Paragraph("Figures", styles["FiguresHeading"]))
            story.append(ListOfFigures(styles))
            i += 1
            continue

        if line.startswith("<!-- pagebreak -->"):
            story.append(PageBreak())
            i += 1
            continue

        # Fenced code.
        if line.startswith("```"):
            info = line[3:].strip()
            language = "text"
            title = None
            if info:
                parts = info.split(maxsplit=1)
                language = parts[0]
                if len(parts) > 1:
                    m = re.search(r'title="([^"]*)"', parts[1])
                    if m:
                        title = m.group(1)
                    elif parts[1].strip():
                        title = parts[1].strip()
            code_lines = []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                code_lines.append(lines[i])
                i += 1
            i += 1
            story.append(Spacer(1, 2))
            story.append(CodeBlock("\n".join(code_lines), language, title))
            story.append(Spacer(1, 7))
            continue

        # H1.
        if line.startswith("# "):
            title = line[2:].strip()
            first_heading_seen = True
            after_h1 = True

            label = "Introduction"
            display_title = title
            toc_text = title
            m = re.match(r"^(\d+)\.\s+(.*)$", title)
            if m:
                label = f"Chapter {m.group(1)}"
                display_title = m.group(2)
            else:
                m = re.match(r"^Appendix\s+([A-Z])\.\s+(.*)$", title)
                if m:
                    label = f"Appendix {m.group(1)}"
                    display_title = m.group(2)

            if title != "About this guide":
                story.append(SetChapter(title))
                story.append(PageBreak())
            else:
                story.append(SetChapter("About this guide"))

            story.append(ChapterHeading(display_title, label, toc_text))
            story.append(Spacer(1, 2))
            i += 1
            continue

        # H2 / H3.
        if line.startswith("## "):
            after_h1 = False
            story.append(Paragraph(inline(line[3:].strip()), styles["H2"]))
            i += 1
            continue
        if line.startswith("### "):
            after_h1 = False
            story.append(Paragraph(inline(line[4:].strip()), styles["H3"]))
            i += 1
            continue

        # Image.
        m = re.match(r"^!\[([^\]]*)\]\(([^)]+)\)(?:\{width=([^}]+)\})?$", line)
        if m:
            alt, path, width_override = m.groups()
            image_path = (ROOT / path).resolve()
            pil = PILImage.open(image_path)
            iw, ih = pil.size
            if width_override:
                w = parse_length(width_override)
            else:
                w = min(TEXT_W, 118 * mm)
                if ih / iw > 1.12:
                    w = min(TEXT_W, 103 * mm)
                if ih / iw < 0.72:
                    w = min(TEXT_W, 145 * mm)
            h = w * ih / iw
            img = Image(str(image_path), width=w, height=h, mask="auto")
            img.hAlign = "CENTER"
            figure_no += 1
            caption = FigureCaption(figure_no, alt, styles["Caption"])
            story.append(KeepTogether([Spacer(1, 3), img, caption]))
            i += 1
            continue

        # Block quote / callout.
        if line.startswith(">"):
            quote_lines = []
            while i < len(lines) and lines[i].startswith(">"):
                quote_lines.append(lines[i][1:].lstrip())
                i += 1
            text = " ".join(x.strip() for x in quote_lines if x.strip())
            title = None
            variant = "note"
            m = re.match(r"^\*\*(Note|Tip|Warning|Important|Caution)[:.]?\*\*\s*(.*)$", text, re.I)
            if m:
                title = m.group(1).capitalize()
                text = m.group(2)
                variant = title.lower()
            fill = {
                "note": colors.HexColor("#eef6f7"),
                "tip": colors.HexColor("#eef7f0"),
                "warning": colors.HexColor("#fdf4e7"),
                "important": colors.HexColor("#f3f0fa"),
                "caution": colors.HexColor("#fbeceb"),
            }.get(variant, colors.HexColor("#eef6f7"))
            bar = {
                "note": TEAL, "tip": colors.HexColor("#3d8b56"),
                "warning": AMBER, "important": colors.HexColor("#7b5cb5"),
                "caution": colors.HexColor("#c0504d"),
            }.get(variant, TEAL)
            content = []
            if title:
                content.append(Paragraph(title.upper(), ParagraphStyle(
                    "CalloutLabel", fontName="SansBold", fontSize=7.6,
                    leading=10, textColor=bar, spaceAfter=2)))
            content.append(Paragraph(inline(text), styles["Callout"]))
            cell = Table([[content]], colWidths=[TEXT_W - 2])
            cell.setStyle(TableStyle([
                ("BACKGROUND", (0, 0), (-1, -1), fill),
                ("LINEBEFORE", (0, 0), (0, -1), 2.6, bar),
                ("LEFTPADDING", (0, 0), (-1, -1), 9),
                ("RIGHTPADDING", (0, 0), (-1, -1), 9),
                ("TOPPADDING", (0, 0), (-1, -1), 7),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ]))
            story.append(Spacer(1, 2))
            story.append(cell)
            story.append(Spacer(1, 8))
            continue

        # Table.
        if line.startswith("|") and i + 1 < len(lines) and re.match(r"^\|?[\s:|-]+\|", lines[i + 1]):
            header = [c.strip() for c in line.strip().strip("|").split("|")]
            i += 2
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                i += 1
            widths = None
            if i < len(lines) and lines[i].strip().startswith("<!-- widths:"):
                nums = re.findall(r"[\d.]+", lines[i])
                widths = [float(x) for x in nums]
                i += 1
            story.append(build_table(header, rows, widths, styles))
            story.append(Spacer(1, 8))
            continue

        # Lists.
        if line.startswith("- ") or line.startswith("* ") or re.match(r"^\d+\.\s", line):
            ordered = bool(re.match(r"^\d+\.\s", line))
            items = []
            while i < len(lines):
                current = lines[i]
                if not (current.startswith("- ") or current.startswith("* ") or re.match(r"^\d+\.\s", current)):
                    break
                level = 0
                stripped = current
                if current.startswith("  "):
                    level = 1
                    stripped = current.lstrip()
                    if not (stripped.startswith("- ") or stripped.startswith("* ") or re.match(r"^\d+\.\s", stripped)):
                        break
                body = re.sub(r"^(?:- |\* |\d+\.\s+)", "", stripped).strip()
                items.append((level, body))
                i += 1
            counters = {0: 0, 1: 0}
            for level, body in items:
                counters[level] += 1
                if ordered:
                    bullet = f"{counters[level]}."
                else:
                    bullet = "•" if level == 0 else "–"
                style = styles["List"] if level == 0 else styles["ListNested"]
                story.append(Paragraph(inline(body), style, bulletText=bullet))
            story.append(Spacer(1, 5))
            continue

        if line.strip() == "---":
            story.append(Spacer(1, 4))
            story.append(HRFlowable(width="100%", thickness=0.5, color=RULE,
                                    spaceBefore=1, spaceAfter=7))
            i += 1
            continue

        # Paragraph.
        para_lines = [line.strip()]
        i += 1
        while i < len(lines):
            nxt = lines[i]
            if not nxt.strip() or is_block_start(nxt):
                break
            para_lines.append(nxt.strip())
            i += 1
        text = " ".join(para_lines)
        style = styles["Lead"] if after_h1 else styles["Body"]
        after_h1 = False
        story.append(Paragraph(inline(text), style))

    return story


def parse_length(value: str) -> float:
    value = value.strip()
    if value.endswith("%"):
        return TEXT_W * float(value[:-1]) / 100
    if value.endswith("mm"):
        return float(value[:-2]) * mm
    if value.endswith("pt"):
        return float(value[:-2])
    return float(value)


def build_table(header, rows, widths, styles):
    ncols = len(header)
    if widths and len(widths) == ncols:
        total = sum(widths)
        col_widths = [TEXT_W * w / total for w in widths]
    elif ncols == 2:
        col_widths = [TEXT_W * 0.30, TEXT_W * 0.70]
    elif ncols == 3:
        col_widths = [TEXT_W * 0.22, TEXT_W * 0.34, TEXT_W * 0.44]
    else:
        col_widths = [TEXT_W / ncols] * ncols

    data = [[Paragraph(inline(c), styles["TableHead"]) for c in header]]
    for row in rows:
        row = (row + [""] * ncols)[:ncols]
        data.append([Paragraph(inline(c), styles["TableBody"]) for c in row])

    table = Table(data, colWidths=col_widths, repeatRows=1, hAlign="LEFT")
    table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), NAVY),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("GRID", (0, 0), (-1, -1), 0.45, colors.HexColor("#dbe2e9")),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4.2),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4.2),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f6f8fa")]),
    ]))
    return table


# ---------------------------------------------------------------------------
# Page decoration


def cover_page(canvas, doc):
    canvas.saveState()
    canvas.setFillColor(NAVY_DARK)
    canvas.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)

    # Subtle grid.
    canvas.setStrokeColor(colors.Color(1, 1, 1, alpha=0.035))
    canvas.setLineWidth(0.4)
    for x in range(0, int(PAGE_W), 26):
        canvas.line(x, 0, x, PAGE_H)
    for y in range(0, int(PAGE_H), 26):
        canvas.line(0, y, PAGE_W, y)

    # Amber accent.
    canvas.setFillColor(AMBER)
    canvas.rect(50, PAGE_H - 150, 7, 74, fill=1, stroke=0)

    canvas.setFillColor(colors.white)
    canvas.setFont("SansBold", 36)
    canvas.drawString(50, PAGE_H - 300, "Build a Wayland")
    canvas.drawString(50, PAGE_H - 348, "GUI Application")

    canvas.setFillColor(colors.HexColor("#c7d7e4"))
    canvas.setFont("Sans", 13.2)
    canvas.drawString(50, PAGE_H - 394, "A hands-on introduction to SerenityOS LibGUI")
    canvas.drawString(50, PAGE_H - 414, "on any Wayland compositor")

    # Badge.
    canvas.setFillColor(colors.HexColor("#17384d"))
    canvas.setStrokeColor(colors.HexColor("#2f6f78"))
    canvas.roundRect(50, PAGE_H - 466, 170, 28, 14, fill=1, stroke=1)
    canvas.setFillColor(colors.HexColor("#9fd0c4"))
    canvas.setFont("Mono", 9.2)
    canvas.drawString(66, PAGE_H - 456, "libgui-wayland")

    canvas.setFillColor(colors.HexColor("#8fa8bb"))
    canvas.setFont("Sans", 9.6)
    canvas.drawString(50, 92, "Four complete example applications  ·  C++  ·  native Wayland clients")
    canvas.drawString(50, 74, "2026 edition")

    # Clock motif, bottom right.
    cx, cy, r = PAGE_W - 118, 196, 92
    canvas.saveState()
    canvas.setStrokeColor(colors.Color(0.62, 0.81, 0.76, alpha=0.55))
    canvas.setLineWidth(1.1)
    canvas.circle(cx, cy, r, fill=0, stroke=1)
    for tick in range(12):
        import math
        angle = tick * math.pi / 6 - math.pi / 2
        inner = r - (12 if tick % 3 == 0 else 7)
        canvas.setLineWidth(2.0 if tick % 3 == 0 else 0.9)
        canvas.line(cx + math.cos(angle) * inner, cy + math.sin(angle) * inner,
                    cx + math.cos(angle) * (r - 2), cy + math.sin(angle) * (r - 2))
    canvas.setLineWidth(4.2)
    canvas.setStrokeColor(colors.Color(0.88, 0.64, 0.35, alpha=0.9))
    canvas.line(cx, cy, cx + 0.50 * r, cy - 0.55 * r)
    canvas.setLineWidth(2.2)
    canvas.setStrokeColor(colors.Color(0.80, 0.87, 0.84, alpha=0.9))
    canvas.line(cx, cy, cx - 0.66 * r, cy - 0.20 * r)
    canvas.setFillColor(colors.Color(0.88, 0.64, 0.35, alpha=1.0))
    canvas.circle(cx, cy, 4, fill=1, stroke=0)
    canvas.restoreState()

    canvas.restoreState()


def body_page(canvas, doc):
    canvas.saveState()
    canvas.setFont("Sans", 7.4)
    canvas.setFillColor(colors.HexColor("#8a97a3"))
    canvas.drawString(MARGIN_L, PAGE_H - 14 * mm, "BUILD A WAYLAND GUI APPLICATION")
    canvas.drawRightString(PAGE_W - MARGIN_R, PAGE_H - 14 * mm, "SERENITYOS LIBGUI ON WAYLAND")
    canvas.setStrokeColor(colors.HexColor("#e3e8ed"))
    canvas.setLineWidth(0.5)
    canvas.line(MARGIN_L, PAGE_H - 16.5 * mm, PAGE_W - MARGIN_R, PAGE_H - 16.5 * mm)

    canvas.setFont("Sans", 7.6)
    canvas.setFillColor(colors.HexColor("#8a97a3"))
    canvas.drawString(MARGIN_L, 12 * mm, "LibGUI on Wayland")
    canvas.drawRightString(PAGE_W - MARGIN_R, 12 * mm, str(canvas.getPageNumber()))
    canvas.restoreState()


# ---------------------------------------------------------------------------
# Document


class GuideDoc(BaseDocTemplate):
    def __init__(self, filename, **kwargs):
        super().__init__(filename, **kwargs)
        self.current_chapter = "Contents"
        self._heading_seq = 0
        self.figure_entries = {}

    def afterFlowable(self, flowable):
        if isinstance(flowable, ChapterHeading):
            self._heading_seq += 1
            key = f"heading-{self._heading_seq}"
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(flowable.toc_text, key, level=0, closed=False)
            self.notify("TOCEntry", (0, flowable.toc_text, self.page))
        elif isinstance(flowable, Paragraph) and flowable.style.name == "H2":
            self._heading_seq += 1
            key = f"heading-{self._heading_seq}"
            text = flowable.getPlainText()
            self.canv.bookmarkPage(key)
            self.canv.addOutlineEntry(text, key, level=1, closed=False)
            self.notify("TOCEntry", (1, text, self.page))


def main():
    register_fonts()

    if not GUIDE_MD.exists():
        raise SystemExit(f"missing {GUIDE_MD}")

    text = GUIDE_MD.read_text(encoding="utf-8")

    doc = GuideDoc(
        str(OUT_PDF),
        pagesize=A4,
        leftMargin=MARGIN_L,
        rightMargin=MARGIN_R,
        topMargin=MARGIN_T,
        bottomMargin=MARGIN_B,
        title="Build a Wayland GUI Application",
        author="libgui-wayland contributors",
        subject="A hands-on introduction to SerenityOS LibGUI on Wayland",
        creator="libgui-wayland guide builder",
    )

    cover_frame = Frame(0, 0, PAGE_W, PAGE_H, id="cover",
                        leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
    body_frame = Frame(
        MARGIN_L, MARGIN_B, TEXT_W, PAGE_H - MARGIN_T - MARGIN_B,
        id="body", leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0,
    )

    doc.addPageTemplates([
        PageTemplate(id="cover", frames=[cover_frame], onPage=cover_page),
        PageTemplate(id="body", frames=[body_frame], onPageEnd=body_page),
    ])

    story = [NextPageTemplate("body"), PageBreak()]
    story += parse_markdown(text, STYLES, doc)

    doc.multiBuild(story)
    print(f"wrote {OUT_PDF} ({doc.page} pages)")


if __name__ == "__main__":
    main()

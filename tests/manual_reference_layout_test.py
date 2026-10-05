"""Regression checks for indivisible one-line reference headings and metadata."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate_manual_pdfs as renderer
import validate_manual_reference as reference


class RecordingCanvas:
    def __init__(self): self.text = []; self.boxes = []
    def saveState(self): pass
    def restoreState(self): pass
    def setFillColor(self, value): pass
    def setStrokeColor(self, value): pass
    def setFont(self, *args): pass
    def setLineWidth(self, value): pass
    def drawString(self, x, y, text): self.text.append((x, y, text))
    def roundRect(self, x, y, w, h, *args, **kwargs): self.boxes.append((x, y, w, h))


class ReferenceLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        renderer.register_fonts(Path(''))
        cls.styles = renderer.make_styles()
        cls.markdown = reference.REFERENCE.read_text(encoding='utf-8')

    def test_all_entries_have_two_framed_badges_on_same_baseline(self):
        blocks = reference.sections(self.markdown)
        inventory = list(reference.STATEMENTS) + list(reference.FUNCTIONS) + list(reference.REPL_ENTRIES)
        for name in inventory:
            with self.subTest(name=name):
                reference.require_entry(blocks, name)
                marker = next(reference.MARKER.search(b) for b in blocks[name] if reference.MARKER.search(b))
                heading = renderer.ReferenceHeading(name, *marker.groups())
                width, height = heading.wrap(renderer.BODY_W, 800)
                canvas = RecordingCanvas(); heading.canv = canvas; heading.draw()
                self.assertEqual([s[2] for s in canvas.text], [name, *heading.badges])
                self.assertEqual(len(set(s[1] for s in canvas.text)), 1)
                self.assertEqual(len(canvas.boxes), 2)
                self.assertLessEqual(canvas.boxes[-1][0] + canvas.boxes[-1][2], width + 0.01)
                self.assertGreaterEqual(heading.title_size, 7)
                self.assertLessEqual(canvas.text[0][0] + renderer.pdfmetrics.stringWidth(name, renderer.FONT_NAME, heading.title_size), canvas.boxes[0][0])

    def test_four_modes_are_single_flowables_and_legacy_is_preserved(self):
        for values in [('対応', '非対応'), ('非対応', '対応'), ('対応', '対応'), ('共通', '共通')]:
            source = f'# Manual title\n## Version 0.94\n\n### TEST\n\n> **対応モード:** Classic={values[0]} / Structured={values[1]}\n\nbody\n'
            modern = renderer.markdown_story(source, self.styles, reference.REFERENCE)
            legacy = renderer.markdown_story(source, self.styles, reference.REFERENCE, legacy=True)
            self.assertEqual(sum(isinstance(x, renderer.ReferenceHeading) for x in modern), 1)
            self.assertEqual(len(modern), 2)
            self.assertFalse(any(isinstance(x, renderer.ReferenceHeading) for x in legacy))
            self.assertEqual(legacy[0].getPlainText(), 'TEST')

    def test_current_pdf_links_resolve_from_source_directory(self):
        token = renderer.SOURCE_PATH.set(reference.REFERENCE)
        try:
            link = renderer.inline('[System](system-manual-ja.md#22-system-information)')
            self.assertIn('/blob/main/docs/system-manual-ja.md#22-system-information', link)
        finally:
            renderer.SOURCE_PATH.reset(token)
        self.assertIn('/blob/main/system-manual-ja.md', renderer.inline('[System](system-manual-ja.md)'))

    def test_detached_duplicate_and_invalid_metadata_are_rejected(self):
        marker = '> **対応モード:** Classic=対応 / Structured=対応'
        example = '\n```basic\nPRINT 1\n```\n'
        for block in ['\nbody\n' + marker + example, '\n' + marker + '\n' + marker + example,
                      '\n' + marker.replace('Structured=対応', 'Structured=UNKNOWN') + example]:
            with self.assertRaises(AssertionError): reference.require_entry({'PRINT': [block]}, 'PRINT')
        with self.assertRaises(ValueError):
            renderer.ReferenceHeading('A' * 2000, '対応', '対応').wrap(renderer.BODY_W, 800)


if __name__ == '__main__': unittest.main()

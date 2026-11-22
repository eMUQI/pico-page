import importlib.util
import json
from itertools import islice
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('css_survey', Path(__file__).parents[1] / 'tools/survey_epub_css.py')
survey = importlib.util.module_from_spec(spec)
spec.loader.exec_module(survey)


class CssSurveyTest(unittest.TestCase):
    def test_trailing_escape_always_advances(self):
        result = list(islice(survey.tokens('p{}\\'), 8))
        self.assertEqual(len(result), 4)
        self.assertEqual(result[-1], ('\\', '\\'))

    def test_selector_components_exclude_class_and_id_names(self):
        result = survey.css_usage('p.a.b > .c, #d {margin:1em}')['selectors']
        self.assertEqual(result['type'], 1)
        self.assertEqual(result['class'], 3)
        self.assertEqual(result['id'], 1)
        self.assertEqual(result['compound'], 1)

    def test_associated_styles_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            book = root / 'fixture.epub'
            with zipfile.ZipFile(book, 'w') as archive:
                archive.writestr('text/chapter.xhtml', '<html><head><link rel="stylesheet" href="../style/main.css"/><style>p {text-align:center}</style></head><body><p style="font-size:2em">Sample</p></body></html>')
                archive.writestr('style/main.css', 'p {margin:1em; content:"{};"} /* } */')
                archive.writestr('style/unused.css', 'p {padding:999px}')
            report = survey.survey(book, root / 'output')
            chapter = report['chapters'][0]
            self.assertEqual(chapter['inline'], 1)
            self.assertEqual(chapter['rules'], 2)
            self.assertNotIn('padding', chapter['properties'])
            self.assertEqual(chapter['elements'], 6)
            self.assertIsNone(survey.package_path('a/b.xhtml', '../../../outside'))
            self.assertIsNone(survey.package_path('a/b.xhtml', 'https://example.invalid/style.css'))
            self.assertNotIn('fixture', json.dumps(report))

    def test_import_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            book = root / 'fixture.epub'
            with zipfile.ZipFile(book, 'w') as archive:
                archive.writestr('chapter.xhtml', '<html><link rel="stylesheet" href="a.css"/></html>')
                archive.writestr('a.css', '@import "b.css"; p {margin:1em}')
                archive.writestr('b.css', '@import url("a.css"); div > p.x {font-size:.5em}')
            report = survey.survey(book, root / 'output')
            self.assertEqual(report['chapters'][0]['import_cycles_or_depth_limits'], 1)
            self.assertEqual(report['chapters'][0]['rules'], 2)
            self.assertEqual(report['chapters'][0]['selectors']['child'], 1)
            self.assertEqual(report['chapters'][0]['selectors']['compound'], 1)

    def test_strings_comments_and_shorthands(self):
        result = survey.css_usage('@media print {p {content:";}";margin:1em 2px!important}} /* @import */')
        self.assertEqual(result['rules'], 1)
        self.assertEqual(result['important'], 1)
        self.assertEqual(result['at_rules'], {'media': 1})
        self.assertEqual(result['properties']['margin'], 1)
        self.assertEqual(result['max_depth'], 2)

    def test_html_fallback_keeps_associations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            book = root / 'fixture.epub'
            with zipfile.ZipFile(book, 'w') as archive:
                archive.writestr('chapter.html', '<html><link rel="stylesheet" href="a.css"><p style="font-size:2em">Text&nbsp;')
                archive.writestr('a.css', 'p {margin:1em}')
            report = survey.survey(book, root / 'output')
            self.assertEqual(report['chapters'][0]['inline'], 1)
            self.assertEqual(report['chapters'][0]['rules'], 1)
            self.assertTrue(report['chapters'][0]['html_fallback'])


if __name__ == '__main__':
    unittest.main()

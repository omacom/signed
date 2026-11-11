"""Exercise the headless commands as an agent would: no display, only files and JSON."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

APP = Path(__file__).resolve().parents[1] / 'build/signed'


def write_pdf(path, pages=2):
    """A Letter-sized PDF with a label and a line to sign on, on every page."""
    objects = ['<< /Type /Catalog /Pages 2 0 R >>', None, '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    kids = []
    for page in range(pages):
        content = (f'BT /F1 10 Tf 72 580 Td (Signature {page + 1}) Tj ET\n72 600 m 300 600 l S').encode()
        objects.append(f'<< /Length {len(content)} >>\nstream\n{content.decode()}\nendstream')
        kids.append(len(objects) + 1)
        objects.append(f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] '
                       f'/Resources << /Font << /F1 3 0 R >> >> /Contents {len(objects)} 0 R >>')
    objects[1] = f'<< /Type /Pages /Kids [{" ".join(f"{k} 0 R" for k in kids)}] /Count {pages} >>'
    out, offsets = b'%PDF-1.4\n', []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += f'{number} 0 obj\n{body}\nendobj\n'.encode()
    xref = len(out)
    out += f'xref\n0 {len(objects) + 1}\n0000000000 65535 f \n'.encode()
    out += b''.join(f'{offset:010d} 00000 n \n'.encode() for offset in offsets)
    out += f'trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()
    Path(path).write_bytes(out)


def write_png(path, width=200, height=60):
    """Dark ink strokes on off-white paper, like a photographed signature."""
    rows = b''
    for y in range(height):
        rows += b'\x00' + b''.join(
            b'\x20\x20\x40' if 15 < y < 45 and (x // 10) % 2 and 20 < x < 180 else b'\xee\xea\xe0'
            for x in range(width))
    chunk = lambda kind, data: struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # No display, on a desktop that asks Qt for Wayland: commands must pick offscreen themselves.
        self.env = {key: value for key, value in os.environ.items() if key not in ('DISPLAY', 'WAYLAND_DISPLAY')}
        self.env.update(QT_QPA_PLATFORM='wayland', XDG_DATA_HOME=str(self.root / 'data'),
                        XDG_CONFIG_HOME=str(self.root / 'config'), HOME=str(self.root / 'home'))
        self.pdf = self.root / 'form.pdf'
        write_pdf(self.pdf)
        write_png(self.root / 'photo.png')

    def signed(self, *arguments, code=0, stdin=None):
        result = subprocess.run([str(APP), *map(str, arguments)], env=self.env, cwd=self.root, input=stdin,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def json(self, *arguments):
        return json.loads(self.signed(*arguments, '--json').stdout)

    def add_signature(self, name='Alex Morgan'):
        self.signed('add-signature', name, 'photo.png')

    def test_bare_signed_prints_help(self):
        self.assertIn('sign <pdf> <output>', self.signed().stdout)
        self.assertIn('--items', self.signed('help', 'sign').stdout)
        self.assertIn('"bottom"', self.signed('help', 'items').stdout)

    def test_signature_library(self):
        self.assertEqual(self.json('signatures')['signatures'], [])
        self.add_signature()
        [saved] = self.json('signatures')['signatures']
        self.assertEqual(saved['name'], 'Alex Morgan')
        # The paper is gone and the image is cropped to the ink.
        self.assertLess(saved['width'], 200)
        self.assertTrue(Path(saved['path']).is_file())
        self.assertIn('--replace', self.signed('add-signature', 'Alex Morgan', 'photo.png', code=1).stderr)
        self.signed('add-signature', 'Alex Morgan', 'photo.png', '--replace', '--threshold', '0.4')
        self.assertEqual(self.json('remove-signature', 'Alex Morgan'), {'removed': 'Alex Morgan'})
        self.assertEqual(self.json('signatures')['signatures'], [])
        self.signed('remove-signature', 'Alex Morgan', code=1)
        self.signed('add-signature', '../escape', 'photo.png', code=1)

    def test_pages_and_text(self):
        pages = self.json('pages', self.pdf)['pages']
        self.assertEqual([(p['page'], p['width'], p['height']) for p in pages], [(1, 612, 792), (2, 612, 792)])
        [line] = self.json('text', self.pdf, '--page', '2')['lines']
        self.assertEqual(line['text'], 'Signature 2')
        self.assertAlmostEqual(line['x'], 72, delta=1)
        # PDF space counts up from the bottom; these points count down from the top.
        self.assertAlmostEqual(line['y'] + line['height'], 792 - 580, delta=4)
        self.signed('text', self.pdf, '--page', '3', code=1)

    def test_changing_a_signature_forgets_its_size(self):
        self.add_signature()
        settings = self.root / 'config/signed/signed.conf'
        settings.parent.mkdir(parents=True)
        settings.write_text('[signatureWidth]\nAlex%20Morgan=300\n')
        item = json.dumps({'signature': 'Alex Morgan', 'page': 1, 'x': 10, 'y': 10})
        width = lambda: self.json('sign', self.pdf, 'out.pdf', '--item', item, '--force')['items'][0]['width']
        self.assertEqual(width(), 300)
        self.signed('add-signature', 'Alex Morgan', 'photo.png', '--replace')
        self.assertEqual(width(), 108)
        settings.write_text('[signatureWidth]\nAlex%20Morgan=300\n')
        self.signed('remove-signature', 'Alex Morgan')
        self.add_signature()
        self.assertEqual(width(), 108)

    def test_removing_a_signature_forgets_its_certificate(self):
        # A signature added again under the same name must not seal with the old one's certificate.
        self.add_signature()
        settings = self.root / 'config/signed/signed.conf'
        settings.parent.mkdir(parents=True)
        settings.write_text('[signatureKey]\nAlex%20Morgan=some-certificate\n')
        self.signed('remove-signature', 'Alex Morgan')
        self.assertNotIn('some-certificate', settings.read_text())

    def test_render_never_writes_over_the_pdf(self):
        original = self.pdf.read_bytes()
        (self.root / 'link.pdf').symlink_to(self.pdf)
        for output in (self.pdf, 'link.pdf', 'notes.txt'):
            self.signed('render', self.pdf, '--page', '1', '-o', output, code=1)
        self.assertEqual(self.pdf.read_bytes(), original)
        # A hard link looks like a different file, so the PNG replaces the link instead.
        os.link(self.pdf, self.root / 'alias.png')
        self.signed('render', self.pdf, '--page', '1', '-o', 'alias.png')
        self.assertEqual(self.pdf.read_bytes(), original)
        self.assertEqual((self.root / 'alias.png').read_bytes()[:4], b'\x89PNG')
        self.assertFalse((self.root / 'notes.txt').exists())

    def test_render(self):
        page = self.json('render', self.pdf, '--page', '1', '--width', '612', '--grid', '-o', 'one.png')['pages'][0]
        self.assertEqual((page['width'], page['height'], page['pixelsPerPoint']), (612, 792, 1))
        self.assertTrue((self.root / 'one.png').is_file())
        self.assertEqual(len(self.json('render', self.pdf)['pages']), 2)
        self.assertTrue((self.root / 'pages/page-2.png').is_file())

    def test_sign(self):
        self.add_signature()
        items = [{'signature': 'Alex Morgan', 'page': 2, 'x': 80, 'bottom': 190, 'width': 120},
                 {'text': 'Alex Morgan', 'page': 1, 'x': 80, 'y': 100, 'size': 14},
                 {'date': True, 'page': 1, 'x': 300, 'y': 100}]
        (self.root / 'items.json').write_text(json.dumps({'items': items}))
        result = self.json('sign', self.pdf, 'out.pdf', '--items', 'items.json')
        signature, text, date = result['items']
        self.assertEqual((signature['type'], signature['page'], signature['width']), ('signature', 2, 120))
        self.assertAlmostEqual(signature['y'] + signature['height'], 190, delta=.1)
        self.assertEqual((text['type'], text['size']), ('text', 14))
        self.assertEqual(date['type'], 'date')
        self.assertRegex(date['text'], r'\d{4}')
        # Placed text stays real text, where it was put.
        lines = {line['text']: line for line in self.json('text', 'out.pdf', '--page', '1')['lines']}
        self.assertAlmostEqual(lines['Alex Morgan']['y'], 100, delta=4)
        self.assertIn(date['text'], lines)

        self.assertIn('--force', self.signed('sign', self.pdf, 'out.pdf', '--items', 'items.json', code=1).stderr)
        self.signed('sign', self.pdf, 'out.pdf', '--items', '-', '--force', stdin=json.dumps(items))
        self.assertIn('new name', self.signed('sign', self.pdf, self.pdf, '--items', 'items.json', code=1).stderr)

    def test_sign_reports_every_problem(self):
        self.add_signature()
        errors = self.signed('sign', self.pdf, 'out.pdf',
                             '--item', '{"signature": "Nobody", "page": 1, "x": 0, "y": 0}',
                             '--item', '{"text": "x", "page": 3, "x": 0, "y": 0}',
                             '--item', '{"text": "wide", "page": 1, "x": 600, "y": 0}',
                             '--item', '{"text": "x", "page": 1, "x": 0}',
                             '--item', '{"signature": "Alex Morgan", "page": 1, "x": 0, "y": 0, "width": "160"}',
                             '--item', '{"text": 5, "page": 1, "x": 0, "y": 0}', code=1).stderr.splitlines()
        self.assertEqual(len(errors), 6, errors)
        self.assertIn('Nobody', errors[0])
        self.assertIn('page 3 does not exist', errors[1])
        self.assertIn('past the page', errors[2])
        self.assertIn('"bottom"', errors[3])
        self.assertIn('"width" must be a number', errors[4])
        self.assertIn('"text" must be a string', errors[5])
        self.assertFalse((self.root / 'out.pdf').exists())

    def test_skill_install(self):
        (self.root / 'home/.claude').mkdir(parents=True)
        self.assertIn('name: signed', self.json('skill')['skill'])
        result = self.json('skill', 'install')
        self.assertTrue(result['installed'].endswith('.agents/skills/signed/SKILL.md'))
        self.assertTrue(result['linked'].endswith('.claude/skills/signed'))
        installed = self.root / 'home/.agents/skills/signed/SKILL.md'
        self.assertIn('name: signed', installed.read_text())
        self.assertEqual((self.root / 'home/.claude/skills/signed/SKILL.md').read_text(), installed.read_text())


if __name__ == '__main__':
    unittest.main()

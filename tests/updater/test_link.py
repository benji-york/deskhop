"""Short screen-readable link diagnostics: no hardware opened."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from deskhop_update import console as c

LINE = b'link\r\nB 0.118 C=0/1 U=90/0/3 D=03/11/51 R=512/0 P=0/0\r\ndeskhop> '


class LinkTests(unittest.TestCase):
    def test_local_snapshot(self):
        self.assertEqual(c.parse_link(LINE), {
            'board': 'B', 'build': '0.118', 'scope': 'local', 'snapshot': 'non_atomic',
            'core_age_ms': [0, 1], 'uart': [144, 0, 3], 'dma_flags': [3, 17, 81],
            'rx_remaining': 512, 'rx_dreq': 0, 'cleanup': [0, 0],
        })

    def test_stalls_and_transition_are_observations(self):
        raw = LINE.replace(b'C=0/1', b'C=4294967295/4294967295').replace(b'D=03/11/51', b'D=7f/7f/7f')
        raw = raw.replace(b'R=512/0', b'R=4294967295/63').replace(b'P=0/0', b'P=2/4')
        result = c.parse_link(raw)
        self.assertEqual(result['cleanup'], [2, 4])
        self.assertEqual(result['rx_remaining'], 4294967295)

    def test_strict_grammar_and_no_extra_payload(self):
        for old, new in ((b'B 0.118', b'C 0.118'), (b'0.118', b'garbage'),
                         (b'C=0/1', b'C=-1/1'), (b'D=03', b'D=ff'),
                         (b'R=512/0', b'R=512/64'), (b'P=0/0', b'P=5/0'),
                         (b'U=90', b'U=100000000'), (b'P=0/0', b'P=0/0 text=secret'),
                         (b'link\r\n', b'link A\r\n')):
            with self.subTest(new=new), self.assertRaises(c.ProtocolError):
                c.parse_link(LINE.replace(old, new))

    def test_help_accepts_only_explicit_new_grammar(self):
        from test_console import CONFIG_HELP, HELP
        # Build the full native-app-era grammar, then add only the local command.
        text = CONFIG_HELP.replace(b'Diagnostics are read-only',
            b'  clipboard <session>   Local binary clipboard helper; close to release port.\r\nDiagnostics are read-only')
        text = text.replace(b'  history [count]', b'  link  Short local UART/DMA snapshot.\r\n  history [count]')
        text = text.replace(b'Diagnostics are read-only and query both boards with bounded peer timeouts.',
            b'Diagnostics are read-only. Status/history/verify query both boards\r\nwith bounded peer timeouts; link is local only.')
        c.validate_help(text)
        c.validate_help(HELP)
        with self.assertRaises(c.ProtocolError):
            c.validate_help(text.replace(b'  link ', b'  reset '))


if __name__ == '__main__':
    unittest.main()

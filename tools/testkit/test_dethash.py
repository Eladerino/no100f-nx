"""Tests of dethash.py: python -m unittest discover -s tools/testkit -p "test_*.py" """
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from testkit import dethash  # noqa: E402

A = ("30 20249190 7ad6cc377e73985d 0939aaa27d431ff2\n"
     "60 40498380 3e3db108fe2ca601 0939aaa27d431ff2\n"
     "90 60747570 bf95502af9d2b4a5 0939aaa27d431ff2\n")


class ParseTests(unittest.TestCase):
    def test_lines(self):
        lines = dethash.parse(A)
        self.assertEqual([l.retrace for l in lines], [30, 60, 90])
        self.assertEqual(lines[1].ticks, 40498380)
        self.assertEqual(lines[2].mem1, "bf95502af9d2b4a5")

    def test_unfinished_last_line_is_left_out(self):
        self.assertEqual(len(dethash.parse(A + "120 80996760 66e98b")), 3)

    def test_junk_is_skipped(self):
        self.assertEqual(len(dethash.parse("hello\n" + A + "x y z w\n")), 3)

    def test_missing_file(self):
        self.assertEqual(dethash.read(Path(tempfile.gettempdir()) / "no-such-det-hash.txt"), [])
        self.assertEqual(dethash.last_retrace(Path(tempfile.gettempdir()) / "no-such-det-hash.txt"), -1)

    def test_last_retrace(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.hash"
            p.write_text(A, encoding="ascii")
            self.assertEqual(dethash.last_retrace(p), 90)


class CompareTests(unittest.TestCase):
    def test_identical(self):
        c = dethash.compare(dethash.parse(A), dethash.parse(A))
        self.assertTrue(c.identical)
        self.assertEqual(c.common, 3)
        self.assertIn("identical over 3", c.describe())

    def test_shorter_run_compares_common_part(self):
        c = dethash.compare(dethash.parse(A), dethash.parse(A.splitlines(True)[0]))
        self.assertTrue(c.identical)
        self.assertEqual(c.common, 1)

    def test_first_difference_memory(self):
        b = A.replace("3e3db108fe2ca601", "0000000000000000")
        c = dethash.compare(dethash.parse(A), dethash.parse(b))
        self.assertFalse(c.identical)
        self.assertEqual(c.first_diff[0].retrace, 60)
        self.assertIn("differ from retrace 60 (MEM1)", c.describe())

    def test_first_difference_clock(self):
        b = A.replace("60747570", "60747571")
        c = dethash.compare(dethash.parse(A), dethash.parse(b))
        self.assertIn("virtual clock", c.describe())
        self.assertEqual(c.first_diff[0].retrace, 90)

    def test_empty_is_not_identical(self):
        c = dethash.compare([], dethash.parse(A))
        self.assertFalse(c.identical)
        self.assertIsNone(c.first_diff)
        self.assertIn("nothing to compare", c.describe())


if __name__ == "__main__":
    unittest.main()

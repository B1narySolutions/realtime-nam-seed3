"""Tests for scripts/check_patterns.py: each rule fires, prose doesn't, and an
allow marker needs the right rule and a reason."""

import importlib.util
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent.parent / "scripts" / "check_patterns.py"
spec = importlib.util.spec_from_file_location("check_patterns", SCRIPT)
check_patterns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_patterns)


def hits(rule, line, comment="//"):
    code = check_patterns.strip_comments([line], comment)[0]
    return bool(check_patterns.RULES[rule][1].search(code))


class RuleTest(unittest.TestCase):
    def test_rules_fire_on_code(self):
        self.assertTrue(hits("libm", "float y = std::sin(x);"))
        self.assertTrue(hits("libm", "float y = sinf(x);"))
        self.assertTrue(hits("heap", "auto *p = new float[4];"))
        self.assertTrue(hits("heap", "buf = malloc(4);"))
        self.assertTrue(hits("heap", "auto m = std::make_unique<A2Lite>(w);"))
        self.assertTrue(hits("mem-copy", "memcpy(a, b, n);"))
        self.assertTrue(hits("mem-copy", "std::fill(a, b, 0.0f);"))
        self.assertTrue(hits("fast-math", "CPPFLAGS += -ffast-math", "#"))

    def test_comments_strings_and_lookalikes_do_not(self):
        self.assertFalse(hits("libm", "y = Sin(x);"))
        self.assertFalse(hits("libm", "// uses sin( here"))
        self.assertFalse(hits("heap", 'log("new model");'))
        self.assertFalse(hits("mem-copy", "/* std::fill */ x = 1;"))
        self.assertFalse(hits("fast-math", "# never -ffast-math", "#"))

    def test_marker_needs_rule_and_reason(self):
        allowed = check_patterns.allowed
        self.assertTrue(allowed(["x = new int; // check-patterns: allow heap: load path"], 0, "heap"))
        self.assertTrue(allowed(["// check-patterns: allow heap: load path", "x = new int;"], 1, "heap"))
        self.assertFalse(allowed(["x = new int; // check-patterns: allow heap:"], 0, "heap"))
        self.assertFalse(allowed(["x = new int; // check-patterns: allow libm: no"], 0, "heap"))

    def test_repository_is_clean(self):
        self.assertEqual(check_patterns.main(), 0)


if __name__ == "__main__":
    unittest.main()

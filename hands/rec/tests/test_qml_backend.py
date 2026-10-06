#!/usr/bin/env python3
"""main.qml against ft_handrec.Backend: every backend.name(...) the window calls is a slot, and
every backend.name it reads is a property or a slot. A method that lost its @Slot shows up in
QML only as "is not a function" when its button is pressed (2026-10-03: Export did nothing).
Needs PySide6 (the dev container); skipped without it. FT_HANDREC_QML=PATH checks another
window against the same backend (the standalone Hand Recorder's, in its CI).

  python3 hands/rec/tests/test_qml_backend.py
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REC = os.path.dirname(HERE)
sys.path.insert(0, REC)
QML = os.environ.get("FT_HANDREC_QML") or os.path.join(REC, "main.qml")


class QmlBackendTest(unittest.TestCase):
    def setUp(self):
        try:
            import ft_handrec
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        meta = self.meta = ft_handrec.Backend.staticMetaObject
        self.slots = {bytes(meta.method(i).name()).decode() for i in range(meta.methodCount())}
        self.props = {meta.property(i).name() for i in range(meta.propertyCount())}
        with open(QML) as f:
            self.qml = f.read()

    def test_calls_are_slots(self):
        called = set(re.findall(r"\bbackend\.(\w+)\s*\(", self.qml))
        self.assertTrue(called)
        self.assertEqual(sorted(called - self.slots), [], "called from main.qml but not a slot")

    def test_call_arity(self):
        """Each backend.name(a, b) call passes as many arguments as some slot of that name takes
        (a decorator left at the old count fails only when the button is pressed)."""
        arity = {}
        meta = self.meta
        for i in range(meta.methodCount()):
            mm = meta.method(i)
            arity.setdefault(bytes(mm.name()).decode(), set()).add(mm.parameterCount())
        bad = []
        for m in re.finditer(r"\bbackend\.(\w+)\s*\(", self.qml):
            depth, args, k, seen = 1, 0, m.end(), False
            while depth and k < len(self.qml):
                c = self.qml[k]
                if c in "([{":
                    depth += 1
                elif c in ")]}":
                    depth -= 1
                elif c == "," and depth == 1:
                    args += 1
                elif not c.isspace() and depth >= 1:
                    seen = True
                k += 1
            n = args + 1 if seen else 0
            if m.group(1) in arity and n not in arity[m.group(1)]:
                bad.append("%s: %d arguments, slots take %s" % (m.group(1), n, sorted(arity[m.group(1)])))
        self.assertEqual(bad, [])

    def test_reads_exist(self):
        read = set(re.findall(r"\bbackend\.(\w+)\b(?!\s*\()", self.qml))
        self.assertTrue(read)
        self.assertEqual(sorted(read - self.props - self.slots), [], "read in main.qml but not on Backend")


if __name__ == "__main__":
    unittest.main()

"""Check that saved Piglit outcomes cannot produce misleading success."""
import bz2
from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "piglit_runner", Path(__file__).resolve().parents[1] / "scripts/run_piglit.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class PiglitSummaryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)

    def save(self, name, results, compressed=False):
        directory = self.out / name
        directory.mkdir()
        data = json.dumps({"tests": {k: {"result": v} for k, v in results.items()}})
        if compressed:
            (directory / "results.json.bz2").write_bytes(bz2.compress(data.encode()))
        else:
            (directory / "results.json").write_text(data)

    def summarize(self, names):
        with redirect_stdout(io.StringIO()):
            return runner.summarize(self.out, names)

    def test_failure_and_unsupported_feature_are_distinct(self):
        self.save("svga", {"ok": "pass", "broken": "fail", "unsupported": "skip"}, True)
        self.save("llvmpipe", {"ok": "pass", "broken": "pass", "unsupported": "pass"})
        self.assertEqual(self.summarize(["svga", "llvmpipe"]), 1)
        report = json.loads((self.out / "summary.json").read_text())
        self.assertEqual(report["svga_problems_passing_llvmpipe"], ["broken"])
        self.assertEqual(report["svga_skips_passing_llvmpipe"], ["unsupported"])

    def test_empty_or_all_skipped_run_fails(self):
        for results in ({}, {"unsupported": "skip"}):
            with self.subTest(results=results):
                name = "empty" if not results else "skips"
                self.save(name, results)
                self.assertEqual(self.summarize([name]), 1)

    def test_problem_statuses_fail_even_with_passing_tests(self):
        for status in ("fail", "crash", "timeout", "warn", "notrun", "incomplete"):
            with self.subTest(status=status):
                self.save(status, {"ok": "pass", "problem": status})
                self.assertEqual(self.summarize([status]), 1)

    def test_comparison_rejects_different_test_lists(self):
        self.save("svga", {"one": "pass"})
        self.save("llvmpipe", {"two": "pass"})
        with self.assertRaisesRegex(RuntimeError, "same test names"):
            self.summarize(["svga", "llvmpipe"])

    def test_pass_and_skip_are_reported_separately(self):
        self.save("svga", {"ok": "pass", "unsupported": "skip"})
        self.assertEqual(self.summarize(["svga"]), 0)
        report = json.loads((self.out / "summary.json").read_text())
        self.assertEqual(report["counts"]["svga"], {"pass": 1, "skip": 1})

    def test_subtest_failure_cannot_be_hidden_by_parent_status(self):
        self.save("svga", {"parent": "pass"})
        path = self.out / "svga/results.json"
        data = json.loads(path.read_text())
        data["tests"]["parent"]["subtests"] = {"__type__": "Subtests", "child": "fail"}
        path.write_text(json.dumps(data))
        self.assertEqual(self.summarize(["svga"]), 1)

    def test_interrupted_run_reports_unrun_cases_and_fails(self):
        directory = self.out / "svga"
        (directory / "tests").mkdir(parents=True)
        (directory / "metadata.json").write_text("{}")
        (directory / "tests/0.json").write_text(json.dumps({"one": {"result": "pass"}}))
        (self.out / "test-list.txt").write_text("one\ntwo\n")
        self.save("llvmpipe", {"one": "pass", "two": "pass"})
        self.assertEqual(self.summarize(["svga", "llvmpipe"]), 1)
        report = json.loads((self.out / "summary.json").read_text())
        self.assertFalse(report["completed"]["svga"])
        self.assertEqual(report["counts"]["svga"], {"pass": 1, "notrun": 1})
        self.assertEqual(report["svga_unverified_passing_llvmpipe"], ["two"])
        self.assertEqual(report["svga_problems_passing_llvmpipe"], [])


if __name__ == "__main__":
    unittest.main()

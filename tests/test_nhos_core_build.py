"""Every firmware build passes the nhos_core library.

The code shared with the Hub lives in firmware/libraries/nhos_core, and the
sketch does not compile without it. A build script, CI step or doc command
that leaves --library out fails -- or, worse, someone "fixes" it by copying
the files back into the sketch.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCES = (
    sorted((REPO / "firmware" / "scripts").glob("*.sh"))
    + sorted((REPO / "firmware" / "scripts").glob("*.ps1"))
    + [REPO / ".github" / "workflows" / "ci.yml", REPO / "README.md"]
)
WORKSPACE_DOCS = [REPO.parent / "FLASHING.md", REPO.parent / "CLAUDE.md", REPO.parent / "scripts" / "check_all.sh"]


def compile_commands(text: str) -> list[str]:
    """Each `arduino-cli compile` of the firmware sketch, continuation lines joined."""
    joined = re.sub(r"[\\`]\s*\n\s*", " ", text)
    sketch = re.compile(r"newhorizons_os\b|\$\{SKETCH\}|\$sketch\b")
    return [line for line in joined.splitlines()
            if re.search(r"arduino-cli\s+compile\b", line) and sketch.search(line)]


class NhosCoreBuildTests(unittest.TestCase):
    def check(self, path: Path):
        text = path.read_text(encoding="utf-8")
        for command in compile_commands(text):
            with self.subTest(file=path.name, command=command.strip()[:80]):
                self.assertRegex(command, r"--library\s+\S*nhos_core|--library \$coreLib|--library \"\$\{CORE_LIB\}\"|--library \"\$CORE_LIB\"")
                # The board define goes through compiler.cpp.extra_flags:
                # build.extra_flags holds the core's own defines.
                self.assertNotIn("build.extra_flags=", command.replace("compiler.cpp.extra_flags=", ""))

    def test_repo_scripts_ci_and_readme(self):
        for path in SOURCES:
            self.check(path)

    def test_workspace_docs_when_checked_out(self):
        for path in WORKSPACE_DOCS:
            if path.exists():
                self.check(path)


if __name__ == "__main__":
    unittest.main()

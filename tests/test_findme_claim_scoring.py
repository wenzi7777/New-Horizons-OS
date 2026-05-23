import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


class FindMeClaimScoringTest(unittest.TestCase):
    def _load_findme(self):
        sys.modules["config"] = types.SimpleNamespace(
            GATEWAY_DISCOVERY_TIMEOUT_MS=1500,
            GATEWAY_DISCOVERY_ATTEMPTS=1,
            DEFAULT_GATEWAY_DISCOVERY_PORT=22346,
            DEFAULT_UDP_STREAM_PORT=13250,
        )
        path = REPO_ROOT / "device" / "os" / "findme.py"
        os_dir = str(path.parent)
        inserted = False
        if os_dir not in sys.path:
            sys.path.insert(0, os_dir)
            inserted = True
        spec = importlib.util.spec_from_file_location("findme_under_test", path)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        try:
            spec.loader.exec_module(module)
        finally:
            if inserted:
                sys.path.remove(os_dir)
        return module

    def test_matching_claim_beats_higher_priority_offer(self):
        findme = self._load_findme()

        claim_offer = {"gateway_id": "gw-target", "claim_id": "claim-1", "claim_match": True, "priority": 10, "latency_ms": 100}
        ordinary_offer = {"gateway_id": "gw-other", "priority": 999, "latency_ms": 1}

        best = sorted([ordinary_offer, claim_offer], key=findme._offer_score, reverse=True)[0]

        self.assertEqual(best["gateway_id"], "gw-target")


if __name__ == "__main__":
    unittest.main()

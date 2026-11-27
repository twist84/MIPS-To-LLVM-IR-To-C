import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from audit_iop_exit_ownership import analyze, read_router


class ExitOwnershipTest(unittest.TestCase):
    def setUp(self):
        self.table = "{ptr @name0, i32 4096, i32 16, i64 -1, ptr @iop_aot_fn}"
        self.router = (
            "@iop_region_ids = private unnamed_addr constant [4 x i8] [\n"
            "  i8 0, i8 0, i8 1, i8 -1\n], align 1\n"
            "  %map_offset = sub i32 %start, 4096\n"
            "  %map_in_range = icmp ult i32 %map_offset, 16\n"
            "  switch i8 %map_id, label %fallback [\n"
            "    i8 0, label %region_0\n"
            "    i8 1, label %region_1\n  ]\n"
        )
        self.key, self.ids = read_router(self.router, self.table)
        self.maps = {self.key: ("fixture", self.ids, {})}

    def test_router(self):
        self.assertEqual(self.key, (4096, 0xffffffffffffffff))
        self.assertEqual(self.ids, [0, 0, 1, -1])
        for bad in (
            self.router.replace("i8 1, i8 -1", "i8 2, i8 -1"),
            self.router.replace("i8 1, label %region_1", "i8 1, label %region_0"),
            self.router.replace("i32 %map_offset, 16", "i32 %map_offset, 12"),
        ):
            with self.assertRaises(ValueError):
                read_router(bad, self.table)

    def row(self, resume, **changes):
        return dict(base=self.key[0], hash=self.key[1], entry=4096,
                    resume=resume, reason=0, delay=0, entries=10,
                    retired=20, budget=3, horizon=4, zero=0, **{}) | changes

    def profile(self, rows):
        return dict(schema=1, dropped=0, keys=len(rows), rows=rows,
                    entries=sum(row["entries"] for row in rows),
                    retired=sum(row["retired"] for row in rows))

    def test_attribution(self):
        rows = [
            self.row(4104),
            self.row(4100),
            self.row(4108),
            self.row(4104, delay=1),
            self.row(4104, reason=1),
            self.row(4100, reason=1, zero=2),
        ]
        result = analyze(self.profile(rows), self.maps)
        totals = result["totals"]
        self.assertEqual(totals["entries"], 60)
        self.assertEqual(totals["cross_region_entries"], 30)
        self.assertEqual(totals["cross_region_remaining_budget_candidates"], 7)
        self.assertEqual(totals["cross_region_nonhorizon_lower_bound"], 3)
        self.assertEqual(totals["same_region_remaining_budget_candidates"], 7)
        self.assertEqual(totals["unowned_or_external_remaining_budget_candidates"], 7)
        self.assertEqual(result["cross_region_edges"][0]["candidates"], 7)

    def test_invalid_profile(self):
        row = self.row(4104)
        for profile in (
            self.profile([row, row]),
            self.profile([self.row(4104, budget=11)]),
            self.profile([self.row(4104, hash=123)]),
            self.profile([row]) | {"dropped": 1},
            self.profile([row]) | {"entries": 99},
        ):
            with self.assertRaises(ValueError):
                analyze(profile, self.maps)


if __name__ == "__main__":
    unittest.main()

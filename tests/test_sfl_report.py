#!/usr/bin/env python3
"""Tests for the INF handling in scripts/sfl-report.py."""
import importlib.util
import os
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("sfl_report", os.path.join(HERE, "..", "scripts", "sfl-report.py"))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

INF = r"""
; comment line
[Version]
Signature="$Windows NT$"
Class=Image
Provider=%Mfg%
DriverVer=01/02/2025,1.2.3.4

[Manufacturer]
%Mfg%=Models,NTamd64.5.2,NTamd64.6.0

[Models.NTamd64.5.2]
%Dev.Desc% = OLD_x64, USB\VID_04B8&PID_0999

[Models.NTamd64.6.0]
%Dev.Desc% = NEW_x64, USB\VID_04B8&PID_0999   ; trailing comment

[NEW_x64]
Include=sti.inf
Needs=STI.USBSection
DeviceData=NEW_x64.DeviceData

[NEW_x64.DeviceData]
ModelCode=ES0999
ExtBinModule="esfw99.bin"

[Strings]
Mfg = "EPSON"
Dev.Desc = "EPSON Test Scanner"
"""


class InfTests(unittest.TestCase):
    def test_decode_utf16(self):
        self.assertEqual(report.decode_inf("[Version]".encode("utf-16")), "[Version]")
        self.assertEqual(report.decode_inf(b"[Version]"), "[Version]")

    def test_parse_sections_and_strings(self):
        sections, strings = report.parse_inf(INF)
        self.assertIn("models.ntamd64.6.0", sections)
        self.assertEqual(strings["dev.desc"], "EPSON Test Scanner")
        self.assertEqual(report.subst("%Mfg% scanner", strings), "EPSON scanner")
        self.assertEqual(sections["models.ntamd64.6.0"], [r"%Dev.Desc% = NEW_x64, USB\VID_04B8&PID_0999"])

    def test_keyvals(self):
        self.assertEqual(report.keyvals(["A = 1", "B"]), [("A", "1"), ("B", "")])

    def test_redact_home_only(self):
        home = os.path.expanduser("~")
        self.assertEqual(report.redact(f"{home}/x and /dev/video0"), "~/x and /dev/video0")

    def test_candidate_needs_free_interface(self):
        def dev(cls, driver):
            return {"class": 0, "interfaces": [{"class": cls, "driver": driver}]}
        self.assertTrue(report.is_candidate(dev(0xff, "none")))
        self.assertTrue(report.is_candidate(dev(0x07, "usblp")))
        self.assertFalse(report.is_candidate(dev(0xff, "r8152")))
        self.assertFalse(report.is_candidate(dev(0x03, "usbhid")))


class DriverAnalysisTests(unittest.TestCase):
    UTILITY = r"""[Version]
Class=USB
Provider=EPSON
[Manufacturer]
EPSON=M,NTamd64.6.0
[M.NTamd64.6.0]
"EPSON Scanner Utility 0999" = UTIL, USB\VID_04B8&PID_0999
[UTIL]
"""

    def analyse(self, infs):
        import tempfile, shutil
        work = tempfile.mkdtemp()
        try:
            for name, text in infs.items():
                with open(os.path.join(work, name), "w") as f:
                    f.write(text)
            with mock.patch.object(report.catalog, "extract_recursive", lambda src, dest: None), \
                 mock.patch.object(report.catalog, "index_files",
                                   lambda root: {n: os.path.join(work, n) for n in infs}), \
                 mock.patch.object(report.catalog, "sha256_file", lambda p: "0" * 64), \
                 mock.patch.object(report.os.path, "getsize", lambda p: 1), \
                 mock.patch("builtins.open", wraps=open) as _:
                return report.analyse_driver(os.path.join(work, list(infs)[0]), "04b8", "0999")[1]
        finally:
            shutil.rmtree(work)

    def test_image_class_inf_wins_over_utility_inf(self):
        image = INF.replace("Class=Image", "Class=Image")
        for order in (("a_scanner.inf", "z_utility.inf"), ("z_scanner.inf", "a_utility.inf")):
            infs = {order[0]: image, order[1]: self.UTILITY}
            model = self.analyse(infs)
            self.assertEqual(model["name"], "EPSON Test Scanner", order)
            self.assertEqual(model["registry"]["device_data"]["ModelCode"], "ES0999")


if __name__ == "__main__":
    unittest.main()

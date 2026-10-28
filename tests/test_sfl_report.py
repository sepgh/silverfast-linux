#!/usr/bin/env python3
"""Tests for the INF handling in scripts/sfl-report.py."""
import importlib.util
import os
import unittest

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


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Tests for the scanner catalog (scripts/sfl-models.py) and models/*.json."""
import copy
import importlib.util
import os
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
spec = importlib.util.spec_from_file_location("sfl_models", os.path.join(ROOT, "scripts", "sfl-models.py"))
models = importlib.util.module_from_spec(spec)
spec.loader.exec_module(models)

CATALOG = models.load_all(os.path.join(ROOT, "models"))
V600 = models.find(CATALOG, "epson-perfection-v600")


class CatalogTests(unittest.TestCase):
    def test_every_model_validates(self):
        for m in CATALOG:
            self.assertEqual(models.validate(m), [], m["id"])
            self.assertEqual(os.path.basename(m["_path"]), m["id"] + ".json")

    def test_find_by_id_or_usb(self):
        self.assertIs(models.find(CATALOG, "04b8:013a"), V600)
        with self.assertRaises(models.ModelError):
            models.find(CATALOG, "nope")

    def test_validate_catches_problems(self):
        bad = copy.deepcopy(V600)
        bad["usb"] = "4b8:13a"
        bad["status"] = "maybe"
        bad["driver"]["files"].pop("esfwa1.bin")
        bad["firmware"]["protocol"] = "canon"
        errors = " ".join(models.validate(bad))
        for needle in ("usb must", "status must", "esfwa1.bin", "protocol"):
            self.assertIn(needle, errors)

    def test_field(self):
        self.assertEqual(models.get_field(V600, "firmware.file"), "esfwa1.bin")
        self.assertEqual(models.get_field(V600, "missing.key"), "")


class RegistryTests(unittest.TestCase):
    """The values SilverFast checks, found while reverse engineering (docs/how-it-works.md)."""

    def setUp(self):
        self.reg = models.registry(V600, "0003")

    def test_key_and_device_id_match_slot(self):
        self.assertIn(r"[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class"
                      r"\{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}\0003]", self.reg)
        self.assertIn(r'"DeviceID"="{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}\\0003"', self.reg)

    def test_matching_id_and_types(self):
        self.assertIn(r'"MatchingDeviceId"="usb\\vid_04b8&pid_013a"', self.reg)
        self.assertIn('"SubClass"="StillImage"', self.reg)
        self.assertIn('"DeviceType"=dword:00000001', self.reg)
        self.assertIn('"Capabilities"=dword:00000013', self.reg)

    def test_old_registration_is_replaced(self):
        self.assertTrue(self.reg.splitlines()[2].startswith("[-HKEY_LOCAL_MACHINE"))

    def test_device_data(self):
        self.assertIn('"ExtCmdModule"="esxuina1.dll"', self.reg)
        self.assertIn('"ExtBinModule"="esfwa1.bin"', self.reg)

    def test_slot_must_be_four_digits(self):
        with self.assertRaises(models.ModelError):
            models.registry(V600, "12")


class SelectionTests(unittest.TestCase):
    def test_udev_has_every_model(self):
        rules = models.udev_rules(CATALOG)
        for m in CATALOG:
            vid, pid = m["usb"].split(":")
            self.assertIn(f'ATTR{{idVendor}}=="{vid}", ATTR{{idProduct}}=="{pid}", TAG+="uaccess"', rules)

    def test_select_defaults_to_supported_and_connected(self):
        extra = dict(copy.deepcopy(V600), id="test-experimental", usb="1234:5678", status="experimental")
        cat = CATALOG + [extra]
        with mock.patch.object(models, "load_all", return_value=cat), \
             mock.patch.object(models, "models_dir", return_value="x"):
            for connected, expected in ((set(), ["epson-perfection-v600"]),
                                        ({"1234:5678"}, ["epson-perfection-v600", "test-experimental"])):
                with mock.patch.object(models, "connected_ids", return_value=connected), \
                     mock.patch("builtins.print") as out:
                    models.main(["select"])
                    self.assertEqual(out.call_args[0][0].split("\n"), expected)


class MicrosoftCatalogTests(unittest.TestCase):
    PAGE = '''<table><tr id="ddc77953-120c-40bb-9d21-9022ac13de72_R1">
      <td></td><td><a>Canon - Image - 20.24.0.3</a></td><td>Windows 10,  and later</td>
      <td>Drivers (Other Hardware)</td><td>3/26/2025</td><td>20.24.0.3</td>
      <td><span>9.4 MB</span> 9865488</td><td></td></tr></table>'''

    def test_search_results_are_parsed(self):
        with mock.patch.object(models, "http", return_value=self.PAGE) as http:
            results = models.ms_search("04a9", "1912")
        self.assertIn("USB%5CVID_04A9%26PID_1912", http.call_args[0][0])
        self.assertEqual(results[0]["update_id"], "ddc77953-120c-40bb-9d21-9022ac13de72")
        self.assertEqual(results[0]["title"], "Canon - Image - 20.24.0.3")
        self.assertEqual(results[0]["size"], "9.4 MB")

    def test_download_urls(self):
        page = "downloadInformation[0].files[0].url = 'https://catalog.s.download.windowsupdate.com/x.cab';"
        with mock.patch.object(models, "http", return_value=page):
            self.assertEqual(models.ms_download_urls("id"), ["https://catalog.s.download.windowsupdate.com/x.cab"])


if __name__ == "__main__":
    unittest.main()

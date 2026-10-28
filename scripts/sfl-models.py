#!/usr/bin/env python3
"""Scanner catalog for silverfast-linux.

Every supported (or experimental) scanner is one JSON file in models/; the
format is described in models/README.md. This tool reads them for the
silverfast-linux script and implements the driver lookup.

usage: sfl-models.py [--models DIR] <command> ...
  list [--json]                 all models, marking connected ones
  validate                      check every model file
  connected                     ids of catalog models plugged in right now
  select [ID ...]               models to install: the given ids, else every
                                "supported" model plus connected ones
  field MODEL KEY[.KEY...]      print one value (MODEL: id or path to a .json)
  registry MODEL SLOT           Windows registry (.reg) text for the scanner
  udev                          udev rules granting access to all catalog scanners
  extract MODEL PACKAGE DIR     unpack the driver package and copy the files
                                the model needs into DIR, verifying their SHA-256
  drivers VID:PID               driver sources: the catalog entry and the
                                Microsoft Update Catalog search results
  ms-download UPDATE_ID         print the download URL(s) of a Microsoft
                                Update Catalog entry
"""
import glob
import hashlib
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

IMAGE_CLASS = "{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}"
MS_CATALOG = "https://www.catalog.update.microsoft.com"
STATUSES = ("supported", "experimental")
USB_RE = re.compile(r"^[0-9a-f]{4}:[0-9a-f]{4}$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")


class ModelError(Exception):
    pass


# ------------------------------------------------------------------ catalog

def models_dir(argv_dir=None):
    if argv_dir:
        return argv_dir
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (os.environ.get("SFL_MODELS_DIR"), os.path.join(here, "models"), os.path.join(here, "..", "models")):
        if d and os.path.isdir(d):
            return d
    raise ModelError("models directory not found")


def load_all(mdir):
    models = []
    for path in sorted(glob.glob(os.path.join(mdir, "*.json"))):
        with open(path) as f:
            m = json.load(f)
        m["_path"] = path
        models.append(m)
    return models


def find(models, ref):
    if ref.endswith(".json") and os.path.exists(ref):
        with open(ref) as f:
            m = json.load(f)
        m["_path"] = ref
        return m
    for m in models:
        if m.get("id") == ref or m.get("usb") == ref.lower():
            return m
    raise ModelError(f"unknown scanner model: {ref}")


def validate(m):
    """Returns a list of problems with one model description."""
    errors = []
    for key in ("id", "name", "vendor", "usb", "status", "registry"):
        if key not in m:
            errors.append(f"missing '{key}'")
    if "id" in m and not re.match(r"^[a-z0-9]+(-[a-z0-9]+)*$", m["id"]):
        errors.append("id must be lower-case words joined by '-'")
    if "usb" in m and not USB_RE.match(m["usb"]):
        errors.append("usb must look like 04b8:013a")
    if m.get("status") not in STATUSES:
        errors.append(f"status must be one of {STATUSES}")
    reg = m.get("registry", {})
    for key in ("device_type", "device_subtype", "capabilities"):
        if not isinstance(reg.get(key), int):
            errors.append(f"registry.{key} must be an integer")
    if not isinstance(reg.get("device_data", {}), dict):
        errors.append("registry.device_data must be an object")
    drv = m.get("driver")
    needed = [f for f in (m.get("interpreter"), (m.get("firmware") or {}).get("file")) if f]
    if needed and not drv:
        errors.append("interpreter/firmware need a 'driver' to take them from")
    if drv:
        if not str(drv.get("url", "")).startswith("https://"):
            errors.append("driver.url must be an https URL")
        if not SHA_RE.match(str(drv.get("sha256", ""))):
            errors.append("driver.sha256 must be a SHA-256 hex digest")
        files = drv.get("files", {})
        for f in needed:
            if not SHA_RE.match(str(files.get(f, ""))):
                errors.append(f"driver.files must pin '{f}' with a SHA-256")
    fw = m.get("firmware")
    if fw and fw.get("protocol") != "epson":
        errors.append("firmware.protocol: only 'epson' is implemented")
    return errors


def get_field(m, dotted):
    v = m
    for part in dotted.split("."):
        if not isinstance(v, dict) or part not in v:
            return ""
        v = v[part]
    return json.dumps(v) if isinstance(v, (dict, list)) else str(v)


# ---------------------------------------------------------------- USB state

def connected_ids():
    found = set()
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(f"{d}/idVendor") as a, open(f"{d}/idProduct") as b:
                found.add(f"{a.read().strip()}:{b.read().strip()}")
        except OSError:
            pass
    return found


# --------------------------------------------------------- registry & udev

def reg_escape(value):
    return str(value).replace("\\", "\\\\").replace('"', '\\"')


def registry(m, slot):
    """REGEDIT4 text that registers the scanner like Windows' STI class installer."""
    if not re.match(r"^\d{4}$", slot):
        raise ModelError("slot must be four digits")
    vid, pid = m["usb"].split(":")
    reg = m["registry"]
    key = rf"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\{IMAGE_CLASS}\{slot}"
    values = {
        "DriverDesc": m["name"], "FriendlyName": m["name"],
        "Vendor": m["vendor"], "ProviderName": m["vendor"],
        "SubClass": "StillImage",
        # SilverFast ignores a device unless DeviceID equals its STI name.
        "DeviceID": f"{IMAGE_CLASS}\\{slot}",
        "MatchingDeviceId": f"usb\\vid_{vid}&pid_{pid}",
        "InfPath": reg.get("inf", ""), "InfSection": reg.get("install_section", ""),
        "CreateFileName": f"\\\\.\\Usbscan{int(slot)}",
    }
    lines = ["REGEDIT4", "", f"[-{key}]", "", f"[{key}]"]
    lines += [f'"{k}"="{reg_escape(v)}"' for k, v in values.items()]
    lines += [f'"DeviceType"=dword:{reg["device_type"]:08x}',
              f'"DeviceSubType"=dword:{reg["device_subtype"]:08x}',
              f'"Capabilities"=dword:{reg["capabilities"]:08x}',
              '"HardwareConfig"=dword:00000004', "", f"[{key}\\DeviceData]"]
    lines += [f'"{reg_escape(k)}"="{reg_escape(v)}"' for k, v in reg.get("device_data", {}).items()]
    return "\n".join(lines) + "\n"


def udev_rules(models):
    out = ["# Generated by silverfast-linux: lets the logged-in user open these scanners with libusb.",
           "# Install: sudo install -m644 THIS_FILE /etc/udev/rules.d/70-silverfast-linux.rules",
           "#          sudo udevadm control --reload && sudo udevadm trigger", ""]
    seen = set()
    for m in models:
        if m["usb"] in seen:
            continue
        seen.add(m["usb"])
        vid, pid = m["usb"].split(":")
        out.append(f"# {m['name']} ({m['status']})")
        out.append(f'SUBSYSTEM=="usb", ATTR{{idVendor}}=="{vid}", ATTR{{idProduct}}=="{pid}", TAG+="uaccess"')
    return "\n".join(out) + "\n"


# ------------------------------------------------------- driver packages

def extract_recursive(src, dest, depth=0):
    """Unpacks an installer/archive with 7z, then any archives found inside
    (driver packages nest .cab files in self-extracting .exe files)."""
    if depth > 3:
        return
    if not shutil.which("7z"):
        raise ModelError("7z is required to unpack driver packages")
    out = os.path.join(dest, f"x{depth}_{os.path.basename(src)}")
    subprocess.run(["7z", "x", "-y", "-bso0", "-bse0", "-bsp0", f"-o{out}", src], capture_output=True)
    for root, _, files in os.walk(out):
        for f in files:
            path = os.path.join(root, f)
            if f.lower().endswith((".cab", ".zip", ".msi", ".exe")) and os.path.getsize(path) > 4096:
                extract_recursive(path, dest, depth + 1)


def index_files(root):
    """Maps lower-case file names to their first path under root."""
    files = {}
    for r, _, fs in os.walk(root):
        for f in fs:
            files.setdefault(f.lower(), os.path.join(r, f))
    return files


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def extract_model_files(m, package, dest):
    drv = m.get("driver") or {}
    if drv.get("sha256") and sha256_file(package) != drv["sha256"]:
        raise ModelError(f"{os.path.basename(package)} does not match the SHA-256 pinned in {m['id']}")
    wanted = drv.get("files", {})
    work = tempfile.mkdtemp(prefix="sfl-driver-")
    try:
        extract_recursive(package, work)
        found = index_files(work)
        os.makedirs(dest, exist_ok=True)
        for name, digest in wanted.items():
            src = found.get(name.lower())
            if not src:
                raise ModelError(f"{name} not found in the driver package")
            if sha256_file(src) != digest:
                raise ModelError(f"{name} does not match its pinned SHA-256")
            shutil.copy(src, os.path.join(dest, name.lower()))
            print(name.lower())
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ------------------------------------------------- Microsoft Update Catalog

def http(url, data=None, timeout=30):
    req = urllib.request.Request(url, data=data, headers={
        "User-Agent": "Mozilla/5.0 (silverfast-linux driver lookup)",
        **({"Content-Type": "application/x-www-form-urlencoded"} if data else {}),
    })
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", errors="replace")


def ms_search(vid, pid):
    """Search results for the scanner's Windows hardware ID: list of dicts."""
    query = urllib.parse.quote(f"USB\\VID_{vid.upper()}&PID_{pid.upper()}")
    page = http(f"{MS_CATALOG}/Search.aspx?q={query}")
    results = []
    for row in re.finditer(r'<tr id="([0-9a-f-]{36})_R\d+".*?</tr>', page, re.S):
        cells = [html.unescape(re.sub(r"<[^>]+>", "", c)).strip()
                 for c in re.findall(r"<td[^>]*>(.*?)</td>", row.group(0), re.S)]
        if len(cells) < 7:
            continue
        results.append({"update_id": row.group(1), "title": cells[1], "products": " ".join(cells[2].split()),
                        "classification": cells[3], "date": cells[4], "version": cells[5],
                        "size": cells[6].split()[0] + " " + cells[6].split()[1] if len(cells[6].split()) > 1 else cells[6]})
    return results


def ms_download_urls(update_id):
    payload = json.dumps([{"size": 0, "languages": "", "uidInfo": update_id, "updateID": update_id}])
    page = http(f"{MS_CATALOG}/DownloadDialog.aspx", data=("updateIDs=" + urllib.parse.quote(payload)).encode())
    return re.findall(r"\.url\s*=\s*'([^']+)'", page)


# --------------------------------------------------------------------- CLI

def main(argv):
    mdir_arg = None
    if len(argv) > 1 and argv[0] == "--models":
        mdir_arg, argv = argv[1], argv[2:]
    if not argv:
        print(__doc__)
        return 1
    cmd, args = argv[0], argv[1:]
    models = load_all(models_dir(mdir_arg)) if cmd not in ("ms-download",) else []

    if cmd == "list":
        here = connected_ids()
        if args == ["--json"]:
            print(json.dumps([{k: v for k, v in m.items() if not k.startswith("_")} | {"connected": m["usb"] in here}
                              for m in models], indent=2))
            return 0
        print(f"{'ID':32} {'USB':10} {'STATUS':13} {'CONNECTED':10} NAME")
        for m in models:
            print(f"{m['id']:32} {m['usb']:10} {m['status']:13} {'yes' if m['usb'] in here else '':10} {m['name']}")
        return 0

    if cmd == "validate":
        bad, ids, usbs = 0, set(), set()
        for m in models:
            errors = validate(m)
            if m.get("id") in ids:
                errors.append("duplicate id")
            if m.get("usb") in usbs:
                errors.append("duplicate usb id")
            ids.add(m.get("id"))
            usbs.add(m.get("usb"))
            if os.path.basename(m["_path"]) != f"{m.get('id')}.json":
                errors.append("file name must be <id>.json")
            for e in errors:
                print(f"{os.path.basename(m['_path'])}: {e}")
            bad += bool(errors)
        print(f"{len(models)} model(s), {bad} with problems")
        return 1 if bad else 0

    if cmd == "connected":
        here = connected_ids()
        print("\n".join(m["id"] for m in models if m["usb"] in here))
        return 0

    if cmd == "select":
        if args:
            print("\n".join(find(models, a)["id"] for a in args))
        else:
            here = connected_ids()
            print("\n".join(m["id"] for m in models if m["status"] == "supported" or m["usb"] in here))
        return 0

    if cmd == "field" and len(args) == 2:
        print(get_field(find(models, args[0]), args[1]))
        return 0

    if cmd == "registry" and len(args) == 2:
        sys.stdout.write(registry(find(models, args[0]), args[1]))
        return 0

    if cmd == "extract" and len(args) == 3:
        extract_model_files(find(models, args[0]), args[1], args[2])
        return 0

    if cmd == "udev":
        sys.stdout.write(udev_rules(models))
        return 0

    if cmd == "drivers" and len(args) == 1 and USB_RE.match(args[0].lower()):
        usb = args[0].lower()
        vid, pid = usb.split(":")
        known = [m for m in models if m["usb"] == usb]
        print(f"Driver sources for USB device {usb}\n")
        if known:
            for m in known:
                d = m.get("driver") or {}
                print(f"silverfast-linux catalog: {m['name']} ({m['status']})")
                print(f"  {d.get('info', '')}\n  {d.get('url', '(no driver needed)')}\n")
        else:
            print("silverfast-linux catalog: not listed yet\n")
        print("Microsoft Update Catalog (Windows Update drivers):")
        try:
            results = ms_search(vid, pid)
        except Exception as e:  # network or page format problems are not fatal
            print(f"  search failed: {e}")
            return 0
        if not results:
            print(f"  no results for USB\\VID_{vid.upper()}&PID_{pid.upper()}")
        for i, r in enumerate(results, 1):
            print(f"  [{i}] {r['title']}  version {r['version']}, {r['date']}, {r['size']}")
            print(f"      {r['classification']}; {r['products'][:90]}")
            print(f"      update id {r['update_id']}")
        return 0

    if cmd == "ms-download" and len(args) == 1:
        urls = ms_download_urls(args[0])
        if not urls:
            print("no download found for that update id", file=sys.stderr)
            return 1
        print("\n".join(urls))
        return 0

    print(__doc__)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except ModelError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

#!/usr/bin/env python3
"""Collects what is needed to add support for a scanner, as Markdown.

Run through `silverfast-linux report`; see docs/adding-a-scanner.md. Gathers:
  - system, Wine and silverfast-linux versions
  - USB descriptors of the scanner (from sysfs; serial numbers are omitted)
  - what SANE reports, if it is installed
  - an analysis of the vendor's Windows driver package: the INF entries for
    this scanner's USB ID, its DeviceData (interpreter/firmware), the files it
    installs and whether it uses the generic usbscan.sys stack
Your home directory is replaced with "~"; serial numbers are never read.
"""
import argparse
import datetime
import getpass
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile

def _load_models_module():
    import importlib.util
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sfl-models.py")
    spec = importlib.util.spec_from_file_location("sfl_models", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


catalog = _load_models_module()

SKIP_CLASSES = {0x01: "audio", 0x03: "HID", 0x08: "mass storage", 0x09: "hub", 0x0e: "video",
                0xe0: "wireless", 0x02: "communications", 0x0a: "CDC data", 0x0b: "smart card"}
SCANNER_CLASSES = {0x06: "still image", 0x07: "printer (MFP)", 0xff: "vendor specific"}


def redact(text):
    home = os.path.expanduser("~")
    if len(home) > 1:
        text = text.replace(home, "~")
    try:  # only as a path component, so e.g. a user named "dev" does not hit /dev
        text = text.replace(f"/home/{getpass.getuser()}/", "/home/<user>/")
    except Exception:
        pass
    return text


def run(cmd, timeout=20):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return (out.stdout + out.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"(unavailable: {e.__class__.__name__})"


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def code(text):
    return "```\n" + (text or "(empty)") + "\n```\n"


# ---------------------------------------------------------------- USB (sysfs)

def usb_devices():
    devs = []
    for d in sorted(glob.glob("/sys/bus/usb/devices/*")):
        if ":" in os.path.basename(d) or not os.path.exists(f"{d}/idVendor"):
            continue
        dev = {
            "path": d,
            "vid": read(f"{d}/idVendor"), "pid": read(f"{d}/idProduct"),
            "manufacturer": read(f"{d}/manufacturer"), "product": read(f"{d}/product"),
            "class": int(read(f"{d}/bDeviceClass", "0"), 16),
            "bcdUSB": read(f"{d}/version"), "bcdDevice": read(f"{d}/bcdDevice"),
            "speed": read(f"{d}/speed"), "busnum": read(f"{d}/busnum"), "devnum": read(f"{d}/devnum"),
            "interfaces": [],
        }
        for i in sorted(glob.glob(f"{d}/{os.path.basename(d)}:*")):
            itf = {
                "name": os.path.basename(i),
                "class": int(read(f"{i}/bInterfaceClass", "0"), 16),
                "subclass": read(f"{i}/bInterfaceSubClass"), "protocol": read(f"{i}/bInterfaceProtocol"),
                "driver": os.path.basename(os.path.realpath(f"{i}/driver")) if os.path.exists(f"{i}/driver") else "none",
                "endpoints": [],
            }
            for e in sorted(glob.glob(f"{i}/ep_*")):
                itf["endpoints"].append({
                    "address": read(f"{e}/bEndpointAddress"), "type": read(f"{e}/type"),
                    "direction": read(f"{e}/direction"), "max_packet": read(f"{e}/wMaxPacketSize"),
                })
            dev["interfaces"].append(itf)
        devs.append(dev)
    return devs


# Scanners have no kernel driver (libusb talks to them) or usblp on MFPs; a
# vendor-specific device bound to e.g. a network driver is something else.
FREE_DRIVERS = {"none", "usbfs", "usblp"}


def is_candidate(dev):
    classes = {dev["class"]} | {i["class"] for i in dev["interfaces"]}
    classes.discard(0)
    if not classes or classes & set(SKIP_CLASSES) == classes or not classes & set(SCANNER_CLASSES):
        return False
    return all(i["driver"] in FREE_DRIVERS for i in dev["interfaces"] if i["class"] in SCANNER_CLASSES)


def device_node(dev):
    try:
        return "/dev/bus/usb/%03d/%03d" % (int(dev["busnum"]), int(dev["devnum"]))
    except ValueError:
        return ""


# ------------------------------------------------------- Windows driver (INF)

def decode_inf(raw):
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff") or raw[1:2] == b"\0":
        return raw.decode("utf-16", errors="replace")
    for enc in ("utf-8-sig", "cp1252"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            pass
    return raw.decode("latin-1")


def parse_inf(text):
    sections, cur = {}, None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip() if not line.lstrip().startswith('"') else line.strip()
        if not line:
            continue
        m = re.match(r"^\[(.+)\]$", line)
        if m:
            cur = m.group(1).strip().lower()
            sections.setdefault(cur, [])
        elif cur is not None:
            sections[cur].append(line)
    strings = {}
    for name, lines in sections.items():
        if name == "strings" or name.startswith("strings."):
            if name != "strings" and strings:
                continue
            for l in lines:
                if "=" in l:
                    k, v = l.split("=", 1)
                    strings.setdefault(k.strip().lower(), v.strip().strip('"'))
    return sections, strings


def subst(value, strings):
    return re.sub(r"%([^%]+)%", lambda m: strings.get(m.group(1).lower(), m.group(0)), value)


def keyvals(lines):
    out = []
    for l in lines:
        if "=" in l:
            k, v = l.split("=", 1)
            out.append((k.strip(), v.strip()))
        else:
            out.append((l, ""))
    return out


def pe_machine(path):
    try:
        with open(path, "rb") as f:
            data = f.read(4096)
        off = struct.unpack_from("<I", data, 0x3c)[0]
        machine = struct.unpack_from("<H", data, off + 4)[0]
        return {0x14c: "x86", 0x8664: "x64", 0xaa64: "arm64"}.get(machine, hex(machine))
    except Exception:
        return "?"


def unquote(v):
    return v.strip().strip('"')


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "scanner"


def analyse_driver(path, vid, pid, keep_dir=None, url=None):
    """Returns (markdown, model). model is a draft catalog entry (see
    models/README.md); with keep_dir, the interpreter/firmware named in
    DeviceData are copied there."""
    model = {}
    lines = [f"Package: `{os.path.basename(path)}` ({os.path.getsize(path)} bytes)  ",
             f"SHA-256: `{hashlib.sha256(open(path, 'rb').read()).hexdigest()}`\n"]
    hwid = f"usb\\vid_{vid}&pid_{pid}".lower()
    work = tempfile.mkdtemp(prefix="sfl-report-")
    try:
        catalog.extract_recursive(path, work)
        files = catalog.index_files(work)
        infs = [p for n, p in files.items() if n.endswith(".inf")]
        lines.append(f"Files in package: {len(files)}, INF files: {len(infs)}\n")
        matched = False
        for inf in sorted(infs):
            text = decode_inf(open(inf, "rb").read())
            if hwid not in text.lower():
                continue
            matched = True
            sections, strings = parse_inf(text)
            lines.append(f"### `{os.path.basename(inf)}`\n")
            ver = dict((k.lower(), v) for k, v in keyvals(sections.get("version", [])))
            lines.append(f"Class: `{ver.get('class', '?')}`, DriverVer: `{ver.get('driverver', '?')}`, "
                         f"Provider: `{subst(ver.get('provider', '?'), strings)}`\n")
            installs = []
            for name, body in sections.items():
                for l in body:
                    if hwid in l.lower() and "=" in l:
                        desc, rest = l.split("=", 1)
                        installs.append((name, subst(desc.strip(), strings), rest.split(",")[0].strip()))
            for models_section, desc, inst in installs:
                lines.append(f"- `{models_section}` → **{desc}** → install section `{inst}`")
            # Details come from the section modern 64-bit Windows would use:
            # the NTamd64 models section with the highest OS version.
            def rank(entry):
                section = entry[0].lower()
                version = tuple(int(x) for x in re.findall(r"\d+", section.split("ntamd64", 1)[-1]))
                return ("ntamd64" in section, version)
            best = max(installs, key=rank) if installs else None
            inst = best[2] if best else None
            if not inst:
                continue
            body = keyvals(sections.get(inst.lower(), []))
            info = {k.lower(): v for k, v in body}
            vendor = subst(ver.get("provider", ""), strings).strip('"')
            device_data = {a: unquote(subst(b, strings))
                           for a, b in keyvals(sections.get(info.get("devicedata", "").lower(), []))}
            # Packages can also carry utility INFs that list every USB ID; the
            # still-image driver INF (Class=Image) is the one Windows binds.
            is_image = ver.get("class", "").strip('"').lower() == "image"
            if model and model.get("_image_class") and not is_image:
                continue
            model.update({
                "_image_class": is_image,
                "id": slug(best[1]), "name": best[1], "vendor": vendor, "usb": f"{vid}:{pid}",
                "status": "experimental",
                "tested": "not yet",
                "driver": {"url": url or "", "sha256": catalog.sha256_file(path),
                           "info": f"{os.path.basename(path)}, {ver.get('driverver', '')}", "files": {}},
                "registry": {
                    "inf": os.path.basename(inf).lower(), "install_section": inst,
                    "device_type": int(info.get("devicetype", "1"), 0),
                    "device_subtype": int(info.get("devicesubtype", "1"), 0),
                    "capabilities": int(info.get("capabilities", "0"), 0),
                    "device_data": device_data,
                },
                "_usbscan": "sti.usbsection" in info.get("needs", "").lower(),
            })
            for key in ("ExtCmdModule", "ExtBinModule"):
                name = device_data.get(key)
                src = files.get(name.lower()) if name else None
                if not src:
                    continue
                model["driver"]["files"][name.lower()] = catalog.sha256_file(src)
                if key == "ExtCmdModule":
                    model["interpreter"] = name.lower()
                elif vid == "04b8":  # the only firmware upload protocol implemented is Epson's
                    model["firmware"] = {"file": name.lower(), "protocol": "epson"}
                if keep_dir:
                    os.makedirs(keep_dir, exist_ok=True)
                    shutil.copy(src, os.path.join(keep_dir, name.lower()))
            lines.append(f"\nInstall section `[{inst}]`:\n")
            lines.append(code("\n".join(f"{k} = {v}" if v else k for k, v in body)))
            for k, v in body:
                kl = k.lower()
                if kl == "devicedata":
                    lines.append(f"DeviceData `[{v}]`:\n")
                    lines.append(code("\n".join(f"{a} = {subst(b, strings)}" for a, b in keyvals(sections.get(v.lower(), [])))))
                if kl == "addreg":
                    for sec in v.split(","):
                        regs = sections.get(sec.strip().lower(), [])
                        if regs:
                            lines.append(f"AddReg `[{sec.strip()}]`:\n")
                            lines.append(code("\n".join(regs)))
                if kl == "copyfiles":
                    for sec in v.split(","):
                        names = [l.split(",")[0].strip() for l in sections.get(sec.strip().lower(), [])]
                        if not names:
                            continue
                        lines.append(f"CopyFiles `[{sec.strip()}]`:\n")
                        for n in names:
                            p = files.get(n.lower())
                            info = f"{os.path.getsize(p)} bytes" + (f", PE {pe_machine(p)}" if p and n.lower().endswith((".dll", ".sys", ".exe")) else "") if p else "not found in package"
                            lines.append(f"- `{n}` ({info})")
                        lines.append("")
            uses_usbscan = any(k.lower() == "needs" and "sti.usbsection" in v.lower() for k, v in body)
            lines.append(f"Uses the generic Windows USB scanner stack (`STI.USBSection` → usbscan.sys): "
                         f"**{'yes' if uses_usbscan else 'no / not detected'}**\n")
        if not matched:
            lines.append(f"No INF in this package mentions `{hwid}`. Is this the driver for this scanner?\n")
            ids = set()
            for inf in infs:
                ids.update(re.findall(r"usb\\vid_[0-9a-f]{4}&pid_[0-9a-f]{4}", decode_inf(open(inf, 'rb').read()).lower()))
            if ids:
                lines.append("USB IDs the package does cover: " + ", ".join(f"`{i}`" for i in sorted(ids)) + "\n")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return "\n".join(lines), model


# ---------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--device", help="scanner USB ID as VID:PID (default: auto-detect)")
    ap.add_argument("--driver", help="Windows driver package (.exe/.zip/.cab) for the scanner")
    ap.add_argument("--tool-version", default="?")
    ap.add_argument("--doctor", help="file with `silverfast-linux doctor` output to include")
    ap.add_argument("--model-json", help="also write the draft catalog entry (models/README.md) to this file")
    ap.add_argument("--driver-url", help="where the driver package was downloaded from (recorded in the draft)")
    ap.add_argument("--list-candidates", action="store_true", help="only print VID:PID of likely scanners")
    ap.add_argument("--driver-ids", action="store_true",
                    help="print the USB IDs (VID:PID) the --driver package's INF files cover")
    ap.add_argument("--draft-only", action="store_true",
                    help="print only the draft catalog entry for --device from --driver; the scanner "
                         "does not need to be connected (used to prepare experimental entries)")
    ap.add_argument("--keep-files", help="copy the driver's interpreter/firmware into this directory")
    args = ap.parse_args()

    if args.list_candidates:
        print("\n".join(f"{d['vid']}:{d['pid']}" for d in usb_devices() if is_candidate(d)))
        return 0

    if args.driver_ids:
        work = tempfile.mkdtemp(prefix="sfl-report-")
        try:
            catalog.extract_recursive(args.driver, work)
            ids = set()
            for name, path in catalog.index_files(work).items():
                if name.endswith(".inf"):
                    text = decode_inf(open(path, "rb").read()).lower()
                    ids.update(f"{v}:{p}" for v, p in re.findall(r"usb\\vid_([0-9a-f]{4})&pid_([0-9a-f]{4})", text))
            print("\n".join(sorted(ids)))
        finally:
            shutil.rmtree(work, ignore_errors=True)
        return 0

    if args.draft_only:
        if not (args.device and args.driver):
            ap.error("--draft-only needs --device VID:PID and --driver")
        vid, _, pid = args.device.lower().partition(":")
        _, model = analyse_driver(args.driver, vid, pid, args.keep_files, args.driver_url)
        draft = {k: v for k, v in model.items() if not k.startswith("_")}
        if not draft:
            print(f"error: no INF in {os.path.basename(args.driver)} covers {args.device}", file=sys.stderr)
            return 1
        print(json.dumps(draft, indent=2))
        return 0

    out = [f"# silverfast-linux scanner report\n",
           f"Generated {datetime.datetime.now().strftime('%Y-%m-%d %H:%M')} by silverfast-linux {args.tool_version}\n"]

    osr = dict(l.split("=", 1) for l in read("/etc/os-release").splitlines() if "=" in l)
    out.append("## System\n")
    out.append(f"| | |\n|---|---|\n| Distribution | {osr.get('PRETTY_NAME', '?').strip(chr(34))} |\n"
               f"| Kernel | {platform.release()} ({platform.machine()}) |\n"
               f"| glibc | {platform.libc_ver()[1] or '?'} |\n"
               f"| Wine | {run(['wine', '--version']) if shutil.which('wine') else 'not installed'} |\n"
               f"| libusb | {run(['pkg-config', '--modversion', 'libusb-1.0']) if shutil.which('pkg-config') else '?'} |\n")

    devs = usb_devices()
    cands = [d for d in devs if is_candidate(d)]
    out.append("## USB devices\n")
    out.append("| ID | Manufacturer | Product | Classes | Likely scanner |\n|---|---|---|---|---|")
    for d in devs:
        classes = sorted({d["class"]} | {i["class"] for i in d["interfaces"]})
        names = ", ".join(SCANNER_CLASSES.get(c) or SKIP_CLASSES.get(c) or f"0x{c:02x}" for c in classes)
        out.append(f"| `{d['vid']}:{d['pid']}` | {d['manufacturer']} | {d['product']} | {names} | {'✓' if d in cands else ''} |")
    out.append("")

    dev = None
    if args.device:
        vid, _, pid = args.device.lower().partition(":")
        dev = next((d for d in devs if d["vid"] == vid and d["pid"] == pid), None)
        if not dev:
            out.append(f"**Device `{args.device}` is not connected.**\n")
    elif len(cands) == 1:
        dev = cands[0]
    else:
        out.append(f"**{len(cands)} possible scanners found; re-run with `--device VID:PID`.**\n")

    if dev:
        node = device_node(dev)
        out.append(f"## Scanner `{dev['vid']}:{dev['pid']}` — {dev['manufacturer']} {dev['product']}\n")
        out.append(f"| | |\n|---|---|\n| USB version | {dev['bcdUSB']} |\n| Device release | {dev['bcdDevice']} |\n"
                   f"| Speed | {dev['speed']} Mbit/s |\n| Device class | 0x{dev['class']:02x} |\n"
                   f"| Accessible by this user | {'yes' if node and os.access(node, os.R_OK | os.W_OK) else 'no'} |\n")
        for itf in dev["interfaces"]:
            out.append(f"Interface `{itf['name'].split(':')[-1]}`: class 0x{itf['class']:02x} "
                       f"({SCANNER_CLASSES.get(itf['class']) or SKIP_CLASSES.get(itf['class']) or 'other'}), "
                       f"subclass {itf['subclass']}, protocol {itf['protocol']}, kernel driver: `{itf['driver']}`\n")
            for e in itf["endpoints"]:
                out.append(f"- endpoint `0x{e['address']}` {e['type']} {e['direction']}, max packet `0x{e['max_packet']}`")
            out.append("")

    out.append("## SANE\n")
    if shutil.which("scanimage"):
        out.append(code(run(["scanimage", "-L"], timeout=30)))
    else:
        out.append("scanimage not installed.\n")

    if args.driver:
        out.append("## Windows driver\n")
        if dev and os.path.exists(args.driver):
            text, model = analyse_driver(args.driver, dev["vid"], dev["pid"], args.keep_files, args.driver_url)
            out.append(text)
            draft = {k: v for k, v in model.items() if not k.startswith("_")}
            if draft:
                out.append("### Draft catalog entry\n")
                out.append("A starting point for `models/" + draft["id"] + ".json` (see models/README.md):\n")
                out.append("```json\n" + json.dumps(draft, indent=2) + "\n```\n")
            if args.model_json:
                with open(args.model_json, "w") as f:
                    json.dump(draft, f, indent=2)
        elif not dev:
            out.append("Skipped: no scanner selected.\n")
        else:
            out.append(f"Driver file `{os.path.basename(args.driver)}` not found.\n")

    if args.doctor and os.path.exists(args.doctor):
        out.append("## silverfast-linux doctor\n")
        out.append(code(re.sub(r"\x1b\[[0-9;]*m", "", read(args.doctor))))

    print(redact("\n".join(out)))


if __name__ == "__main__":
    sys.exit(main())

import xml.etree.ElementTree as ET
import requests
import os
import json
import time
from datetime import datetime

# ---------- CONFIG ----------
XLIGHTS_XML_PATH = r"F:\Lights 2025\xlights_networks.xml"
HTTP_TIMEOUT = 10  # seconds

# ---------- HELPERS ----------
def is_matrix_controller(ctrl):
    # Skip FPP/matrix controllers; we only want ESP-style endpoints
    for tag in ["Name", "Description", "comment"]:
        val = ctrl.get(tag, "")
        if val and "fpp" in val.lower():
            return True
    return False

def get_controller_summary(ctrl):
    ip = ctrl.get("IP", "UNKNOWN_IP")
    name = ctrl.get("Name", "")
    desc = ctrl.get("Description", "")
    display_name = name if name else (desc if desc else "(no name)")
    return (ip, display_name)

def get_target_controllers(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()
    controllers = root.findall(".//Controller")

    targets = []
    for ctrl in controllers:
        ip = ctrl.get("IP")
        if not ip:
            continue
        if is_matrix_controller(ctrl):
            print(f"🚫 Skipping matrix/FPP controller: {ip} [{ctrl.get('Name','')}]")
            continue
        targets.append(get_controller_summary(ctrl))
    return targets

def parse_selection(selection, count):
    selection = selection.strip().lower()
    if selection == "all":
        return list(range(count))
    chosen = set()
    for part in selection.split(','):
        part = part.strip()
        if '-' in part:
            start, end = part.split('-', 1)
            try:
                s = int(start) - 1
                e = int(end) - 1
            except ValueError:
                continue
            for i in range(s, e + 1):
                if 0 <= i < count:
                    chosen.add(i)
        else:
            try:
                idx = int(part) - 1
            except ValueError:
                continue
            if 0 <= idx < count:
                chosen.add(idx)
    return sorted(chosen)

def check_reachable(ip):
    # Quick probe using a config endpoint we’ll touch anyway
    try:
        r = requests.get(f"http://{ip}/conf/config.json", timeout=HTTP_TIMEOUT)
        return r.status_code == 200
    except requests.RequestException:
        return False

# ---------- CONTROLLER CHANGES ----------
def enable_fpp(ip, results):
    """Set secondary input to FPP Remote and configure Marquee effect groups."""
    url = f"http://{ip}/conf/input_config.json"
    try:
        r = requests.get(url, timeout=HTTP_TIMEOUT)
        if r.status_code != 200:
            raise Exception(f"GET {url} -> HTTP {r.status_code}")
        config = r.json()

        # Make sure the nested structure exists
        ic = config.setdefault('input_config', {})
        ch = ic.setdefault('channels', {})
        ch1 = ch.setdefault('1', {})  # Secondary input slot "1"
        ch1['type'] = 5  # 5 == FPP Remote (as used in your original flow)

        # Ensure channel "1" sub-config exists
        ch1cfg = ch1.setdefault('1', {})
        ch1cfg.update({
            "type": "Effects",
            "currenteffect": "Marquee",
            "EffectSpeed": 6,
            "EffectReverse": False,
            "EffectMirror": False,
            "EffectAllLeds": False,
            "EffectBrightness": 100,
            "EffectWhiteChannel": False,
            "EffectColor": "#b700ff",
            "pixel_count": 1,
            "FlashEnable": False,
            "FlashMinInt": 100,
            "FlashMaxInt": 100,
            "FlashMinDelay": 100,
            "FlashMaxDelay": 5000,
            "FlashMinDur": 25,
            "FlashMaxDur": 50,
            "TransCount": 300,
            "TransDelay": 100,
            "effects": [
                {"name": "Solid"}, {"name": "Blink"}, {"name": "Flash"},
                {"name": "Rainbow"}, {"name": "Chase"}, {"name": "Fire flicker"},
                {"name": "Lightning"}, {"name": "Breathe"}, {"name": "Random"},
                {"name": "Transition"}, {"name": "Marquee"}
            ],
            "MarqueeGroups": [
                {"brightness": 100, "brightnessEnd": 100, "pixel_count": 5,
                 "color": {"r": 255, "g": 0, "b": 0}},
                {"brightness": 100, "brightnessEnd": 100, "pixel_count": 5,
                 "color": {"r": 0, "g": 255, "b": 0}},
                {"brightness": 100, "brightnessEnd": 100, "pixel_count": 5,
                 "color": {"r": 0, "g": 0, "b": 255}},
                {"brightness": 100, "brightnessEnd": 100, "pixel_count": 5,
                 "color": {"r": 128, "g": 128, "b": 128}},
                {"brightness": 100, "brightnessEnd": 100, "pixel_count": 5,
                 "color": {"r": 0, "g": 0, "b": 0}}
            ]
        })

        pr = requests.post(url, headers={'Content-Type': 'application/json'},
                           data=json.dumps(config), timeout=HTTP_TIMEOUT)
        if pr.status_code == 200:
            msg = f"🔧 FPP + Marquee configured"
        else:
            msg = f"⚠️ POST {url} -> HTTP {pr.status_code}: {pr.text.strip()}"
        results[ip].append(msg)
        print(f"{ip}: {msg}")
    except Exception as e:
        msg = f"❌ Failed FPP/Marquee: {e}"
        results[ip].append(msg)
        print(f"{ip}: {msg}")

def update_system_config(ip, device_name, results):
    """Update /conf/config.json with ID, hostname, and AP SSID."""
    url = f"http://{ip}/conf/config.json"
    try:
        r = requests.get(url, timeout=HTTP_TIMEOUT)
        if r.status_code != 200:
            raise Exception(f"GET {url} -> HTTP {r.status_code}")
        config = r.json()

        syscfg = config.setdefault('system', {})
        dev = syscfg.setdefault('device', {})
        net = syscfg.setdefault('network', {})
        wifi = net.setdefault('wifi', {})

        dev['id'] = device_name
        net['hostname'] = device_name
        wifi['ap_ssid'] = f"{device_name} - AP"

        pr = requests.post(url, headers={'Content-Type': 'application/json'},
                           data=json.dumps(config), timeout=HTTP_TIMEOUT)
        if pr.status_code == 200:
            msg = f"🔧 System (ID/hostname/AP SSID) updated"
        else:
            msg = f"⚠️ POST {url} -> HTTP {pr.status_code}: {pr.text.strip()}"
        results[ip].append(msg)
        print(f"{ip}: {msg}")
    except Exception as e:
        msg = f"❌ Failed system config: {e}"
        results[ip].append(msg)
        print(f"{ip}: {msg}")




# ---------- MAIN ----------
def main():
    if not os.path.isfile(XLIGHTS_XML_PATH):
        print(f"❌ xLights XML not found: {XLIGHTS_XML_PATH}")
        return

    targets = get_target_controllers(XLIGHTS_XML_PATH)
    if not targets:
        print("🚫 No non-matrix controllers found.")
        return

    print(f"\n🎯 Found {len(targets)} controller(s):")
    for idx, (ip, name) in enumerate(targets, 1):
        print(f" {idx:2d}) {ip}  [{name}]")

    selection = input(
        "\nSelect controllers to configure (e.g. 1,3-5) or 'all' [default: all]: "
    ).strip() or "all"
    indices = parse_selection(selection, len(targets))
    if not indices:
        print("❌ No valid selection made. Aborting.")
        return

    chosen = [targets[i] for i in indices]
    print("\n🔧 You chose to configure:")
    for ip, name in chosen:
        print(f" - {ip} [{name}]")

    confirm = input("\n⚠️ Type 'yes' to push API changes: ").strip().lower()
    if confirm != "yes":
        print("❌ Aborted by user.")
        return

    results = {}
    for ip, name in chosen:
        results[ip] = []
        # Reachability check (no reboot loop since we’re NOT flashing firmware)
        if not check_reachable(ip):
            msg = "❌ Not reachable (skipping)"
            results[ip].append(msg)
            print(f"{ip}: {msg}")
            continue

        enable_fpp(ip, results)
        update_system_config(ip, name, results)

    print("\n📋 Summary of API changes:")
    for ip, msgs in results.items():
        print(f" - {ip}:")
        for m in msgs:
            print(f"   • {m}")


    input("\n✅ Done. Press Enter to exit...")

if __name__ == "__main__":
    main()

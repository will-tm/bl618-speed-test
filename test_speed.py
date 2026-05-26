#!/usr/bin/env python3
"""Selenium-based speed test verification for the WiFi speed test app."""

import sys
import time
import json
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC

URL = sys.argv[1] if len(sys.argv) > 1 else "http://10.0.101.121"
TIMEOUT = 180

print(f"Testing {URL}")

options = webdriver.ChromeOptions()
options.add_argument("--headless=new")
driver = webdriver.Chrome(options=options)

# Enable CDP console and network logging
driver.execute_cdp_cmd("Runtime.enable", {})
driver.execute_cdp_cmd("Network.enable", {})

console_logs = []
network_errors = []

def process_cdp_logs():
    """Drain CDP logs and print new entries."""
    try:
        logs = driver.get_log("driver")
    except Exception:
        logs = []
    for entry in logs:
        msg = entry.get("message", "")
        if "console-api" in msg or "Runtime.consoleAPICalled" in msg:
            console_logs.append(msg)
        if "Network.responseReceived" in msg or "Network.loadingFailed" in msg:
            network_errors.append(msg)

try:
    driver.get(URL)
    WebDriverWait(driver, 10).until(
        EC.element_to_be_clickable((By.ID, "btn"))
    )

    # Inject console interceptor
    driver.execute_script("""
        window.__consoleLogs = [];
        window.__netErrors = [];
        var orig = {log: console.log, warn: console.warn, error: console.error};
        ['log','warn','error'].forEach(function(m) {
            console[m] = function() {
                var args = Array.prototype.slice.call(arguments);
                window.__consoleLogs.push('[' + m.toUpperCase() + '] ' + args.join(' '));
                orig[m].apply(console, arguments);
            };
        });
        // Monitor XHR errors
        var origOpen = XMLHttpRequest.prototype.open;
        var origSend = XMLHttpRequest.prototype.send;
        XMLHttpRequest.prototype.open = function(method, url) {
            this.__url = url;
            this.__method = method;
            return origOpen.apply(this, arguments);
        };
        XMLHttpRequest.prototype.send = function() {
            var self = this;
            this.addEventListener('error', function() {
                window.__netErrors.push(self.__method + ' ' + self.__url + ' => NET_ERROR');
            });
            this.addEventListener('load', function() {
                if (self.status >= 400) {
                    window.__netErrors.push(self.__method + ' ' + self.__url + ' => ' + self.status + ' ' + self.statusText);
                }
            });
            return origSend.apply(this, arguments);
        };
    """)

    print("Page loaded, clicking START")
    driver.find_element(By.ID, "btn").click()

    prev_st = ""
    for tick in range(TIMEOUT):
        time.sleep(1)
        st = driver.find_element(By.ID, "st").text
        spd = driver.find_element(By.ID, "spd").text
        lbl = driver.find_element(By.ID, "lbl").text.strip()

        if st != prev_st:
            print(f"  Status: {st}")
            prev_st = st

        if lbl and tick % 3 == 0:
            print(f"  {lbl}: {spd} Mbps")

        # Drain intercepted logs
        clogs = driver.execute_script("var l = window.__consoleLogs.splice(0); return l;")
        for l in (clogs or []):
            print(f"  [CONSOLE] {l}")
        nerrs = driver.execute_script("var l = window.__netErrors.splice(0); return l;")
        for l in (nerrs or []):
            print(f"  [NET ERR] {l}")

        if st == "Complete":
            break

    dl = driver.find_element(By.ID, "dl").text
    ul = driver.find_element(By.ID, "ul").text

    # Final drain
    clogs = driver.execute_script("var l = window.__consoleLogs.splice(0); return l;")
    for l in (clogs or []):
        print(f"  [CONSOLE] {l}")
    nerrs = driver.execute_script("var l = window.__netErrors.splice(0); return l;")
    for l in (nerrs or []):
        print(f"  [NET ERR] {l}")

    print(f"\n=== Results ===")
    print(f"Download: {dl} Mbps")
    print(f"Upload:   {ul} Mbps")

    dl_val = float(dl) if dl != "--" else 0
    ul_val = float(ul) if ul != "--" else 0

    if dl_val < 0.1 or ul_val < 0.1:
        print("\nFAIL: Speed test returned near-zero results")
        sys.exit(1)

    print(f"\nPASS: DL={dl_val:.1f} UL={ul_val:.1f} Mbps")

finally:
    driver.quit()

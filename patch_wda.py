"""Patch only V15 Clean v16's Windows display-affinity initialization."""
from pathlib import Path
import hashlib

source = Path("src/main.cpp")
data = source.read_bytes()
expected = "24b2fa5bec64694cf047451a17da84cee6e4ec0ee91c87942d0d6e39601b83ae"
if hashlib.sha256(data).hexdigest() != expected:
    raise SystemExit("Unexpected baseline; refusing to patch")

old = """        // Fail closed: without capture exclusion the presenter can recursively capture itself.
        if(!SetWindowDisplayAffinity(hwnd,0x00000011))throw Error("WDA_EXCLUDEFROMCAPTURE unsupported",GetLastError());
        // Full-opacity layered click-through overlay: input reaches CS2 underneath.
        if(!SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA))
            throw Error("SetLayeredWindowAttributes failed",GetLastError());
"""

new = """        // Initialize the layered DWM surface BEFORE requesting capture exclusion.
        // On some Windows compositor states, setting affinity on a newly created
        // layered HWND with no surface fails with ERROR_NOT_ENOUGH_MEMORY (8).
        if(!SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA))
            throw Error("SetLayeredWindowAttributes failed",GetLastError());
        // Preserve V15's fail-closed behavior: never capture the presenter itself.
        // First try hidden, as V15 did. If DWM hasn't created the surface yet,
        // briefly attach it to composition and retry once. This does not alter
        // the capture, motion, shader, input, or DXGI presentation paths.
        if(!SetWindowDisplayAffinity(hwnd,0x00000011)) {
            DWORD firstError=GetLastError();
            ShowWindow(hwnd,SW_SHOWNOACTIVATE);
            // A single DwmFlush gives the new top-level HWND a chance to be
            // registered by DWM. We immediately hide it before rendering.
            DwmFlush();
            BOOL excluded=SetWindowDisplayAffinity(hwnd,0x00000011);
            DWORD retryError=excluded?ERROR_SUCCESS:GetLastError();
            ShowWindow(hwnd,SW_HIDE);
            if(!excluded) {
                // Keep the first error for the original V15 error dialog;
                // a separate optional diagnostic can report the retry error.
                (void)firstError;
                throw Error("WDA_EXCLUDEFROMCAPTURE failed after DWM retry",retryError);
            }
        }
"""
decoded = data.decode("utf-8")
if decoded.count(old) != 1:
    raise SystemExit("Original WDA block missing or ambiguous")
fixed = decoded.replace(old, new).encode("utf-8")
new_sha = hashlib.sha256(fixed).hexdigest()
expected_fixed = "4bb76cb492d1d95004636c05fb1161504b45cb796a0c682674da77ab1768ffea"
if new_sha != expected_fixed:
    raise SystemExit(f"Patched source SHA mismatch: {new_sha}")
source.write_bytes(fixed)
print("Verified V15 Clean v16 WDA compatibility source:", new_sha)

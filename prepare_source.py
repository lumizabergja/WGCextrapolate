import base64
import brotli
from pathlib import Path

root = Path('src')
encoded = ''.join((root / f'part{i}.b64').read_text(encoding='ascii').strip() for i in range(1,6))
decoded = brotli.decompress(base64.b64decode(encoded)).decode('utf-8')
decoded = decoded.replace('p.pNVFBCFrameGrabInfo', 'p.pNvFBCFrameGrabInfo')
# Patch the verified baseline with source-derived adaptive extrapolation and UI.
# Based on FG-Fast-Crosshair-Timings-Adaptive source supplied by the user.
import json
import hashlib
base_hash = '79313bb7ab0231955ce5141d47ded9431e56704f52228fe3d074cdc6aa6bd108'
target_hash = 'b6dace8cc6c5f621a5395928927889f01299d43076643e42bfa563f2d1b2d909'
if hashlib.sha256(decoded.encode('utf-8')).hexdigest() != base_hash:
    raise RuntimeError('Baseline source hash mismatch; do not patch an unknown version')
payload = ''.join((root / f'ported-adaptive{i}.b64').read_text(encoding='ascii').strip() for i in range(1,4))
ops = json.loads(brotli.decompress(base64.b64decode(payload)).decode('utf-8'))
source_lines = decoded.splitlines(keepends=True)
for start, stop, replacement in reversed(ops):
    source_lines[start:stop] = [replacement]
decoded = ''.join(source_lines)
actual_hash = hashlib.sha256(decoded.encode('utf-8')).hexdigest()
if actual_hash != target_hash:
    raise RuntimeError(f'Adaptive port reconstruction mismatch: {actual_hash}')
print('Adaptive original-extrapolator DX11 port validated by SHA-256:', actual_hash)
# Native-game-resolution path supersedes the previous 1080p crop/stretch patch.
# Retain the original uploaded extrapolation shader and its validated hash.
out = root / 'main.cpp'
# Preserve LF bytes on Windows so `git apply` and the source checksum agree.
with out.open('w', encoding='utf-8', newline='\n') as native_file:
    native_file.write(decoded)
print(f'Reconstructed {out} ({out.stat().st_size} bytes)')
if out.stat().st_size < 20000:
    raise RuntimeError('Reconstructed C++ source unexpectedly small')
# Apply a source-verified native 4:3 capture/presenter patch.
# NvFBC crops directly into game-sized DX9Ex shared textures.
# NvOFA, extrapolator and DX11 backbuffer stay at that same resolution.
import re
patch_blob = (root / 'native-resolution.patch.br.b64').read_text(encoding='ascii').strip()
patch_text = brotli.decompress(base64.b64decode(patch_blob)).decode('utf-8')
# Exact-content unified diff application in Python. Windows git apply
# rejected this patch even though the source SHA-256 was correct.
source_lines = out.read_text(encoding='utf-8').splitlines()
patch_lines = patch_text.splitlines()
updated = []
cursor = 0
index = 0
while index < len(patch_lines):
    line = patch_lines[index]
    if not line.startswith('@@ '):
        index += 1
        continue
    m = re.match(r'@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@', line)
    if not m:
        raise RuntimeError(f'Invalid native-resolution diff header: {line}')
    first = int(m.group(1)) - 1
    if first < cursor:
        raise RuntimeError('Overlapping native-resolution patch hunks')
    updated.extend(source_lines[cursor:first])
    cursor = first
    index += 1
    while index < len(patch_lines) and not patch_lines[index].startswith('@@ '):
        entry = patch_lines[index]
        index += 1
        marker = entry[:1]
        if marker in (' ', '-'):
            if cursor >= len(source_lines) or source_lines[cursor] != entry[1:]:
                raise RuntimeError(f'Native-resolution patch context mismatch at source line {cursor + 1}')
            if marker == ' ':
                updated.append(source_lines[cursor])
            cursor += 1
        elif marker == '+':
            updated.append(entry[1:])
        elif entry.startswith('\\ No newline at end of file'):
            continue
        else:
            raise RuntimeError(f'Unexpected native-resolution diff line: {entry}')
updated.extend(source_lines[cursor:])
with out.open('w', encoding='utf-8', newline='\n') as native_file:
    native_file.write('\n'.join(updated) + '\n')
native_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_native_hash = '40e95381a7c461e112de787df1c69ad699e5c089cb3df1b53baeeeb8f0b0a1db'
if native_hash != expected_native_hash:
    raise RuntimeError(f'Native game-resolution source mismatch: {native_hash}')
print('Verified native-resolution DX9Ex/DX11 source:', native_hash)

# Keep CS2's 1280x960 input native, while filling the output window
# and tracking display resolution changes as focus returns to CS2.
patch_blob = (root / 'auto-mode-fullscreen.patch.br.b64').read_text(encoding='ascii').strip()
patch_text = brotli.decompress(base64.b64decode(patch_blob)).decode('utf-8')
# Apply final-stage output stretch and monitor-mode synchronization.
# Input textures, optical flow, and extrapolation are unchanged.
source_lines = out.read_text(encoding='utf-8').splitlines()
patch_lines = patch_text.splitlines()
updated = []
cursor = 0
index = 0
while index < len(patch_lines):
    line = patch_lines[index]
    if not line.startswith('@@ '):
        index += 1
        continue
    m = re.match(r'@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@', line)
    if not m:
        raise RuntimeError(f'Invalid native-resolution diff header: {line}')
    first = int(m.group(1)) - 1
    if first < cursor:
        raise RuntimeError('Overlapping native-resolution patch hunks')
    updated.extend(source_lines[cursor:first])
    cursor = first
    index += 1
    while index < len(patch_lines) and not patch_lines[index].startswith('@@ '):
        entry = patch_lines[index]
        index += 1
        marker = entry[:1]
        if marker in (' ', '-'):
            if cursor >= len(source_lines) or source_lines[cursor] != entry[1:]:
                raise RuntimeError(f'Native-resolution patch context mismatch at source line {cursor + 1}')
            if marker == ' ':
                updated.append(source_lines[cursor])
            cursor += 1
        elif marker == '+':
            updated.append(entry[1:])
        elif entry.startswith('\\ No newline at end of file'):
            continue
        else:
            raise RuntimeError(f'Unexpected native-resolution diff line: {entry}')
updated.extend(source_lines[cursor:])
with out.open('w', encoding='utf-8', newline='\n') as native_file:
    native_file.write('\n'.join(updated) + '\n')
fullscreen_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_fullscreen_hash = 'c1b4a53598a6e20e34db3cddec70093326bc59ad48cb9e680b6516d4da872e89'
if fullscreen_hash != expected_fullscreen_hash:
    raise RuntimeError(f'Fullscreen autosync source hash mismatch: {fullscreen_hash}')
print('Verified fullscreen-present/Alt-Tab source:', fullscreen_hash)
# Geometry diagnostic built on exact SHA-validated fullscreen source.
patch_blob = (root / 'geometry-diagnostic.patch.br.b64').read_text(encoding='ascii').strip()
patch_text = brotli.decompress(base64.b64decode(patch_blob)).decode('utf-8')
# Diagnostic magenta border distinguishes output window size from capture content.
source_lines = out.read_text(encoding='utf-8').splitlines()
patch_lines = patch_text.splitlines()
updated = []
cursor = 0
index = 0
while index < len(patch_lines):
    line = patch_lines[index]
    if not line.startswith('@@ '):
        index += 1
        continue
    m = re.match(r'@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@', line)
    if not m:
        raise RuntimeError(f'Invalid native-resolution diff header: {line}')
    first = int(m.group(1)) - 1
    if first < cursor:
        raise RuntimeError('Overlapping native-resolution patch hunks')
    updated.extend(source_lines[cursor:first])
    cursor = first
    index += 1
    while index < len(patch_lines) and not patch_lines[index].startswith('@@ '):
        entry = patch_lines[index]
        index += 1
        marker = entry[:1]
        if marker in (' ', '-'):
            if cursor >= len(source_lines) or source_lines[cursor] != entry[1:]:
                raise RuntimeError(f'Native-resolution patch context mismatch at source line {cursor + 1}')
            if marker == ' ':
                updated.append(source_lines[cursor])
            cursor += 1
        elif marker == '+':
            updated.append(entry[1:])
        elif entry.startswith('\\ No newline at end of file'):
            continue
        else:
            raise RuntimeError(f'Unexpected native-resolution diff line: {entry}')
updated.extend(source_lines[cursor:])
with out.open('w', encoding='utf-8', newline='\n') as native_file:
    native_file.write('\n'.join(updated) + '\n')
geometry_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_geometry_hash = 'dc958fca795cdfee15ccefe0b2c0d84d3b7bdea74e948a3a87f4a24266628410'
if geometry_hash != expected_geometry_hash:
    raise RuntimeError(f'Output geometry diagnostics source mismatch: {geometry_hash}')
print('Validated output-boundary diagnostic:', geometry_hash)
# Capture source fix: NvFBC SCALE full NVIDIA-stretched scanout into native game dimensions.
patch_blob = (root / 'capture-fullscreen-fix.br.b64').read_text(encoding='ascii').strip()
patch_text = brotli.decompress(base64.b64decode(patch_blob)).decode('utf-8')
# Selectable source modes; uploaded extrapolation math remains unchanged.
source_lines = out.read_text(encoding='utf-8').splitlines()
patch_lines = patch_text.splitlines()
updated = []
cursor = 0
index = 0
while index < len(patch_lines):
    line = patch_lines[index]
    if not line.startswith('@@ '):
        index += 1
        continue
    m = re.match(r'@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@', line)
    if not m:
        raise RuntimeError(f'Invalid native-resolution diff header: {line}')
    first = int(m.group(1)) - 1
    if first < cursor:
        raise RuntimeError('Overlapping native-resolution patch hunks')
    updated.extend(source_lines[cursor:first])
    cursor = first
    index += 1
    while index < len(patch_lines) and not patch_lines[index].startswith('@@ '):
        entry = patch_lines[index]
        index += 1
        marker = entry[:1]
        if marker in (' ', '-'):
            if cursor >= len(source_lines) or source_lines[cursor] != entry[1:]:
                raise RuntimeError(f'Native-resolution patch context mismatch at source line {cursor + 1}')
            if marker == ' ':
                updated.append(source_lines[cursor])
            cursor += 1
        elif marker == '+':
            updated.append(entry[1:])
        elif entry.startswith('\\ No newline at end of file'):
            continue
        else:
            raise RuntimeError(f'Unexpected native-resolution diff line: {entry}')
updated.extend(source_lines[cursor:])
with out.open('w', encoding='utf-8', newline='\n') as native_file:
    native_file.write('\n'.join(updated) + '\n')
capture_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_capture_hash = 'c6ca5b292ca8a2c72e4d1f0a79901647b0bc35ada71841a47e70edb0b18730fb'
if capture_hash != expected_capture_hash:
    raise RuntimeError(f'Capture source fixed-source hash mismatch: {capture_hash}')
print('Validated NVIDIA fullscreen source capture:', capture_hash)
# Stable pacing on top of the exact working fullscreen capture source.
# Do not touch NVIDIA source mode, 4:3 mapping, or uploaded FG shader.
patch_blob = (root / 'stutter-fix.patch.br.b64').read_text(encoding='ascii').strip()
patch_text = brotli.decompress(base64.b64decode(patch_blob)).decode('utf-8')
# Selectable source modes; uploaded extrapolation math remains unchanged.
source_lines = out.read_text(encoding='utf-8').splitlines()
patch_lines = patch_text.splitlines()
updated = []
cursor = 0
index = 0
while index < len(patch_lines):
    line = patch_lines[index]
    if not line.startswith('@@ '):
        index += 1
        continue
    m = re.match(r'@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@', line)
    if not m:
        raise RuntimeError(f'Invalid native-resolution diff header: {line}')
    first = int(m.group(1)) - 1
    if first < cursor:
        raise RuntimeError('Overlapping native-resolution patch hunks')
    updated.extend(source_lines[cursor:first])
    cursor = first
    index += 1
    while index < len(patch_lines) and not patch_lines[index].startswith('@@ '):
        entry = patch_lines[index]
        index += 1
        marker = entry[:1]
        if marker in (' ', '-'):
            if cursor >= len(source_lines) or source_lines[cursor] != entry[1:]:
                raise RuntimeError(f'Native-resolution patch context mismatch at source line {cursor + 1}')
            if marker == ' ':
                updated.append(source_lines[cursor])
            cursor += 1
        elif marker == '+':
            updated.append(entry[1:])
        elif entry.startswith('\\ No newline at end of file'):
            continue
        else:
            raise RuntimeError(f'Unexpected native-resolution diff line: {entry}')
updated.extend(source_lines[cursor:])
with out.open('w', encoding='utf-8', newline='\n') as native_file:
    native_file.write('\n'.join(updated) + '\n')
stable_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_stable_hash = '7d6133cff54c80c00fdcf743dcd0da243956058856017a7d9b24f4787f7b4564'
if stable_hash != expected_stable_hash:
    raise RuntimeError(f'Low-stutter pipeline source mismatch: {stable_hash}')
print('Validated stable pacing and asynchronous GPU retire:', stable_hash)
# Dedicated low-latency patch on top of the SHA-verified working baseline.
# Keep original adaptive FG, native capture, 4:3 stretch and Alt+Tab.
patch_blob = (root / 'ultra-low-latency.patch.br.b64').read_text(encoding='ascii').strip()
patch_text = brotli.decompress(base64.b64decode(patch_blob)).decode('utf-8')
# Lowest valid DXGI frame-latency 1, zero-backlog mailbox, late present.
source_lines = out.read_text(encoding='utf-8').splitlines()
patch_lines = patch_text.splitlines()
updated = []
cursor = 0
index = 0
while index < len(patch_lines):
    line = patch_lines[index]
    if not line.startswith('@@ '):
        index += 1
        continue
    m = re.match(r'@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@', line)
    if not m:
        raise RuntimeError(f'Invalid native-resolution diff header: {line}')
    first = int(m.group(1)) - 1
    if first < cursor:
        raise RuntimeError('Overlapping native-resolution patch hunks')
    updated.extend(source_lines[cursor:first])
    cursor = first
    index += 1
    while index < len(patch_lines) and not patch_lines[index].startswith('@@ '):
        entry = patch_lines[index]
        index += 1
        marker = entry[:1]
        if marker in (' ', '-'):
            if cursor >= len(source_lines) or source_lines[cursor] != entry[1:]:
                raise RuntimeError(f'Native-resolution patch context mismatch at source line {cursor + 1}')
            if marker == ' ':
                updated.append(source_lines[cursor])
            cursor += 1
        elif marker == '+':
            updated.append(entry[1:])
        elif entry.startswith('\\ No newline at end of file'):
            continue
        else:
            raise RuntimeError(f'Unexpected native-resolution diff line: {entry}')
updated.extend(source_lines[cursor:])
with out.open('w', encoding='utf-8', newline='\n') as native_file:
    native_file.write('\n'.join(updated) + '\n')
low_latency_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_low_latency_hash = '199747e40229666f6a6f7dbef3fac73ae1cd72190b4a47eeb6e11a556eeee8b9'
if low_latency_hash != expected_low_latency_hash:
    raise RuntimeError(f'Low-latency source mismatch: {low_latency_hash}')
print('Verified low-latency source:', low_latency_hash)
# Minimal UI + fixed NVIDIA-stretched full scanout.
# Keep uploaded extrapolation and low-latency pipeline unchanged.
ui_encoded = (root / 'ui-cleanup-ops.br.b64').read_text(encoding='ascii').strip()
ui_ops = json.loads(brotli.decompress(base64.b64decode(ui_encoded)).decode('utf-8'))
clean_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(ui_ops):
    clean_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as cleaned:
    cleaned.write(''.join(clean_lines))
cleaned_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_cleaned_hash = 'fac9d75303339d6879110d0051b183cd5780ffd9388d60922744a4bfefb3e052'
if cleaned_hash != expected_cleaned_hash:
    raise RuntimeError(f'Clean-controls source mismatch: {cleaned_hash}')
print('Verified clean UI and fixed NVIDIA capture mode:', cleaned_hash)
# Reconstruct the exact FG Cursor-Under source from the verified clean-controls baseline.
# No implicit fallback to a different build when a patch or SHA check fails.
encoded_delta = (root / 'cursor-under-delta.br.b64').read_text(encoding='ascii').strip()
cursor_ops = json.loads(brotli.decompress(base64.b64decode(encoded_delta)).decode('utf-8'))
lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(cursor_ops):
    lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as cursor_build:
    cursor_build.write(''.join(lines))
cursor_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_cursor_hash = '6b209a9d417e17f286acfff087e3338f4fd7ebac4adfc2c0deaff3393be2bac8'
if cursor_hash != expected_cursor_hash:
    raise RuntimeError(f'FG Cursor-Under source mismatch: {cursor_hash}')
print('Verified FG Cursor-Under source:', cursor_hash)
# Menu fix: restore CS2-controlled cursor, and activate full
# cross-process layered click-through while the game menu is open.
menu_patch = (root / 'menu-interaction-fix.br.b64').read_text(encoding='ascii').strip()
menu_ops = json.loads(brotli.decompress(base64.b64decode(menu_patch)).decode('utf-8'))
source_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(menu_ops):
    source_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as menu_fixed:
    menu_fixed.write(''.join(source_lines))
fixed_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_fixed_hash = '36912f42838f72c131e4b2cb5415bd60469943c4c5324b9ec3ba41c1af0c5336'
if fixed_hash != expected_fixed_hash:
    raise RuntimeError(f'CS2 menu interaction source mismatch: {fixed_hash}')
print('Verified CS2 menu click-through fix:', fixed_hash)
# Menu pointer v3: always-layered cross-process click-through +
# separate nonactivating cursor visual, only when visible/forced.
cursor_encoded = (root / 'cursor-visible-menu.br.b64').read_text(encoding='ascii').strip()
cursor_ops = json.loads(brotli.decompress(base64.b64decode(cursor_encoded)).decode('utf-8'))
cursor_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(cursor_ops):
    cursor_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as cursor_visible_build:
    cursor_visible_build.write(''.join(cursor_lines))
cursor_visible_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_visible_hash = '073101fc25c68f974e61271e7224f33715142eb98571857ae70fb88d0743733f'
if cursor_visible_hash != expected_visible_hash:
    raise RuntimeError(f'Visible menu pointer source mismatch: {cursor_visible_hash}')
print('Verified visible CS2 menu cursor source:', cursor_visible_hash)
# v4: Remove CURSOR_SHOWING feedback loop causing repeated arrow
# visibility in gameplay. Use only Escape / manual shortcut edges.
stable_encoded = (root / 'menu-pointer-stable-v4.br.b64').read_text(encoding='ascii').strip()
stable_ops = json.loads(brotli.decompress(base64.b64decode(stable_encoded)).decode('utf-8'))
stable_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(stable_ops):
    stable_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as stable_menu:
    stable_menu.write(''.join(stable_lines))
stable_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_stable_hash = 'c10798331bc3fa94f378ceeebca35a67d4a87d263f05ebdaed9789e03ae7975a'
if stable_hash != expected_stable_hash:
    raise RuntimeError(f'CS2 menu cursor stable source mismatch: {stable_hash}')
print('Verified stable CS2 cursor source:',stable_hash)
# v5: Menu clicks can close CS2 without an Escape keystroke.
# Hide the fallback cursor on click, only rearm when the menu
# demonstrably remains interactive; no global cursor hooks.
click_encoded = (root / 'menu-click-v5.br.b64').read_text(encoding='ascii').strip()
click_ops = json.loads(brotli.decompress(base64.b64decode(click_encoded)).decode('utf-8'))
click_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(click_ops):
    click_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as click_fixed:
    click_fixed.write(''.join(click_lines))
click_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_click_hash = '76e36e20606ba917edca0a38643a3ebb026dbee0b47bf73f75b0ba9276b3a1b1'
if click_hash != expected_click_hash:
    raise RuntimeError(f'CS2 click cursor source mismatch: {click_hash}')
print('Verified CS2 click-return cursor fix:',click_hash)
# v6: remove the synthetic cursor overlay and all menu/click
# heuristics. CS2/Windows owns cursor drawing and mouse input.
native_blob = (root / 'remove-fake-cursor-v6.br.b64').read_text(encoding='ascii').strip()
native_ops = json.loads(brotli.decompress(base64.b64decode(native_blob)).decode('utf-8'))
native_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(native_ops):
    native_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as native_cursor_file:
    native_cursor_file.write(''.join(native_lines))
native_cursor_sha = hashlib.sha256(out.read_bytes()).hexdigest()
expected_native_cursor_sha = 'fdbd06c3f92ac3bca539d21a1a93e447eb3f2fb12c8e2b3629b7f0bce1948982'
if native_cursor_sha != expected_native_cursor_sha:
    raise RuntimeError(f'FG native cursor v6 source mismatch: {native_cursor_sha}')
print('Verified FG native cursor v6 source:', native_cursor_sha)
# v7: preserve DXGI flip-model pipeline but remove legacy layered
# click-through path, and add only center radius/feather controls.
# Both default to zero (protection disabled); no HUD bands.
center_encoded = (root / 'centeronly-v7-patch.br.b64').read_text(encoding='ascii').strip()
center_ops = json.loads(brotli.decompress(base64.b64decode(center_encoded)).decode('utf-8'))
center_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(center_ops):
    center_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as center_source:
    center_source.write(''.join(center_lines))
center_sha = hashlib.sha256(out.read_bytes()).hexdigest()
expected_center_sha = '04a4863f50d08b3a48ffd3cf2ac3465e1777dd9a620e6da0ae8339657b401b7c'
if center_sha != expected_center_sha:
    raise RuntimeError(f'FG center-only v7 SHA mismatch: {center_sha}')
print('Verified FG center-only v7 source:', center_sha)
# v8: restore reliable cross-process input pass-through to CS2.
# Keep existing flip-discard swapchain and center-only shader.
pass_blob = (root / 'menu-input-v8.patch.br.b64').read_text(encoding='ascii').strip()
pass_ops = json.loads(brotli.decompress(base64.b64decode(pass_blob)).decode('utf-8'))
pass_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(pass_ops):
    pass_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as pass_source:
    pass_source.write(''.join(pass_lines))
pass_sha = hashlib.sha256(out.read_bytes()).hexdigest()
expected_pass_sha = 'dfc818864a00156ccf2d94cd788c7bec52eb4456bb7c40aa39a3e9a4a675283d'
if pass_sha != expected_pass_sha:
    raise RuntimeError(f'FG v8 menu input source mismatch: {pass_sha}')
print('Verified FG v8 mouse pass-through source:', pass_sha)
# v9: revert the presenter window and message procedure exactly
# to the v6 source, preserving v7/v8 center-only controls.
v6_encoded = (root / 'restore-v6-presenter-v9.br.b64').read_text(encoding='ascii').strip()
v6_ops = json.loads(brotli.decompress(base64.b64decode(v6_encoded)).decode('utf-8'))
v6_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for start, stop, replacement in reversed(v6_ops):
    v6_lines[start:stop] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as v6_presenter_file:
    v6_presenter_file.write(''.join(v6_lines))
v6_source_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_v6_source_hash = '9abbfd3f2afddf33c350e90d3dcdf4a0a8fe2df89f750e79b1bfe82f2447f35b'
if v6_source_hash != expected_v6_source_hash:
    raise RuntimeError(f'FG v9 source mismatch: {v6_source_hash}')
print('Verified v6 presenter restored in FG v9:', v6_source_hash)
# v10: real-frame-first output with direct GPU shared textures.
# Retain the v6/v9 window and all existing NvFBC/NvOFA resources.
direct_gpu_patch = (root / 'directgpu-realfirts-v10.br.b64').read_text(encoding='ascii').strip()
direct_gpu_ops = json.loads(brotli.decompress(base64.b64decode(direct_gpu_patch)).decode('utf-8'))
direct_gpu_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(direct_gpu_ops):
    direct_gpu_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as direct_gpu_source:
    direct_gpu_source.write(''.join(direct_gpu_lines))
direct_gpu_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_direct_gpu_hash = 'ab2a83dfe1144c0bd162431452eaaa34241efed1ed238ebfbb7b2522f1c9bb7c'
if direct_gpu_hash != expected_direct_gpu_hash:
    raise RuntimeError(f'FG v10 direct-GPU source mismatch: {direct_gpu_hash}')
print('Verified FG v10 direct GPU real-first:', direct_gpu_hash)
# v11: event-driven capture slot and frame-ready wakeups.
# No changes to renderer, HWND, swapchain, OFA or crosshair controls.
event_blob = (root / 'event-wake-v11.br.b64').read_text(encoding='ascii').strip()
event_ops = json.loads(brotli.decompress(base64.b64decode(event_blob)).decode('utf-8'))
event_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(event_ops):
    event_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as event_source:
    event_source.write(''.join(event_lines))
event_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_event_hash = '20408558e0e3c4bfffde7a365ea2ca82c09e339e09787d6bb9ff05864af89a2e'
if event_hash != expected_event_hash:
    raise RuntimeError(f'FG v11 source mismatch: {event_hash}')
print('Verified FG v11 event-driven capture:', event_hash)
# v14: exact v11 source + typed NvFBC recreate error and full
# GPU-session teardown/rebuild, not the v12/v13 recovery branches.
clean_patch = (root / 'v11-full-recreate-v14.br.b64').read_text(encoding='ascii').strip()
clean_ops = json.loads(brotli.decompress(base64.b64decode(clean_patch)).decode('utf-8'))
clean_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(clean_ops):
    clean_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as clean_source:
    clean_source.write(''.join(clean_lines))
clean_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_clean_hash = '4ff5a4ed2b748b2e9a6f4d5c22c687f52663743ac5a0fc9b5d7839b8c8fe7d65'
if clean_hash != expected_clean_hash:
    raise RuntimeError(f'FG v14 v11-restored source mismatch: {clean_hash}')
print('Verified clean v11 session-recreation v14:', clean_hash)
# v15: keep all v11 rendering unchanged; restore restart after a
# captured game's exit using bounded NVFBC grab waits and a GUI
# lifecycle check that unlocks Start only once teardown completes.
restart_encoded = (root / 'v14-restart-v15.br.b64').read_text(encoding='ascii').strip()
restart_ops = json.loads(brotli.decompress(base64.b64decode(restart_encoded)).decode('utf-8'))
restart_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(restart_ops):
    restart_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as restart_source:
    restart_source.write(''.join(restart_lines))
restart_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_restart_hash = '1db87cc1b1e9c1f61075d095ad668ac7d76bde13a9714191a5ccc4881abe4b22'
if restart_hash != expected_restart_hash:
    raise RuntimeError(f'FG v15 source mismatch: {restart_hash}')
print('Verified FG v15 game-exit restart:', restart_hash)
# v16: cleanup only, preserving exact shader / HWND / DXGI behavior.
cleanup_blob = (root / 'v15-cleanup-v16.br.b64').read_text(encoding='ascii').strip()
cleanup_ops = json.loads(brotli.decompress(base64.b64decode(cleanup_blob)).decode('utf-8'))
cleanup_lines = out.read_text(encoding='utf-8').splitlines(keepends=True)
for first, last, replacement in reversed(cleanup_ops):
    cleanup_lines[first:last] = [replacement]
with out.open('w', encoding='utf-8', newline='\n') as cleaned_source:
    cleaned_source.write(''.join(cleanup_lines))
cleanup_hash = hashlib.sha256(out.read_bytes()).hexdigest()
expected_cleanup_hash = '24b2fa5bec64694cf047451a17da84cee6e4ec0ee91c87942d0d6e39601b83ae'
if cleanup_hash != expected_cleanup_hash:
    raise RuntimeError(f'FG v16 clean source mismatch: {cleanup_hash}')
print('Verified FG v16 cleanup:', cleanup_hash)

# Additional isolated NvFBC community compatibility experiment.
import zlib
original = out.read_bytes()
base_sha = "24b2fa5bec64694cf047451a17da84cee6e4ec0ee91c87942d0d6e39601b83ae"
target_sha = "fb445337af61c569883a67b83fa27587387745425c02775bd1165476edbeecee"
if hashlib.sha256(original).hexdigest() != base_sha:
    raise RuntimeError("Refusing NvFBC community patch on non-V15-v16 source")
ops = json.loads(zlib.decompress(base64.b64decode((root / "community_delta.b64").read_text(encoding="ascii"))))
lines = original.decode("utf-8").splitlines(keepends=True)
for i, j, replacement in sorted(ops, key=lambda v: v[0], reverse=True):
    lines[i:j] = [replacement] if replacement else []
patched = "".join(lines).encode("utf-8")
h = hashlib.sha256(patched).hexdigest()
if h != target_sha:
    raise RuntimeError(f"NvFBC community source failed SHA256 validation: {h}")
out.write_bytes(patched)
print("V15 community NvFBC source verified:", h)

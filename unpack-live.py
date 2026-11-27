from pathlib import Path
import base64, hashlib, zipfile, subprocess, sys, shutil
root = Path(__file__).resolve().parent
data = base64.b64decode((root/'livewarp-patch.b64').read_text(), validate=True)
if hashlib.sha256(data).hexdigest() != 'b0955566801a1fdcb2f06870591f7f38052fcd1046aa638428a3daa4d51d4fa1':
    raise SystemExit('Live integration patch checksum mismatch')
(root/'livewarp-patch.zip').write_bytes(data)
p = root/'livewarp-patch'
with zipfile.ZipFile(root/'livewarp-patch.zip') as z:
    assert z.testzip() is None
    z.extractall(p)
subprocess.run([sys.executable, str(p/'apply.py'), str(root/'fg')],check=True)
for name in ('README.md','CI-Build.cmd'):
    shutil.copyfile(p/name,root/'fg'/name)
print('Live NGX real-frame integration source prepared and hash verified.')

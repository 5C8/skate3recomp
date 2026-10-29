"""Fetch the matching optional VR runtime from this project's release."""
from pathlib import Path
import hashlib,io,urllib.request,zipfile
url='https://github.com/5C8/skate3recomp/releases/download/v1.0.0-skate3.20261029/OpenVR-Windows-x64.zip'
expected='36c6ef46c8b9e8b6b7614f37edb16a6d4b191d93a5d64f567bd829242001e9fa'
with urllib.request.urlopen(url,timeout=120) as response:
    archive=zipfile.ZipFile(io.BytesIO(response.read()))
data=archive.read('openvr_api.dll')
if hashlib.sha256(data).hexdigest()!=expected:raise SystemExit('OpenVR checksum mismatch')
target=Path(__file__).resolve().parents[1]/'thirdparty/openvr/openvr_api.dll'
target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
print('OpenVR runtime ready:',target)

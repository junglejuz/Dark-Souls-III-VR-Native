# Dark Souls III VR (native stereo) release package (PACKAGING_GUIDE.md, made like batman_asylum/tools/make_package.py):
# builds dist/DS3VR-Native-DarkSoulsIII.zip from one source of truth per file - the mod from build/Release (refused unless
# it is the build JJ tested), NVIDIA's DLSS library from the game's DS3VR2 folder (the copy the mod ran with), the
# installer and launcher from install/, the tested VR settings from JJ's live DS3VR2\settings.ini minus his headset's
# own size (the mod reads every headset's at its first start). Zip made with zipfile: forward slashes, no top folder
# (Windows' "Extract All" rejects backslash entries). NVIDIA's DLSS library ships in the release zip only - never in git.
#   python tools/make_package.py [--settings "<settings.ini>"] [--dll-sha256 <prefix>]
import argparse, hashlib, os, re, shutil, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VERSION = '2.0.0'
GAME_DATA = r'E:\Games\Steam\steamapps\common\DARK SOULS III\Game\DS3VR2'
ap = argparse.ArgumentParser()
ap.add_argument('--settings', default=os.path.join(GAME_DATA, 'settings.ini'))
ap.add_argument('--dll-sha256', default='428A0FF173C3', help='start of the tested build\'s sha256 (JJ, 2026-11-01 12:19)')
a = ap.parse_args()

dist = os.path.join(ROOT, 'dist')
stage = os.path.join(dist, 'DS3VR-Native')
zpath = os.path.join(dist, 'DS3VR-Native-DarkSoulsIII.zip')
if os.path.isdir(stage): shutil.rmtree(stage)
os.makedirs(os.path.join(stage, 'files', 'DS3VR2'))
h = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest().upper()

def put(src, rel):
    dst = os.path.join(stage, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)

inst = os.path.join(ROOT, 'install')
for f in ('Install-DS3VR-Native.bat', 'Install-DS3VR-Native.ps1', 'Uninstall-DS3VR-Native.bat', 'Uninstall-DS3VR-Native.ps1', 'Dark Souls III VR.bat'):
    put(os.path.join(inst, f), f)
put(os.path.join(ROOT, 'build', 'Release', 'dinput8.dll'), 'dinput8.dll')
dd = os.path.join('files', 'DS3VR2')
put(os.path.join(GAME_DATA, 'nvngx_dlss.dll'), os.path.join(dd, 'nvngx_dlss.dll'))
for f in ('LICENSE-NVIDIA-DLSS.txt', 'LICENSE-bioshock-vr.txt', 'LICENSE-luma-framework.md', 'LICENSE-MinHook.txt',
          'LICENSE-ImGui.txt', 'LICENSE-OpenXR.txt', 'THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES-bioshock-vr.md'):
    put(os.path.join(ROOT, f), os.path.join(dd, f))
open(os.path.join(stage, dd, 'version.txt'), 'w', newline='\n').write(VERSION + '\n')
# the tested settings, without JJ's headset size / lens (every headset's own are read at its first start) and without the
# empty texture-hide slots
lines = open(a.settings, encoding='latin-1').read().splitlines()
keep = [l for l in lines if not re.match(r'(?i)^(xr_rec_w|xr_rec_h|xr_ang|runtime_json|hud_hide_\d+)=', l)]
open(os.path.join(stage, 'files', 'settings.ini'), 'w', encoding='latin-1', newline='').write('\r\n'.join(keep) + '\r\n')
for f in ('README.md', 'THIRD_PARTY_NOTICES.md'):
    put(os.path.join(ROOT, f), f)

# checks: batch files CRLF, the DLL is ours and the tested build, the settings are the tested ones (no test switches)
bad = []
for dp, _, fs in os.walk(stage):
    for f in fs:
        if f.lower().endswith('.bat'):
            b = open(os.path.join(dp, f), 'rb').read()
            if b.count(b'\n') != b.count(b'\r\n'): bad.append(f + ': not CRLF')
dll = os.path.join(stage, 'dinput8.dll')
if b'DS3VR2-M' not in open(dll, 'rb').read(): bad.append('dinput8.dll is not ours')
if not h(dll).startswith(a.dll_sha256.upper()): bad.append('dinput8.dll is not the tested build (sha256 %s)' % h(dll)[:12])
st = open(os.path.join(stage, 'files', 'settings.ini'), encoding='latin-1').read()
for need in ('[ds3vr2]', 'dlss_mode=3', 'lens=1', 'cull_fix=1', 'hud_hidevs_0=', 'stereo3d=1'):
    if need not in st: bad.append('settings.ini lacks ' + need)
if re.search(r'(?im)^(auto_rec|self_check|mono|afw)\s*=\s*1', st): bad.append('settings.ini has a test switch on')
if 'xr_rec_w' in st: bad.append('settings.ini still holds a headset size')
if bad:
    print('REFUSED:', *bad, sep='\n  '); sys.exit(1)

if os.path.exists(zpath): os.remove(zpath)
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for dp, _, fs in os.walk(stage):
        for f in sorted(fs):
            full = os.path.join(dp, f)
            arc = os.path.relpath(full, stage).replace(os.sep, '/')
            z.write(full, arc, zipfile.ZIP_STORED if f.lower().endswith(('.dll', '.exe')) else zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(zpath) as z:
    assert z.testzip() is None
    names = z.namelist()
    assert not any('\\' in n for n in names), 'backslash in an entry name'
for n in names: print('  ', n)
print(f'{zpath}: {len(names)} files, {os.path.getsize(zpath) / 1e6:.1f} MB, sha256 {h(zpath)}')

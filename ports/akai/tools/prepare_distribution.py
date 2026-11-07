#!/usr/bin/env python3
"""Stage only the current, explicitly paired device releases for sharing."""
import hashlib,json,shutil,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT.parent/'DISTRIBUTION'
PAIRS=[dict(device='MPC One+',plugin='0.3.6',helper='0.2.7',source=ROOT/'dist',
 files=['RetroLooper-One-0.3.6-mpc-armv7.zip','RetroLooper-MPC-Buttons-0.2.7-mpc-armv7.zip'],
 ids=['retrolooper-one','retrolooper-buttons'],uid='526c4f31',
 build_id='bc141062ea5ea5be619d98366f423c8102574365',status='Hardware tested on MPC One+ 3.10.0.49',
 plugin_sha='bc625d472d8566926d89e506cdb640e5325c3d28a260178d048f5a6d2a1c1f08',
 helper_sha='1dc3f1f035ef89e1bb633009bdce3738ca39e98274f3d2cd40b10839ecc659d6'),
 dict(device='Akai Force',plugin='0.1.0',helper='0.1.0',source=ROOT/'force/dist',
 files=['RetroLooper-Force-0.1.0-mpc-armv7.zip','RetroLooper-Force-Controls-0.1.0-mpc-armv7.zip'],
 ids=['retrolooper-force','retrolooper-force-controls'],uid='526c4631',
 build_id='154ae9752e0f3c6490912a703b0dfe4b668b343d',status='HARDWARE TEST BUILD: installed and startup verified on the provided Force; isolated audio/helper tests passed; live Arrange timing, audio routing and pad actions pending')]
def package_info(path):
 with zipfile.ZipFile(path) as z:
  assert not z.testzip(),path
  manifests=[json.loads(z.read(n)) for n in z.namelist() if n.endswith('/mpc-plugin.json')]
  assert manifests and all(m==manifests[0] for m in manifests),path
  m=manifests[0];binaries=[n for n in z.namelist() if n.endswith('/'+m['so'])];assert len(binaries)==1,(path,binaries)
  return m,hashlib.sha256(z.read(binaries[0])).hexdigest()
def main():
 OUT.mkdir(exist_ok=True)
 for pair in PAIRS:
  dest=OUT/pair['device'];dest.mkdir(exist_ok=True);records=[]
  # Reject ambiguous stale installers instead of silently leaving them beside
  # a newly paired release. This script never deletes existing user files.
  extras=[p.name for p in dest.glob('*.zip') if p.name not in pair['files']]
  assert not extras,('Archive old distribution ZIPs before updating',dest,extras)
  for index,name in enumerate(pair['files']):
   source=pair['source']/name;m,so_sha=package_info(source)
   assert m['id']==pair['ids'][index] and m['arch']=='armv7'
   assert m['version']==pair['plugin' if index==0 else 'helper']
   assert m['kind']==('effect' if index==0 else 'addin')
   if index==0:assert m['uid']==pair['uid']
   expected=pair.get('plugin_sha' if index==0 else 'helper_sha')
   if pair['device']=='Akai Force':
    validation=json.loads((ROOT/'force/diagnostics/VALIDATION.json').read_text())
    expected=validation['production_so_sha256' if index==0 else 'helper_so_sha256']
   if expected:assert so_sha==expected,(name,so_sha,expected)
   shutil.copy2(source,dest/name)
   records.append(dict(file=name,kind=m['kind'],id=m['id'],version=m['version'],sha256=hashlib.sha256(source.read_bytes()).hexdigest(),binary_sha256=so_sha))
  info=dict(device=pair['device'],status=pair['status'],host_build_id=pair['build_id'],packages=records)
  (dest/'RELEASE.json').write_text(json.dumps(info,indent=2)+'\n')
  title=f"RetroLooper — {pair['device']}"
  details=('Force automatic length follows the global Arrange loop, with /2, x1 and x2. The looper controls use the bottom two pad rows only while its editor is visible. Native modes retain the full grid when the editor closes.\n\n' if pair['device']=='Akai Force' else 'This is the working MPC One+ release. Bank A controls the looper in the supported editor/MAIN contexts and yields to native Track Mute, Pad Mute and other performance modes.\n\n')
  (dest/'START HERE.md').write_text(f'''# {title}

**{pair['status']}**

Plugin {pair['plugin']} + integration helper {pair['helper']}.

## Install the matching pair

1. Save your device project. Installation of the helper requires an application restart.
2. Use a community installer that supports portable plugin packages and addin packages. Import **both ZIPs from this folder**. Keep their filenames and contents intact.
3. Follow the installer prompts. The plugin is an Insert Effect; the helper runs in the background and is not a second instrument/effect.
4. After the required restart, add RetroLooper {'Force' if pair['device']=='Akai Force' else 'One'} to an Audio track's insert chain. Place effects you want to record **before** the looper.

Only use this pair on **{pair['device']}**, with the matching host firmware. The helper is pinned to host GNU build ID `{pair['build_id']}`. Other builds require separate validation. The `mpc-armv7` filename suffix describes the community package architecture; it does not make the Force and MPC helpers interchangeable.

{details}The looper's touchscreen/pads control recording and overdubbing; the host controls the clock and global transport. Capture preparation disarms the native Audio track to avoid a duplicate timeline recording and prepares the selected track's live input.

WAV export writes an audio file. Automatic placement onto the native Audio timeline is **not implemented** in these packages.

`RELEASE.json` identifies both components and their binary hashes. `SHA256SUMS` verifies the distribution files. The ZIPs include the standard install/uninstall scripts and supporting license notices. Development sources and diagnostics are outside this distribution folder.
''')
  if pair['device']=='Akai Force':
   shutil.copy2(ROOT/'force/README.md',dest/'FORCE TEST STATUS.md')
   shutil.copy2(ROOT/'force/diagnostics/VALIDATION.json',dest/'VALIDATION.json')
  checks=[]
  for path in sorted(dest.iterdir()):
   if path.is_file() and path.name!='SHA256SUMS':checks.append(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.name)
  (dest/'SHA256SUMS').write_text('\n'.join(checks)+'\n')
  print('PASS:',dest,'—',len(records),'matched, verified packages')
 (OUT/'START HERE.md').write_text('''# RetroLooper device distribution

Choose the folder matching your hardware:

- **MPC One+/** — RetroLooper One 0.3.6 + MPC Buttons/Track helper 0.2.7. Hardware tested.
- **Akai Force/** — RetroLooper Force 0.1.0 + Force Controls helper 0.1.0. Hardware test build; installed on the provided Force, with live validation pending.

Each folder contains only its matching pair of installer ZIPs, installation notes, release metadata and checksums. Send the complete device folder when distributing. Do not mix helper packages between devices. Both components support one RetroLooper insert effect; the integration helper is not another plugin in the browser.

Force and MPC packages both use the community `mpc-armv7` archive suffix. The device folder, product name, unique plugin ID and helper ID identify the correct target.
''')
if __name__=='__main__':main()

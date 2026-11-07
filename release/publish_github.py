#!/usr/bin/env python3
"""Publish this repository's already validated, device-separated release artifacts."""
from pathlib import Path
import hashlib,json,os,subprocess,urllib.request,urllib.error,urllib.parse,zipfile
ROOT=Path(__file__).resolve().parents[1]
REPO='asearan/Retrolooper-One-MPC-Force'
API='https://api.github.com/repos/'+REPO

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,text=True).strip()
def main():
 assert os.environ.get('GITHUB_ACTIONS')=='true','Run via the repository publication workflow'
 assert os.environ.get('GITHUB_REPOSITORY')==REPO
 token=os.environ['GH_TOKEN']
 def request(method,path,data=None,binary=None):
  body=binary if binary is not None else json.dumps(data).encode() if data is not None else None
  url=path if path.startswith('https://') else API+path
  headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json','X-GitHub-Api-Version':'2022-11-28','User-Agent':'RetroLooper-Release','Content-Type':'application/octet-stream' if binary is not None else 'application/json'}
  try:
   with urllib.request.urlopen(urllib.request.Request(url,data=body,headers=headers,method=method),timeout=90) as r:return json.load(r)
  except urllib.error.HTTPError as e:
   if e.code==404:return None
   raise RuntimeError(f'GitHub {method} {url}: HTTP {e.code}') from None
 manifest=json.loads((ROOT/'SOURCE-MANIFEST.json').read_text())
 for rel,digest in manifest['files'].items():assert sha(ROOT/rel)==digest,rel
 generated=[]
 for device,name in [('MPC One+','RetroLooper-One-MPC-OnePlus-0.3.6-complete.zip'),('Force','RetroLooper-Force-0.1.0-complete.zip')]:
  folder=ROOT/'releases'/device;info=json.loads((folder/'RELEASE.json').read_text());files=[r['file'] for r in info['packages']]+['README.md','RELEASE.json','SHA256SUMS']
  for record in info['packages']:
   p=folder/record['file'];assert sha(p)==record['sha256'];
   with zipfile.ZipFile(p) as z:
    assert not z.testzip();m=json.loads(z.read(next(n for n in z.namelist() if n.endswith('/mpc-plugin.json'))));b=z.read(next(n for n in z.namelist() if n.endswith('/'+m['so'])));assert hashlib.sha256(b).hexdigest()==record['binary_sha256']
  with zipfile.ZipFile(folder/name,'w',zipfile.ZIP_DEFLATED) as z:
   for filename in files:
    entry = zipfile.ZipInfo(f'{device}/{filename}')
    z.writestr(entry, (folder / filename).read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
  generated.append(str((folder/name).relative_to(ROOT)))
 git('config','user.name','github-actions[bot]');git('config','user.email','41898282+github-actions[bot]@users.noreply.github.com')
 git('add','--',*manifest['files'].keys(),'SOURCE-MANIFEST.json',*generated)
 if subprocess.run(['git','diff','--cached','--quiet'],cwd=ROOT).returncode:
  git('commit','-m','Publish RetroLooper source, paired device releases, documentation and catalog artwork')
  git('push','origin','HEAD:main')
 commit=git('rev-parse','HEAD')
 releases=[dict(tag='mpc-controls-v0.2.7',name='RetroLooper One Controls 0.2.7 — MPC',beta=False,files=['MPC One+/RetroLooper-MPC-Buttons-0.2.7-mpc-armv7.zip'],body='Matching background integration for RetroLooper One 0.3.6 on MPC One+ 3.10.0.49. Installed automatically by the catalog dependency resolver. This is not a second insert effect.'),
 dict(tag='force-v0.1.0',name='RetroLooper One 0.1.0 — Force (experimental)',beta=True,files=['Force/RetroLooper-Force-0.1.0-mpc-armv7.zip','Force/RetroLooper-Force-Controls-0.1.0-mpc-armv7.zip','Force/RetroLooper-Force-0.1.0-complete.zip'],body='EXPERIMENTAL Force-only pair. Prior hardware-test build passed isolated audio/helper tests, installation/startup and native readiness. This release rebuilds the effect display name as RetroLooper One; the curated source host suite and skin checks pass. Live Arrange timing, audio routing, editor actions and lower-row pad behaviour remain pending. Never exchange the MPC and Force helpers. The complete archive contains both standard packages, README and checksums.'),
 dict(tag='mpc-v0.3.6',name='RetroLooper One 0.3.6 — MPC One+',beta=False,files=['MPC One+/RetroLooper-One-0.3.6-mpc-armv7.zip','MPC One+/RetroLooper-One-MPC-OnePlus-0.3.6-complete.zip'],body='Hardware-tested MPC One+ 3.10.0.49 release. Stereo audio-track looping/overdubbing with host sequence sync, threshold start, reverse, filters, waveform/cassette/meter views and coloured pad controls. Opening an instance selects its Audio track and matching grid; native Track Mute/Pad Mute retain their pads. The catalog automatically adds the exact matching integration helper 0.2.7 to the same batch. For direct installation, use the complete archive. WAV export writes a file; automatic native timeline placement is not implemented. Force downloads are a separate experimental release.')]
 for spec in releases:
  release=request('GET','/releases/tags/'+spec['tag'])
  if not release:
   release=request('POST','/releases',dict(tag_name=spec['tag'],target_commitish=commit,name=spec['name'],body=spec['body']+'\n\n[Installation and build instructions](https://github.com/'+REPO+'/blob/'+commit+'/README.md)\n\nCopyright © 2026 IAMTHEBAND LLC, Retroloop USA.\n\nGPL-3.0-only; corresponding source and third-party notices are in this tag. No proprietary host firmware is included.',draft=False,prerelease=spec['beta']))
  existing={a['name']:a for a in release.get('assets',[])}
  for rel in spec['files']:
   p=ROOT/'releases'/rel;digest='sha256:'+sha(p)
   if p.name in existing:
    assert existing[p.name].get('digest')==digest,'Existing release asset differs; preserving it';continue
   upload=release['upload_url'].split('{')[0]+'?name='+urllib.parse.quote(p.name)
   asset=request('POST',upload,binary=p.read_bytes());assert asset.get('size')==p.stat().st_size
   print('Uploaded',spec['tag'],p.name,flush=True)
  print('Published',release['html_url'],flush=True)
 print('Source commit:',commit,flush=True)
if __name__=='__main__':main()

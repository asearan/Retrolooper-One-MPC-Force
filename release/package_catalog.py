#!/usr/bin/env python3
"""Build separate catalog packages with exact matching helper dependencies."""
from pathlib import Path
import argparse,hashlib,json,shlex,shutil,subprocess,sys,tempfile,zipfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE_REPO='asearan/Retrolooper-One-MPC-Force'
SPDX='GPL-3.0-only'
def run(a):subprocess.run([str(x) for x in a],check=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--kit',type=Path,required=True);p.add_argument('--output',type=Path,default=ROOT/'releases');p.add_argument('--mpc-build',type=Path,default=ROOT/'ports/akai');p.add_argument('--force-build',type=Path,default=ROOT/'ports/akai/force/workspace');p.add_argument('--mpc-helper',type=Path);p.add_argument('--force-helper',type=Path);a=p.parse_args()
 sys.path.insert(0,str(a.kit/'tools'));from catalog_check import parse_addin_manifest
 pairs=[dict(device='MPC One+',name='RetroLooper One',version='0.3.6',helper_version='0.2.7',helper_name='RetroLooper MPC Buttons',helper_id='retrolooper-buttons',id='retrolooper-one',root=a.mpc_build,helper=a.mpc_helper or a.mpc_build/'control',so='retrolooper_one.so',needs='MPC One+ with community plugins and modified MPC 3.10.0.49; host build bc141062ea5ea5be619d98366f423c8102574365. The matching helper is installed automatically.',about='Single-track stereo audio overdubbing, host sequence sync, threshold start, reverse, filters and coloured pad controls.',docs='MPC-ONE-PLUS.md',complete='RetroLooper-One-MPC-OnePlus-0.3.6-complete.zip',status='Hardware tested on MPC One+ 3.10.0.49'),
 dict(device='Force',name='RetroLooper One',folder_name='RetroLooper Force',asset_name='RetroLooper-Force',version='0.1.0',helper_version='0.1.0',helper_name='RetroLooper Force Controls',helper_id='retrolooper-force-controls',id='retrolooper-force',root=a.force_build,helper=a.force_helper or a.force_build/'control',so='retrolooper_force.so',needs='EXPERIMENTAL: Force 3.10, host build 154ae9752e0f3c6490912a703b0dfe4b668b343d. Live Arrange/audio/pad functional validation pending. Never use MPC helper packages.',about='Experimental Force port: Arrange-loop sync, stereo overdubbing and editor-scoped lower-row controls; functional hardware validation pending.',docs='FORCE.md',complete='RetroLooper-Force-0.1.0-complete.zip',status='Experimental hardware test release; live functional validation pending')]
 for pair in pairs:
  out=a.output/pair['device'];out.mkdir(parents=True,exist_ok=True)
  skin=pair['root']/('build/skin/RetroLooper - VST - '+pair.get('folder_name',pair['name']));entry=pair['root']/'build/pluginlist-entry.xml';so=pair['root']/'build'/pair['so']
  common=['--repo',SOURCE_REPO,'--license',SPDX,'--requires',pair['needs']]
  args=[sys.executable,a.kit/'tools/release.py','--so',so,'--skin',skin,'--entry',entry,'--version',pair['version'],'--about',pair['about'],*common,'--depends',pair['helper_id']+'='+pair['helper_version'],'--user-data','Loops','-o',out]
  if pair['device']=='Force':args+=['--user-data','Exports','--folder-name',pair['folder_name'],'--id',pair['id']]
  extras=[(ROOT/'LICENSE','licenses/RetroLooper-GPL-3.0.txt'),(ROOT/'COPYRIGHT','licenses/RetroLooper-COPYRIGHT.txt'),(ROOT/'THIRD_PARTY_NOTICES.md','licenses/THIRD_PARTY_NOTICES.md'),(ROOT/'docs'/pair['docs'],'README.md'),(ROOT/'ports/akai/vendor/mpc-vst-plugins-LICENSE','licenses/mpc-vst-plugins-LICENSE')]
  for n in ['signalsmith-stretch','signalsmith-linear']:extras.append((ROOT/'third_party'/n/'LICENSE.txt','licenses/'+n+'-LICENSE.txt'))
  for n in ['Inter','PressStart2P','VT323']:extras.append((ROOT/'licenses'/(n+'-OFL.txt'),'licenses/'+n+'-OFL.txt'))
  for src,dst in extras:args+=['--extra',str(src)+':'+dst]
  run(args)
  with tempfile.TemporaryDirectory(prefix='retrolooper-helper-') as temp:
   stage=Path(temp);text=(pair['helper']/'addin.manifest').read_text();m=parse_addin_manifest(text);text=text.replace(m['ADDIN_NAME'],'RetroLooper One Controls ('+pair['device']+')').replace('Opening RetroLooper Force','Opening RetroLooper One')
   for name in [m['ADDIN_SO'],*m.get('ADDIN_FILES','').split()]:shutil.copy2(pair['helper']/name,stage/name)
   text=text.replace("ADDIN_FILES='"+m['ADDIN_FILES']+"'","ADDIN_FILES='"+m['ADDIN_FILES']+" project-LICENSE COPYRIGHT THIRD-PARTY-NOTICES.txt'")
   (stage/'addin.manifest').write_text(text);shutil.copy2(ROOT/'LICENSE',stage/'project-LICENSE');shutil.copy2(ROOT/'COPYRIGHT',stage/'COPYRIGHT');shutil.copy2(ROOT/'THIRD_PARTY_NOTICES.md',stage/'THIRD-PARTY-NOTICES.txt')
   run([sys.executable,a.kit/'tools/release_addin.py','--dir',stage,'--version',pair['helper_version'],'--about','Matching background integration for '+pair['name']+'; installed automatically with the effect.',*common,'-o',out])
  expected_helper=out/(pair['helper_name'].replace(' ','-')+'-'+pair['helper_version']+'-mpc-armv7.zip')
  candidates=[p for p in out.glob('*-mpc-armv7.zip') if p.name.startswith('RetroLooper-One-Controls-')]
  assert len(candidates)==1,candidates
  candidates[0].replace(expected_helper)
  plugin_name=pair.get('asset_name',pair['name'].replace(' ','-'))+'-'+pair['version']+'-mpc-armv7.zip';helper_name=pair['helper_name'].replace(' ','-')+'-'+pair['helper_version']+'-mpc-armv7.zip'
  records=[]
  for name,pid in [(plugin_name,pair['id']),(helper_name,pair['helper_id'])]:
   zpath=out/name;run([sys.executable,a.kit/'tools/catalog_check.py',zpath,'--catalog','--require-portable','--expect-id',pid,'--expect-repo',SOURCE_REPO])
   with zipfile.ZipFile(zpath) as z:
    ms=[json.loads(z.read(n)) for n in z.namelist() if n.endswith('/mpc-plugin.json')];m=ms[0];assert all(n==m for n in ms);b=next(n for n in z.namelist() if n.endswith('/'+m['so']));binary_sha=hashlib.sha256(z.read(b)).hexdigest()
   records.append(dict(file=name,id=pid,version=m['version'],kind=m['kind'],sha256=sha(zpath),binary_sha256=binary_sha,depends=m.get('depends',[])))
  (out/'RELEASE.json').write_text(json.dumps(dict(device=pair['device'],status=pair['status'],packages=records),indent=2)+'\n')
  (out/'README.md').write_text('# '+pair['name']+' — '+pair['device']+'\n\n'+pair['status']+'\n\nCatalog install: select '+pair['name']+'; the matching helper is added automatically to the same batch. Only one insert effect appears on the device. Save the project before restarting.\n\nDirect/older-installer install: extract this complete archive and import BOTH component ZIPs into one batch. They are the effect and its background helper, not two effects. Do not mix device folders.\n\n'+(ROOT/'docs'/pair['docs']).read_text())
  checksum=''.join(sha(out/n)+'  '+n+'\n' for n in [plugin_name,helper_name,'README.md','RELEASE.json']);(out/'SHA256SUMS').write_text(checksum)
  with zipfile.ZipFile(out/pair['complete'],'w',zipfile.ZIP_DEFLATED) as z:
   for n in [plugin_name,helper_name,'README.md','RELEASE.json','SHA256SUMS']:z.write(out/n,pair['device']+'/'+n)
  print('COMPLETE:',out/pair['complete'])
if __name__=='__main__':main()

#!/usr/bin/env python3
"""Build separate opt-in patch scripts from the stripped ARMv7 helpers."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import textwrap

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
PROFILES = {
    'mpc': dict(
        id='retrolooper-buttons', patch_id='retrolooper-mpc-controls',
        title='Retrolooper Controls for MPC 3.10.0.49', version='0.2.13',
        firmware='MPC One+ 3.10.0.49 (modified host)',
        build='bc141062ea5ea5be619d98366f423c8102574365',
        md5='b6cc8e7a398bdf6c3fd8f7176eecd3ee',
        sha='753f838ca362d42c8cc3f5c9c3fcc604ba076b5919212e0301923a3c11c93494',
        path='control/retrolooper_buttons.so'),
    'force': dict(
        id='retrolooper-force-controls', patch_id='retrolooper-force-controls',
        title='Retrolooper Controls for Force 3.10', version='0.2.10',
        firmware='Akai Force 3.10 (verified host image)',
        build='154ae9752e0f3c6490912a703b0dfe4b668b343d',
        md5='a282a259d094223b7f7c7d74c1c6f1fb',
        sha='7bbbd8050bfe67db4d320df0ab39f5c3194210f9bbd225d8415f0c4c5f43300d',
        path='force/workspace/control/retrolooper_force_controls.so'),
}

def digest(data):
    return hashlib.sha256(data).hexdigest()

def check_stripped_arm(data):
    assert data[:6] == b'\x7fELF\x01\x01'
    assert struct.unpack_from('<HH', data, 16) == (3,40), 'expected ARM shared library'
    start = struct.unpack_from('<I', data, 32)[0]
    step,n,strings = struct.unpack_from('<HHH', data, 46)
    sections = [struct.unpack_from('<10I',data,start+i*step) for i in range(n)]
    tab=sections[strings]
    name_data=data[tab[4]:tab[4]+tab[5]]
    names=[name_data[s[0]:].split(b'\0',1)[0] for s in sections]
    assert not any(s.startswith((b'.debug',b'.zdebug')) or s==b'.symtab' for s in names), names
    assert b'.dynsym' in names and b'/Users/' not in data and b'/private/tmp/' not in data

def render(template, profile, data):
    p=profile
    check_stripped_arm(data)
    b64='\n'.join(textwrap.wrap(base64.b64encode(data).decode(),76))
    fields=dict(TITLE=p['title'],ID=p['id'],VERSION=p['version'],BINARY=Path(p['path']).name,
                HOST_SHA=p['sha'],PAYLOAD_SHA=digest(data),FIRMWARE=p['firmware'],
                PAYLOAD_FUNCTION="payload(){\ncat <<'RETROLOOPER_PAYLOAD'\n"+b64+"\nRETROLOOPER_PAYLOAD\n}")
    for k,v in fields.items():
        key=f'@{k}@'
        assert template.count(key)==1,key
        template=template.replace(key,v)
    assert not re.search(r'@[A-Z_]+@',template)
    return template

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    entries=[]
    template=(HERE/'controls-patch.sh.in').read_text()
    for device,p in PROFILES.items():
        data=(PORT/p['path']).read_bytes()
        patch=args.out/(p['patch_id']+'-'+p['version']+'.sh')
        patch.write_text(render(template,p,data))
        patch.chmod(0o755)
        subprocess.run(['sh','-n',patch],check=True)
        entries.append(dict(
            id=p['patch_id'],title=p['title'],
            summary='Experimental, optional track and pad integration for Retrolooper One, including '+
                    ('Matrix clip bounce.' if device=='force' else 'Audio track timeline bounce.'),
            tier='experimental',
            requires=p['firmware']+' on ARMv7 (host build '+p['build']+')',
            author='Retroloop USA / IAMTHEBAND LLC',license='GPL-3.0-only',
            docs=f'docs/retrolooper-controls-{device}.md',
            script=dict(filename=patch.name,sha256=digest(patch.read_bytes())),
            supports=dict(os=p['firmware']+'; host build '+p['build'],arch='armv7l',mpc_md5=[p['md5']]),
            modifies=[f'/data/mpc-addins/{p["id"]}/{Path(p["path"]).name}',
                      '/etc/systemd/system/acvs.service.d/90-mpc-addins.conf'],
            backup=f'/data/retrolooper-controls/backups/{p["id"]}',restarts_mpc=True,reversible=True,
            validation=dict(host_sha256=p['sha'],library_sha256=digest(data))))
        print(f'{device}: stripped Controls verified, {len(data):,} bytes; {patch}')
    (args.out/'patch-candidates.json').write_text(json.dumps(dict(schema=1,patches=entries,
       note='Draft for review. Pin script URLs to the published source commit after hardware tests.'),indent=2)+'\n')

if __name__=='__main__':main()

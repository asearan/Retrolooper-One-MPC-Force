#!/usr/bin/env python3
"""Exercise both complete generated patch scripts against an isolated fake systemd root."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
from build_patches import HERE, PORT, PROFILES, digest, render

def run_fixture(profile, decoder):
    with tempfile.TemporaryDirectory(prefix='retrolooper-patch-test.',dir='/tmp') as td:
        root=Path(td)
        tools=root/'bin'
        tools.mkdir()
        template=(HERE/'controls-patch.sh.in').read_text().replace("root=''",'root='+shlex.quote(str(root)))
        host=root/'usr/bin/MPC'
        host.parent.mkdir(parents=True)
        host.write_bytes(b'Mock exact native host for test\n')
        versioned={**profile,'sha':digest(host.read_bytes())}
        binary=(PORT/profile['path']).read_bytes()
        patch=root/'patch.sh'
        patch.write_text(render(template,versioned,binary))
        so=root/'data/mpc-addins'/profile['id']/Path(profile['path']).name
        fragment=root/'usr/lib/systemd/system/acvs.service'
        fragment.parent.mkdir(parents=True)
        fragment.write_text('[Service]\nEnvironment=LD_PRELOAD=/usr/lib/vendor.so\n')
        drop=root/'etc/systemd/system/acvs.service.d/90-mpc-addins.conf'
        drop.parent.mkdir(parents=True)
        prior='[Service]\n# lib: 2\n# base: /usr/lib/vendor.so\nEnvironment=LD_PRELOAD=/usr/lib/vendor.so:/data/mpc-addins/other.so\n'
        drop.write_text(prior)
        (root/'pid').write_text('20\n')
        proc=root/'proc/20'
        proc.mkdir(parents=True)
        (proc/'maps').write_text('')
        def shim(name,body):
            p=tools/name;p.write_text('#!/bin/sh\nset -eu\n'+body+'\n');p.chmod(0o755)
        shim('uname','echo armv7l')
        shim('id','echo 0')
        shim('sleep',':')
        shim('pidof','cat "$TEST_ROOT/pid"')
        shim('systemctl',r'''
case "$1" in
 show)
    case "$4" in
        FragmentPath) echo "$TEST_ROOT/usr/lib/systemd/system/acvs.service";;
        DropInPaths)
            f="$TEST_ROOT/etc/systemd/system/acvs.service.d/90-mpc-addins.conf"
            [ ! -f "$f" ] || echo "$f"
            ;;
    esac
    ;;
 daemon-reload|is-active) : ;;
 restart)
    old=$(cat "$TEST_ROOT/pid"); new=$((old+1))
    echo restart >>"$TEST_ROOT/restarts"
    echo "$new" >"$TEST_ROOT/pid"; mkdir -p "$TEST_ROOT/proc/$new"
    f="$TEST_ROOT/etc/systemd/system/acvs.service.d/90-mpc-addins.conf"
    if [ -f "$TEST_ROOT/fail-once" ]; then
        rm "$TEST_ROOT/fail-once"; : >"$TEST_ROOT/proc/$new/maps"
    elif [ -f "$f" ]; then
        sed -n 's/^Environment=LD_PRELOAD=//p' "$f" | tr ':' '\n' |
            awk '{print "a-b rw-p 0000 00:00 0 " $0}' >"$TEST_ROOT/proc/$new/maps"
    else : >"$TEST_ROOT/proc/$new/maps"; fi
    ;;
 *) exit 3 ;;
esac''')
        # Mirror the device PATH by exposing only the selected decoder.
        for name in ('awk', 'cat', 'chmod', 'cp', 'dirname', 'grep', 'md5sum',
                     'mkdir', 'mktemp', 'mv', 'rm', 'rmdir', 'sed', 'sha256sum',
                     'sh', 'sort', 'tail', 'tr', decoder):
            executable = shutil.which(name)
            assert executable, name
            (tools/name).symlink_to(executable)
        env={**os.environ,'TEST_ROOT':str(root),'PATH':str(tools)}
        def cmd(*args,ok=True):
            result=subprocess.run(['sh',str(patch),*args],env=env,text=True,capture_output=True,timeout=15)
            assert (result.returncode==0)==ok,(args,result)
            return result.stdout
        before={p.relative_to(root):p.read_bytes() for p in root.rglob('*') if p.is_file()}
        assert 'STATE state=stock supported=1 backup=0' in cmd('status')
        assert before=={p.relative_to(root):p.read_bytes() for p in root.rglob('*') if p.is_file()}, 'status wrote files'
        host.write_bytes(b'Unsupported host')
        assert 'STATE state=unsupported supported=0' in cmd('status')
        cmd('install','--confirmed',ok=False)
        assert not so.exists() and drop.read_text()==prior and not (root/'restarts').exists()
        host.write_bytes(b'Mock exact native host for test\n')
        (root/'fail-once').touch()
        cmd('install','--confirmed',ok=False)
        assert not so.exists() and drop.read_text()==prior
        assert len((root/'restarts').read_text().splitlines())==2, 'failed apply must restore and restart'
        assert 'STATE state=patched supported=1 backup=1' in cmd('install','--confirmed')
        assert so.read_bytes()==binary
        assert '/data/mpc-addins/other.so' in drop.read_text()
        assert len((root/'restarts').read_text().splitlines())==3
        drop.write_text(drop.read_text().replace('other.so','other.so:/data/mpc-addins/added-later.so'))
        host.write_bytes(b'Firmware replaced after install')
        assert 'Controls' in cmd('uninstall','--confirmed')
        assert not so.exists()
        assert '/data/mpc-addins/other.so:/data/mpc-addins/added-later.so' in drop.read_text()
        assert str(so) not in drop.read_text()
        assert len((root/'restarts').read_text().splitlines())==4
        assert 'already removed' in cmd('uninstall','--confirmed')
        print('PASS',profile['title'],': readonly status, exact hash refusal, failed-start rollback, apply, remove after firmware change and unrelated add-ins retained')
for key, profile in PROFILES.items():
    run_fixture(profile, 'openssl' if key == 'force' else 'base64')

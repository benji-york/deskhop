#!/usr/bin/env python3
"""Repeatable hardware-free test tiers: python3 tests/run.py [fast|deep|arm]."""
import argparse
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import time
ROOT=pathlib.Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/tests'
BASE=['-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
results=[]
result_name='results.json'
def run(name,command,env=None,timeout=120):
    print(f'\n{name}: {shlex.join(map(str,command))}',flush=True);start=time.monotonic()
    result=subprocess.run(list(map(str,command)),cwd=ROOT,env=env,timeout=timeout)
    results.append({'name':name,'command':list(map(str,command)),
                    'seconds':round(time.monotonic()-start,3),'returncode':result.returncode})
    encoded=json.dumps(results,indent=2)+'\n'
    (BUILD/'results.json').write_text(encoded)
    (BUILD/result_name).write_text(encoded)
    result.check_returncode()
def compile_test(name,sources,extra=()):
    binary=BUILD/name
    run('build '+name,[os.environ.get('CC','cc'),*BASE,*extra,'-Isrc/include',*sources,'-o',binary])
    run(name,[binary]);return binary

def main():
    global result_name
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('tier',choices=['fast','deep','arm'],nargs='?',default='fast');a=p.parse_args()
    result_name=f'results-{a.tier}.json'
    BUILD.mkdir(parents=True,exist_ok=True)
    if a.tier=='arm':
        env=dict(os.environ);env['PATH']='/opt/homebrew/opt/arm-gcc-bin@14/bin:'+env.get('PATH','')
        run('ARM configure',['cmake','-S','.','-B','build/arm-validation'],env,120)
        run('ARM firmware',['cmake','--build','build/arm-validation','--parallel','4'],env,600)
        return
    run('host updater contracts',[sys.executable,'-m','unittest','discover','-s','tests/updater','-v'])
    for name,unit in [('zoom_tracker','zoom_tracker'),('fw_update','fw_update'),('fw_batch','fw_batch'),
                      ('screensaver_policy','screensaver_policy'),('reboot_hotkey','reboot_hotkey'),
                      ('config_migration','config_migration'),('selection','selection'),
                      ('peer_status','peer_status'),('history','history'),
                      ('peer_history','peer_history'),('peer_observation','peer_observation'),('verification','verification'),
                      ('peer_verify','peer_verify')]:
        dependencies = ['src/history.c'] if name == 'peer_history' else []
        compile_test(name,[f'tests/test_{name}.c',f'src/{unit}.c',*dependencies])
    compile_test('diagnostic_verify',['tests/test_diagnostic_verify.c','src/peer_verify.c',
                 'pico-sdk/src/common/pico_util/queue.c'],
                 ['-Wno-unused-parameter','-Wno-sign-compare','-Itests/sim/include',
                  '-Ipico-sdk/src/common/pico_util/include','-Ipico-sdk/lib/tinyusb/src'])
    node=os.environ.get('NODE') or shutil.which('node') or str(pathlib.Path.home()/'.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node')
    run('WebConfig auto-start',[node,'tests/test_webconfig_autostart.js'])
    run('WebConfig Bootloader button',[node,'tests/test_webconfig_bootloader.js'])
    run('WebConfig timer units',[node,'tests/test_webconfig_timers.js'])
    run('WebConfig confirmed two-Pico saves',[node,'tests/test_webconfig_confirmed_saves.js'])
    hid_flags=['-Wno-unused-parameter','-Wno-sign-compare','-Itests/hid_stubs','-Ipico-sdk/lib/tinyusb/src']
    hid_sources=['src/hid_parser.c','src/hid_report.c','src/keyboard.c','src/usb.c','src/reboot_hotkey.c',
                 'tests/history_stub.c']
    compile_test('hid_regressions',['tests/test_hid_regressions.c',*hid_sources],hid_flags)
    hid=BUILD/'hid_properties'
    run('build HID properties',[os.environ.get('CC','cc'),*BASE,*hid_flags,'-Isrc/include','tests/hid_fuzz.c',*hid_sources,'-o',hid])
    run('HID properties',[hid,'--iterations','20000' if a.tier=='deep' else '2000','--artifact',BUILD/'last-hid-input.hex'])
    compile_test('model_policy',['tests/model/policy_contract.c','src/fw_update.c'])
    run('finite flash model',[sys.executable,'tests/model/check_flash.py','--mutations','--power-loss','--output',BUILD/'model-traces'])
    run('real storage transactions',[sys.executable,'tests/storage/run.py','--seeds','16' if a.tier=='deep' else '1'])
    run('real TinyUSB device stack',[sys.executable,'tests/usb_stack/run.py'])
    run('real TinyUSB host stack',[sys.executable,'tests/usb_host/run.py'])
    run('upstream USB transaction and DPRAM regressions',[sys.executable,'tests/upstream_usb/run.py'])
    sys.path.insert(0,str(ROOT/'tests/sim'))
    from build import build
    build(BUILD/'sim/node.so')
    native=build(BUILD/'sim/native-boundaries',executable=ROOT/'tests/sim/test_native_boundaries.c',sanitize=True)
    run('production native boundary properties',[native])
    advertisement=build(BUILD/'sim/fw-advertisement',executable=ROOT/'tests/sim/test_fw_advertisement.c',sanitize=True)
    for role in ('0','1'):
        run('firmware advertisement boundary role '+role,[advertisement,role])
    config=build(BUILD/'sim/configuration',executable=ROOT/'tests/sim/test_configuration.c',sanitize=True)
    run('configuration ingestion and consumer properties',[config])
    crashes=build(BUILD/'sim/config-crashes',executable=ROOT/'tests/sim/test_config_crashes.c',sanitize=True)
    run('configuration equal-border regression',[crashes,'border'])
    run('configuration output-identity regression',[crashes,'identity'])
    run('paired production firmware',[sys.executable,'tests/sim/run.py','--known-gaps'],timeout=300)
    run('simulator contract checks',[sys.executable,'tests/sim/test_harness.py'])
    run('confirmed configuration paired transport',
        [sys.executable,'tests/sim/test_config_confirm.py',
         *(['--all-orders'] if a.tier=='deep' else [])],timeout=300)
    run('firmware startup guard and historical starvation witnesses',
        [sys.executable,'tests/sim/test_fw_startup.py','--library',BUILD/'sim/node.so',
         '--seeds','1',*(['--full'] if a.tier=='deep' else []),
         '--output',BUILD/f'fw-startup-{a.tier}.json'],timeout=180)
    if a.tier=='deep':
        run('accepted-v108 configuration crash witnesses',[sys.executable,'tests/sim/test_config_baseline.py'])
        run('upstream USB source mutations',[sys.executable,'tests/upstream_usb/run.py','--mutations'])
        run('bounded core order exploration',[sys.executable,'tests/sim/run.py','--scenario','backpressure','--interleavings'])
        run('peer query core order exploration',[sys.executable,'tests/sim/run.py','--scenario','peer_status_roundtrip','--interleavings'])
        run('verification core order exploration',[sys.executable,'tests/sim/run.py','--scenario','verify_concurrent','--interleavings'])
        run('peer history core order exploration',[sys.executable,'tests/sim/run.py','--scenario','peer_history_roundtrip','--interleavings'])
        run('batch transfer core order exploration',[sys.executable,'tests/sim/run.py','--scenario','batch_core_schedule','--interleavings'])
        run('mixed-version firmware transfer',[sys.executable,'tests/sim/test_fw_batch.py','--mixed-versions'],timeout=300)
        run('generated end-to-end inputs',[sys.executable,'tests/sim/run.py','--scenario','generated','--seeds','32'],timeout=180)
        run('storage source mutations',[sys.executable,'tests/storage/mutations.py'])
        run('paired source mutations',[sys.executable,'tests/sim/mutations.py'],timeout=180)
        run('baseline behavior traces',[sys.executable,'tests/sim/differential.py'])
    print(f'\n{a.tier} tier passed. Results: {BUILD / result_name}')
if __name__=='__main__':main()

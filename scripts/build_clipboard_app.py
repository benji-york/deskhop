#!/usr/bin/env python3
"""Build/test/package the all-Swift app; never install, open serial or read clipboard."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'macos/DeskHopClipboard'
BUILD = ROOT / 'build/clipboard-app'


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), cwd=ROOT, check=True, **kwargs)


def environment():
    env = dict(os.environ)
    cache = ROOT / 'build/clipboard-module-cache'
    cache.mkdir(parents=True, exist_ok=True)
    env['CLANG_MODULE_CACHE_PATH'] = str(cache)
    env['SWIFTPM_MODULECACHE_OVERRIDE'] = str(cache)
    return env


def swift(command, scratch, env, *flags):
    return run(['xcrun', 'swift', command, '--package-path', PACKAGE,
                '--scratch-path', scratch, '--cache-path', ROOT / 'build/clipboard-swift-cache',
                '--config-path', ROOT / 'build/clipboard-swift-config',
                '--security-path', ROOT / 'build/clipboard-swift-security',
                '--disable-sandbox', *flags], env=env)


def tests(env):
    scratch = ROOT / 'build/clipboard-swift'
    swift('build', scratch, env, '--product', 'DeskHopClipboard')
    swift('run', scratch, env, 'ClipboardCoreTests')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--test-only', action='store_true')
    p.add_argument('--identity', default='-', help='explicit Developer ID Application identity; default ad-hoc local build')
    a = p.parse_args()
    if platform.system() != 'Darwin':
        p.error('macOS with Xcode Command Line Tools is required')
    env = environment()
    tests(env)
    if a.test_only:
        return
    BUILD.mkdir(parents=True, exist_ok=True)
    iconset = BUILD / 'Clipboard.iconset'
    run(['xcrun', 'swift', '-module-cache-path', env['CLANG_MODULE_CACHE_PATH'],
         PACKAGE / 'Tools/make-icon.swift', iconset], env=env)
    # ICNS PNG elements avoid iconutil's dependence on desktop image services.
    elements = []
    for kind, filename in [('icp4', 'icon_16x16.png'), ('icp5', 'icon_32x32.png'),
                           ('icp6', 'icon_32x32@2x.png'), ('ic07', 'icon_128x128.png'),
                           ('ic08', 'icon_256x256.png'), ('ic09', 'icon_512x512.png'),
                           ('ic10', 'icon_512x512@2x.png')]:
        png = (iconset / filename).read_bytes()
        elements.append(kind.encode('ascii') + struct.pack('>I', len(png) + 8) + png)
    body = b''.join(elements)
    (BUILD / 'Clipboard.icns').write_bytes(b'icns' + struct.pack('>I', len(body) + 8) + body)
    run(['sips', '-g', 'format', BUILD / 'Clipboard.icns'])
    binaries, readers = [], []
    for arch in ('arm64', 'x86_64'):
        scratch = BUILD / arch
        swift('build', scratch, env, '-c', 'release', '--triple', f'{arch}-apple-macosx13.0', '--product', 'DeskHopClipboard')
        binaries.append(scratch / f'{arch}-apple-macosx/release/DeskHopClipboard')
        reader = BUILD / f'reader-{arch}'
        run(['xcrun', 'swiftc', '-O', '-target', f'{arch}-apple-macosx13.0', '-module-cache-path',
             env['CLANG_MODULE_CACHE_PATH'], ROOT / 'scripts/clipboard/native.swift', '-o', reader], env=env)
        readers.append(reader)
    # Rebuild in staging; replace only our named generated artifact after success.
    with tempfile.TemporaryDirectory(prefix='package-', dir=BUILD) as staging:
        stage = Path(staging)
        app = stage / 'DeskHop Clipboard.app'
        contents = app / 'Contents'
        (contents / 'MacOS').mkdir(parents=True)
        (contents / 'Helpers').mkdir()
        (contents / 'Resources').mkdir()
        shutil.copy2(PACKAGE / 'Resources/Info.plist', contents / 'Info.plist')
        shutil.copy2(ROOT / 'LICENSE', contents / 'Resources/LICENSE')
        shutil.copy2(BUILD / 'Clipboard.icns', contents / 'Resources/Clipboard.icns')
        executable = contents / 'MacOS/DeskHopClipboard'
        reader = contents / 'Helpers/DeskHopClipboardReader'
        run(['lipo', '-create', *binaries, '-output', executable])
        run(['lipo', '-create', *readers, '-output', reader])
        executable.chmod(0o755); reader.chmod(0o755)
        signing = ['--force', '--options', 'runtime', '--sign', a.identity]
        signing += ['--timestamp=none'] if a.identity == '-' else ['--timestamp']
        run(['codesign', *signing, reader])
        run(['codesign', *signing, app])
        run(['codesign', '--verify', '--deep', '--strict', '--verbose=2', app])
        run(['plutil', '-lint', contents / 'Info.plist'])
        run([executable, '--bundle-check'])
        run(['lipo', executable, '-verify_arch', 'arm64', 'x86_64'])
        run(['lipo', reader, '-verify_arch', 'arm64', 'x86_64'])
        # This mode only consumes supplied stdin bytes; no AppKit pasteboard call.
        for text in (b'', b'A\tB\n', b'x' * 1024, b'x' * 1025, b'CR\rLF\n', 'emoji \U0001f642'.encode()):
            result = run([reader, '--fixture'], input=text, capture_output=True)
            expected = 3 if len(text) > 1024 else 1 if not text else 4 if any(c not in (9, 10) and not 32 <= c <= 126 for c in text) else 0
            assert result.stdout == bytes([expected]) + (len(text) if expected == 0 else 0).to_bytes(2, 'little') + (text if expected == 0 else b'')
        destination = BUILD / app.name
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(app, destination, symlinks=True)
    distribution = BUILD / 'DeskHop Clipboard'
    if distribution.exists():
        shutil.rmtree(distribution)
    distribution.mkdir()
    shutil.copytree(destination, distribution / destination.name, symlinks=True)
    shutil.copy2(PACKAGE / 'INSTALL.txt', distribution / 'INSTALL.txt')
    archive = BUILD / 'DeskHop-Clipboard-0.1.0.zip'
    if archive.exists():
        archive.unlink()
    run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', distribution, archive])
    print(f'Built {destination}\nPackaged {archive}\nNot installed. Login item unchanged. Not notarized.')


if __name__ == '__main__':
    main()

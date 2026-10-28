"""Validate archives and synthetic tasks from the packaged binary."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import wave


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_reports(directory: Path):
    platforms = {'windows-x64', 'windows-arm64', 'macos-arm64'}
    seen = set()
    revisions = set()
    for path in directory.glob('*verification.json'):
        report = json.loads(path.read_text(encoding='utf-8'))
        assert report['passed'], path
        assert report['platform'] not in seen, 'Duplicate platform report'
        seen.add(report['platform'])
        revisions.add(report['revision'])
        archive = directory / report['archive']
        assert sha256(archive) == report['sha256'], archive
    assert seen == platforms, f'Expected {platforms}, got {seen}'
    assert len(revisions) == 1, 'Platform packages must have the same source revision'
    if os.environ.get('GITHUB_SHA'):
        assert revisions == {os.environ['GITHUB_SHA']}, 'Reports do not match the Release commit'


def verify_archive(archive: Path):
    seven = shutil.which('7z') or shutil.which('7zz') or shutil.which('7za')
    if not seven:
        raise RuntimeError('7-Zip is required to verify a Release archive')
    subprocess.run([seven, 't', str(archive)], check=True, capture_output=True)
    root = Path(__file__).resolve().parents[2]
    test_root = root / 'build' / 'release-validation'
    test_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='package-', dir=test_root) as temporary:
        temporary = Path(temporary)
        subprocess.run([seven, 'x', '-y', f'-o{temporary}', str(archive)], check=True, capture_output=True)
        executables = list(temporary.glob('*/MiaCode.exe'))
        if not executables:
            executables = list(temporary.glob('*/MiaCode.app/Contents/MacOS/MiaCode'))
        assert len(executables) == 1, 'Exactly one packaged entry point is required'
        executable = executables[0]
        runtime = temporary / 'synthetic-runtime'
        (runtime / 'config').mkdir(parents=True)
        (runtime / '.better-miacode-runtime').write_text('BetterMiacode.runtime.v1\n')
        environment = os.environ.copy()
        environment['BETTER_MIACODE_RUNTIME_ROOT'] = str(runtime)
        # Exercise plugins shipped in the actual package, without borrowing SDK
        # plugins. The renderer uses hidden capture surfaces on the native QPA.
        environment['QT_QPA_PLATFORM'] = 'windows' if os.name == 'nt' else 'cocoa'
        environment['QT_FORCE_STDERR_LOGGING'] = '1'
        environment['MIACODE_SKIP_PREFLIGHT'] = '1'
        fixture = temporary / '合成 chart'
        fixture.mkdir()
        chart = fixture / 'maidata.txt'
        chart.write_text('&title=Synthetic verification\n&artist=Fixture\n&first=0\n&lv_4=1\n'
                         '&inote_4=(120){4},,,,1,2b,3h[4:1],4-6[4:1],(150)5,6,7,8,\nE\n', encoding='utf-8')
        with wave.open(str(fixture / 'track.wav'), 'wb') as audio:
            audio.setnchannels(2)
            audio.setsampwidth(2)
            audio.setframerate(48000)
            audio.writeframes(b'\0' * (48000 * 8 * 4))
        checks = []

        def invoke(*arguments, expected=0):
            process = subprocess.run([str(executable), *map(str, arguments)], env=environment,
                                     cwd=fixture, capture_output=True, text=True, encoding='utf-8',
                                     errors='replace', timeout=180)
            if process.returncode != expected:
                raise RuntimeError(f'{arguments}: exit {process.returncode}\n{process.stdout}\n{process.stderr}')
            checks.append(list(map(str, arguments[:3])))
            return process.stdout

        invoke('--help')
        invoke('--version', '--json')
        invoke('capabilities', '--json')
        invoke('stats', '--chart', chart, '--difficulty', 4, '--json', expected=77)
        preferences = runtime / 'config' / 'preferences_v2.json'
        preferences.write_text(json.dumps({'command_line': {'enabled': True}}), encoding='utf-8')
        invoke('stats', '--chart', chart, '--difficulty', 4, '--json')
        invoke('presets', 'list', '--json')
        bpm = fixture / 'bpm.txt'
        invoke('export', 'bpm', '--chart', chart, '--difficulty', 4, '--output', bpm, '--json')
        assert bpm.is_file() and '120' in bpm.read_text(encoding='utf-8')
        cover = fixture / 'cover.png'
        invoke('export', 'cover', '--chart', chart, '--difficulty', 4, '--output', cover,
               '--time', 2.5, '--json')
        assert cover.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n'
        video = fixture / 'video.mp4'
        invoke('export', 'video', '--chart', chart, '--difficulty', 4, '--output', video,
               '--width', 640, '--height', 360, '--fps', 30, '--start', 2, '--duration', 1,
               '--show-bpm', '--show-cps', '--json')
        assert video.is_file() and video.stat().st_size > 1024
        before = sha256(video)
        invoke('export', 'video', '--chart', chart, '--difficulty', 4, '--output', video,
               '--duration', 1, '--json', expected=3)
        assert sha256(video) == before, 'Output conflict changed an existing video'
        report = {'passed': True, 'platform': os.environ.get('CI_PLATFORM', 'windows-x64'),
                  'archive': archive.name, 'sha256': sha256(archive), 'checks': checks,
                  'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                  'manualGuiTest': False}
    archive.with_suffix(archive.suffix + '.sha256').write_text(
        report['sha256'] + '  ' + archive.name + '\n', encoding='utf-8')
    archive.with_name(archive.stem + '-verification.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--dist', type=Path)
    parser.add_argument('--verify-reports', type=Path)
    args = parser.parse_args()
    if args.verify_reports:
        verify_reports(args.verify_reports)
    elif args.dist:
        archives = list(args.dist.resolve().glob('*.7z'))
        assert len(archives) == 1, 'Expected one platform archive in the staging directory'
        verify_archive(archives[0])
    else:
        parser.error('--dist or --verify-reports is required')

"""在隔离的真实 IDE 中编译原件与回包件，逐一确认 EXE、DLL 或 EC 产物。"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8-sig')


def compile_one(args, item, label, source):
    case = args.output / f'{item["id"]:04}' / label
    case.mkdir(parents=True, exist_ok=True)
    target = item.get("target", "ecom")
    artifact = case / (f'audit_{item["id"]:04}_{label}' + (".ec" if target == "ecom" else ".exe"))
    result = case / 'result.json'
    data = source.read_bytes()
    fingerprint = hashlib.sha256(data).hexdigest()
    if (case / 'stage.json').exists():
        cached = read(case / 'stage.json')
        assert cached['input_sha256'] == fingerprint and cached['tool_hashes'] == args.tool_hashes and cached.get('target', 'ecom') == target and cached.get('static_compile', False) == args.static_compile, 'stale IDE audit cache'
        return cached
    with tempfile.NamedTemporaryFile(prefix='__ide_ecom_audit_', suffix='.e', dir=(args.roundtrip / 'inputs' / item['relative']).parent, delete=False) as stream:
        staged = Path(stream.name)
        stream.write(data)
    command = [str(args.launcher), 'headless-compile', str(args.ide), str(staged), str(artifact),
               '--target', target, '--result', str(result), '--timeout', str(args.timeout)]
    if args.static_compile:
        command.append('--static')
    # IDE 启动会探测临时目录，独立目录避免并行实例争用固定探测文件。
    temp_directory = case / 'temp'
    temp_directory.mkdir(exist_ok=True)
    environment = dict(os.environ, TEMP=str(temp_directory), TMP=str(temp_directory))
    started = time.monotonic()
    row = {'input_sha256': fingerprint, 'tool_hashes': args.tool_hashes, 'command': command, 'target': target, 'static_compile': args.static_compile}
    try:
        with (case / 'launcher.log').open('wb') as log:
            process = subprocess.Popen(command, cwd=case, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW, env=environment)
            try:
                row['exit'] = process.wait(timeout=args.timeout + 60)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=15)
                row['exit'] = process.wait(timeout=15)
                row['outer_timeout'] = True
        payload = read(result) if result.exists() else {}
        compiled = payload.get('compile_result', {})
        actual_path = compiled.get('output_path')
        if actual_path:
            candidate = Path(actual_path)
            assert candidate.parent.resolve() == case.resolve(), 'unexpected artifact directory'
            artifact = candidate
        row.update(ok=row['exit'] == 0 and compiled.get('ok') is True and compiled.get('artifact_verified') is True
                   and artifact.exists() and artifact.stat().st_size > 0,
                   error=payload.get('error') or compiled.get('error'), diagnostics=compiled.get('diagnostics', []),
                   timed_out=payload.get('launcher_timed_out', False) or row.get('outer_timeout', False) or 'timeout' in str(payload.get('error', '')),
                   output=compiled.get('output_window_text', ''))
        if artifact.exists():
            row['artifact_sha256'] = hashlib.sha256(artifact.read_bytes()).hexdigest()
    finally:
        staged.unlink(missing_ok=True)
    row['seconds'] = round(time.monotonic() - started, 3)
    save(case / 'stage.json', row)
    return row


def audit(args, item):
    stages = {'original': compile_one(args, item, 'original', args.roundtrip / 'inputs' / item['relative'])}
    for arch in ('Win32', 'x64'):
        stages[arch] = compile_one(args, item, arch, args.roundtrip / 'cases' / f'{item["id"]:04}' / arch / 'edited.e')
    row = {'id': item['id'], 'source': item['relative'], 'stages': stages,
           'regressions': [a for a in ('Win32', 'x64') if stages['original']['ok'] and not stages[a]['ok']]}
    save(args.output / f'{item["id"]:04}' / 'result.json', row)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--roundtrip', type=Path, required=True)
    parser.add_argument('--compile-baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--ide', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--timeout', type=int, default=45)
    parser.add_argument('--ids', type=int, nargs='*')
    parser.add_argument('--static', dest='static_compile', action='store_true', help='use the IDE static compiler for native projects')
    parser.add_argument('--native-only', action='store_true', help='compile non-module projects using their native IDE target')
    args = parser.parse_args()
    # IDE 发布可能自动复制到 ecom，禁止使用指向安装目录的联接。
    ecom = args.ide.parent / 'ecom'
    assert ecom.is_dir() and not ecom.is_junction() and not ecom.is_symlink(), 'EC publication requires a private ecom directory'
    args.tool_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in [args.launcher, args.ide, args.ide.parent / 'lib/AutoLinker.fne']}
    items = []
    for item in read(args.roundtrip / 'manifest.json'):
        if not item['size'] or (args.ids and item['id'] not in args.ids):
            continue
        native = read(args.compile_baseline / f'{item["id"]:04}' / 'original/stage.json')
        is_module = native.get('project_type') in (1000, 11000)
        if is_module != args.native_only:
            # 原生头部明确给出目标，不依赖 IDE 标题推断 GUI/控制台/DLL。
            item['target'] = 'ecom' if is_module else {0: 'win_exe', 1: 'win_console_exe', 2: 'win_dll'}[native['project_type']]
            assert hashlib.sha256((args.roundtrip / 'inputs' / item['relative']).read_bytes()).hexdigest() == item['sha256']
            items.append(item)
    args.output.mkdir(parents=True, exist_ok=True)
    rows = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        pending = [pool.submit(audit, args, item) for item in items]
        for future in as_completed(pending):
            row = future.result()
            rows.append(row)
            save(args.output / 'progress.json', rows)
            print(json.dumps({'done': len(rows), 'id': row['id'], 'success': {k: v['ok'] for k, v in row['stages'].items()},
                              'regressions': row['regressions']}), flush=True)
    summary = {'files': len(rows), 'success': {k: sum(r['stages'][k]['ok'] for r in rows) for k in ('original', 'Win32', 'x64')},
               'timeouts': {k: sum(r['stages'][k].get('timed_out', False) for r in rows) for k in ('original', 'Win32', 'x64')},
               'regressions': [{'id': r['id'], 'source': r['source'], 'architectures': r['regressions']} for r in rows if r['regressions']]}
    save(args.output / 'summary.json', summary)
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()

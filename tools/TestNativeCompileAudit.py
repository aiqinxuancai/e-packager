"""分别编译原工程与语义回包，区分原有编译错误和回包引入的错误。"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8-sig')


def compile_one(args, item, label, data):
    case = args.output / f'{item["id"]:04}' / label
    case.mkdir(parents=True, exist_ok=True)
    source = args.roundtrip / 'inputs' / item['relative']
    result_path = case / 'compile.json'
    input_hash = hashlib.sha256(data).hexdigest()
    target = getattr(args, 'target', 'auto')
    core_hash = getattr(args, 'core_sha256', None) or hashlib.sha256(args.core.read_bytes()).hexdigest()
    if result_path.exists() and (case / 'stage.json').exists():
        cached = read(case / 'stage.json')
        if (cached.get('input_sha256') != input_hash or cached.get('core_sha256') != core_hash
                or cached.get('target') != target or cached.get('module_policy') != args.module_policy):
            raise ValueError(f'stale compile cache: {case}; use a new output directory')
        return cached
    with tempfile.NamedTemporaryFile(prefix='__compile_audit_', suffix='.e', dir=source.parent, delete=False) as stream:
        staged = Path(stream.name)
        stream.write(data)
    command = [str(args.core), 'compile', '--project', str(staged), '--output', str(case / 'artifact.exe'),
               '--result', str(result_path), '--e-dir', str(args.e_dir), '--module-policy', args.module_policy,
               '--target', getattr(args, 'target', 'auto')]
    started = time.monotonic()
    stage = {'label': label, 'input_sha256': input_hash, 'core_sha256': core_hash, 'target': target,
             'module_policy': args.module_policy, 'command': command}
    try:
        with (case / 'compile.log').open('wb') as log:
            process = subprocess.Popen(command, cwd=case, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                stage['exit'] = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=15)
                stage['exit'] = process.wait(timeout=15)
                stage['timeout'] = True
        result = read(result_path) if result_path.exists() else {}
        stage.update(ok=stage['exit'] == 0 and result.get('ok') is True and result.get('artifact_verified') is True,
                     error=result.get('error'), native_error=result.get('native_error'),
                     project_type=result.get('project_type'), diagnostics=result.get('diagnostics', []),
                     error_location=result.get('error_location', {}), messages=result.get('messages', []))
    finally:
        staged.unlink()
    stage['seconds'] = round(time.monotonic() - started, 3)
    save(case / 'stage.json', stage)
    return stage


def audit(args, item):
    original = (args.roundtrip / 'inputs' / item['relative']).read_bytes()
    assert hashlib.sha256(original).hexdigest() == item['sha256']
    stages = {'original': compile_one(args, item, 'original', original)}
    for arch in args.architectures:
        rebuilt = args.roundtrip / 'cases' / f'{item["id"]:04}' / arch / 'edited.e'
        stages[arch] = compile_one(args, item, arch, rebuilt.read_bytes())
    row = {'id': item['id'], 'source': item['relative'], 'stages': stages,
           'regressions': [a for a in args.architectures if stages['original']['ok'] and not stages[a]['ok']]}
    save(args.output / f'{item["id"]:04}' / 'result.json', row)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--roundtrip', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--e-dir', type=Path, required=True)
    parser.add_argument('--module-policy', choices=['embedded', 'update', 'check'], default='embedded')
    parser.add_argument('--architectures', nargs='+', default=['Win32', 'x64'])
    parser.add_argument('--target', choices=['auto', 'win_exe', 'win_console_exe', 'win_dll'], default='auto')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--ids', nargs='*', type=int)
    args = parser.parse_args()
    args.core_sha256 = hashlib.sha256(args.core.read_bytes()).hexdigest()
    args.output.mkdir(parents=True, exist_ok=True)
    items = [i for i in read(args.roundtrip / 'manifest.json') if i['size'] and (not args.ids or i['id'] in args.ids)]
    rows = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        pending = {pool.submit(audit, args, item): item for item in items}
        for future in as_completed(pending):
            row = future.result()
            rows.append(row)
            print(json.dumps({'done': len(rows), 'id': row['id'], 'compiled': {k: s['ok'] for k, s in row['stages'].items()},
                              'regressions': row['regressions']}, ensure_ascii=False), flush=True)
            save(args.output / 'progress.json', rows)
    summary = {'target': args.target, 'module_policy': args.module_policy, 'files': len(rows), 'success': {label: sum(r['stages'][label]['ok'] for r in rows)
               for label in ['original', *args.architectures]},
               'regressions': [{'id': r['id'], 'source': r['source'], 'architectures': r['regressions']}
                               for r in rows if r['regressions']]}
    save(args.output / 'summary.json', summary)
    print(json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()

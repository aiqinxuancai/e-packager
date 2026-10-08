"""批量检查样例拆包、所有源码页编辑、双架构语义回包及真实 IDE 编译。"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8-sig')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log, timeout=150, env=None):
    process = subprocess.Popen([str(x) for x in command], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=env)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        log.write_bytes(stdout + stderr)
        return process.returncode
    except subprocess.TimeoutExpired:
        # 工作进程属于本次命令；超时必须连同子进程清理，避免残留 IDE/控件宿主。
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True)
        stdout, stderr = process.communicate()
        log.write_bytes(stdout + stderr + b'\nAUDIT TIMEOUT')
        return -999


def normalize(text):
    return '\n'.join(line.strip() for line in text.splitlines() if line.strip())


def compile_file(args, source, root, label, static=False, blackmoon=None):
    report = root / (label + '.json')
    archived_artifact = root / (label + '.exe')
    # 链接器相对库路径以产物目录为基准；产物与输入副本同目录，再归档。
    artifact = source.with_name(source.stem + '.' + label + '.exe')
    # 每次重试清除本次输出，禁止旧报告或旧产物被误认作当前成功。
    report.unlink(missing_ok=True)
    artifact.unlink(missing_ok=True)
    archived_artifact.unlink(missing_ok=True)
    temporary = root / (label + '-temp')
    temporary.mkdir(exist_ok=True)
    env = dict(os.environ, TEMP=str(temporary), TMP=str(temporary))
    code = run([args.launcher, 'headless-compile', args.ide, source, artifact,
                '--target', 'auto', '--result', report, '--timeout', str(args.timeout)] + (['--blackmoon', blackmoon] if blackmoon else ['--static'] if static else []),
               root / (label + '.log'), args.timeout + 30, env)
    data = read(report) if report.exists() else {}
    output_text = data.get('compile_result', {}).get('output_window_text', '')
    if not static and not blackmoon and '不能动态编译' in output_text:
        result = compile_file(args, source, root, label + '-static', static=True)
        result['dynamic_attempt'] = str(report)
        return result
    compiler_errors = re.findall(r'(?im)^.*(?:fatal error|链接失败|程序代码编译失败).*$|黑月处理程序失败', output_text)
    if artifact.exists():
        shutil.copy2(artifact, archived_artifact)
    return {'exit': code, 'compiler_errors': compiler_errors, 'passed': not compiler_errors and code == 0 and data.get('ok', False)
            and data.get('compile_result', {}).get('artifact_verified', False)
            and artifact.exists() and artifact.stat().st_size > 0,
            'report': str(report), 'details': data}


def compile_repacked(args, edited, copied, root, label, blackmoon=None):
    # 编译插件可能校验工程文件名；在输入副本原位临时替换，完成后恢复。
    original = copied.read_bytes()
    try:
        copied.write_bytes(edited.read_bytes())
        return compile_file(args, copied, root, label, blackmoon=blackmoon)
    finally:
        copied.write_bytes(original)


def classify(result):
    if 'architectures' not in result:
        return result['status']
    return 'passed' if not result['placeholders'] and all(
        row['pack_exit'] == 0 and row.get('changed_bytes') and row.get('decode_exit') == 0
        and not row.get('mismatches') and row.get('compile', {}).get('passed')
        for row in result['architectures'].values()) else 'failed'


def write_report(output, results):
    lines = ['# samples 语义回包调查', '',
             '全部操作在副本中进行；原例与回包产物仅编译，不运行生成的程序。',
             '程序集内每个方法插入注释；固定声明页和窗口 XML 加空行。',
             '保留原生元数据，通过源码修改触发全语义重建，并再次拆包比较源码、窗口和资源。', '',
             f'样例数量：{len(results)}；状态：{dict(Counter(r["status"] for r in results))}', '',
             '| 架构 | 尝试回包 | 回包成功 | 无头编译通过 | 内容完全一致且编译通过 |',
             '| --- | ---: | ---: | ---: | ---: |']
    for arch in ('Win32', 'x64'):
        rows = [r['architectures'][arch] for r in results if arch in r.get('architectures', {})]
        compiled = [r for r in rows if r.get('compile', {}).get('passed')]
        strict = [r for r in compiled if r.get('changed_bytes') and r.get('decode_exit') == 0 and not r['mismatches']]
        lines.append(f'| {arch} | {len(rows)} | {sum(r["pack_exit"] == 0 for r in rows)} | {len(compiled)} | {len(strict)} |')
    lines += ['', '## 逐例结果', '',
              '原例编译失败不等于回包缺陷；内容差异不等于编译失败。详细错误和 IDE 输出见各编号目录。', '',
              '| ID | 文件 | 缺库/拆包 | 原例编译 | Win32 回包 / 编译 / 差异数 | x64 回包 / 编译 / 差异数 | _Lib 行数 |',
              '| --- | --- | --- | --- | --- | --- | ---: |']
    def state(row):
        if not row:
            return '未执行'
        compiled = row.get('compile', {}).get('passed')
        return f'{row["pack_exit"]} / {compiled} / {len(row.get("mismatches", []))}'
    for r in results:
        reason = ', '.join(d.get('fileName', d.get('name', '?')) for d in
                           (r.get('npk_libraries', []) or r.get('missing_libraries', [])))
        if r.get('unpack_exit'):
            reason = f'拆包失败 {r["unpack_exit"]}'
        lines.append(f'| [{r["id"]:03}]({Path(r["directory"]).as_posix()}/result.json) | {r["source"].split("samples", 1)[-1]} | {reason} | '
                     f'{r.get("baseline_compile", {}).get("passed")} | {state(r.get("architectures", {}).get("Win32"))} | '
                     f'{state(r.get("architectures", {}).get("x64"))} | {len(r.get("placeholders", []))} |')
    (output / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8-sig')


def audit(args, index, original, copied, root, repo):
    root.mkdir(parents=True)
    result = {'id': index, 'source': str(original), 'sha256': digest(original), 'directory': str(root)}
    decoder = args.bin_root / 'Win32/Release/e-packager.exe'
    workspace = root / 'workspace'
    try:
        if not original.stat().st_size:
            result['status'] = 'skip_empty'
            return result
        code = run([decoder, 'unpack', copied, workspace], root / 'unpack.log')
        result['unpack_exit'] = code
        if code:
            npk_names = sorted(set(re.findall(rb'[A-Za-z0-9_\-]+\.npk', original.read_bytes(), re.I)))
            if npk_names:
                result['npk_libraries'] = [{'fileName': name.decode('ascii'),
                                          'evidence': 'original binary filename'} for name in npk_names]
                result['status'] = 'skip_npk'
                return result
            result['status'] = 'unpack_failed'
            return result
        dependencies = read(workspace / 'project/.module.json').get('dependencies', [])
        result['npk_libraries'] = [d for d in dependencies if d.get('kind') == 'elib'
                                   and d.get('fileName', '').lower().endswith('.npk')]
        result['missing_libraries'] = [d for d in dependencies if d.get('kind') == 'elib'
            and not (d.get('resolvedPath') and Path(d['resolvedPath']).is_file())]
        result['unavailable_library_exports'] = [d for d in dependencies if d.get('kind') == 'elib'
            and d.get('resolvedPath') and Path(d['resolvedPath']).is_file()
            and not (d.get('localWorkspace') and (workspace / d['localWorkspace']).is_file())]
        result['placeholders'] = []
        for page in (workspace / 'src').rglob('*'):
            if page.suffix not in ('.txt', '.xml'):
                continue
            for line_number, line in enumerate(page.read_text(encoding='utf-8-sig').splitlines(), 1):
                tokens = re.findall(r'_Lib-?\d+(?:Cmd|Const|Type)\w*', line)
                if tokens:
                    result['placeholders'].append({'file': str(page.relative_to(workspace)),
                                                   'line': line_number, 'tokens': tokens, 'text': line})
        if result['npk_libraries']:
            result['status'] = 'skip_npk'
            return result
        if result['missing_libraries']:
            result['status'] = 'skip_missing_library'
            return result
        result['modified_pages'] = []
        # 对每个方法加入唯一注释，保证不是只改页尾或仅改变可忽略的空白。
        for page_index, path in enumerate(sorted((workspace / 'src').rglob('*.txt'))):
            text = path.read_text(encoding='utf-8-sig')
            marker = f"samples-audit-{index}-{page_index}"
            lines = text.splitlines()
            methods = [i for i, line in enumerate(lines) if line.startswith('.子程序 ')]
            if methods:
                for position in reversed(methods[1:] + [len(lines)]):
                    lines[position:position] = ["' " + marker, '']
            else:
                # 固定声明页与空程序集只增加空行，不改变声明内容。
                lines.append('')
            path.write_bytes(('\r\n'.join(lines) + '\r\n').encode('utf-8-sig'))
            result['modified_pages'].append({'path': str(path.relative_to(workspace)), 'marker': marker if methods else None,
                                              'methods': len(methods)})
        result['modified_forms'] = []
        for path in sorted((workspace / 'src').rglob('*.xml')):
            path.write_bytes(path.read_bytes() + b'\r\n')
            result['modified_forms'].append(str(path.relative_to(workspace)))
        result['architectures'] = {}
        for arch in ('Win32', 'x64'):
            arch_root = root / arch
            arch_root.mkdir()
            output = arch_root / 'edited.e'
            row = {'pack_exit': run([args.bin_root / f'{arch}/Release/e-packager.exe',
                                    'pack', workspace, output], arch_root / 'pack.log')}
            result['architectures'][arch] = row
            if row['pack_exit']:
                continue
            row['changed_bytes'] = digest(output) != result['sha256']
            decoded = arch_root / 'decoded'
            row['decode_exit'] = run([decoder, 'unpack', output, decoded, '--main-only'], arch_root / 'decode.log')
            row['mismatches'] = []
            if not row['decode_exit']:
                for folder in ('src', 'image', 'audio'):
                    expected_files = {p.relative_to(workspace) for p in (workspace / folder).rglob('*') if p.is_file()}
                    actual_files = {p.relative_to(decoded) for p in (decoded / folder).rglob('*') if p.is_file()}
                    for relative in expected_files | actual_files:
                        before, after = workspace / relative, decoded / relative
                        if not before.exists() or not after.exists():
                            row['mismatches'].append(str(relative))
                        elif folder == 'src':
                            if normalize(before.read_text(encoding='utf-8-sig')) != normalize(after.read_text(encoding='utf-8-sig')):
                                row['mismatches'].append(str(relative))
                        elif relative.name == 'list.json':
                            if read(before) != read(after):
                                row['mismatches'].append(str(relative))
                        elif digest(before) != digest(after):
                            row['mismatches'].append(str(relative))
            # 旧 IDE 模块代理按工程所在目录查找 .ec；在输入副本旁编译以保留真实依赖环境。
            row['compile'] = compile_repacked(args, output, copied, arch_root, 'compile',
                                               blackmoon='asm' if index in args.blackmoon_ids else None)
        result['baseline_compile'] = compile_file(args, copied, root, 'baseline-compile',
                                                 blackmoon='asm' if index in args.blackmoon_ids else None)
        result['status'] = classify(result)
    except Exception as exc:
        result['status'] = 'audit_error'
        result['error'] = repr(exc)
    finally:
        result['original_unchanged'] = digest(original) == result['sha256']
        save(root / 'result.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    repo = Path(__file__).resolve().parent.parent
    parser.add_argument('--samples', type=Path, default=repo / 'eproj/samples')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ide', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--ids', help='只运行逗号分隔的样例编号，编号仍按完整文件列表计算')
    parser.add_argument('--bin-root', type=Path, default=repo / 'bin')
    parser.add_argument('--merge-audits', type=Path, nargs='+', help='按先后顺序合并审计结果，保留实际日志目录')
    parser.add_argument('--blackmoon-ids', default='', help='明确使用黑月汇编模式的样例编号')
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--retry-compile-failures', action='store_true',
                        help='保留首次结果，在已有审计目录中用独立 TEMP 重试失败的编译')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.bin_root = args.bin_root.resolve()
    args.blackmoon_ids = {int(i) for i in args.blackmoon_ids.split(',') if i}
    if args.merge_audits:
        args.output.mkdir(parents=True, exist_ok=False)
        merged = {}
        for audit_root in args.merge_audits:
            for result in read(audit_root / 'results.json'):
                result['audit_origin'] = str(audit_root.resolve())
                merged[result['id']] = result
        results = sorted(merged.values(), key=lambda r: r['id'])
        save(args.output / 'results.json', results)
        save(args.output / 'summary.json', {'total': len(results),
             'statuses': dict(Counter(r['status'] for r in results)),
             'originals_unchanged': all(digest(Path(r['source'])) == r['sha256'] for r in results),
             'audit_origins': [str(p.resolve()) for p in args.merge_audits]})
        write_report(args.output, results)
        return
    if args.retry_compile_failures:
        results = read(args.output / 'results.json')
        selected = {int(i) for i in args.ids.split(',')} if args.ids else None
        for result in results:
            if selected is not None and result['id'] not in selected:
                continue
            root = Path(result['directory'])
            copied = root.parent / 'inputs' / Path(result['source']).relative_to(args.samples)
            cases = []
            for arch, row in result.get('architectures', {}).items():
                if not row.get('compile'):
                    continue
                cases.append((row, root / arch / 'edited.e', root / arch, 'compile'))
            if result.get('baseline_compile'):
                cases.append(({'compile': result['baseline_compile'],
                               'compile_history': list(result.get('baseline_compile_history', []))},
                              copied,
                              root, 'baseline-compile'))
            for row, source, case_root, label in cases:
                if row['compile']['passed']:
                    continue
                row.setdefault('initial_compile', row['compile'])
                row.setdefault('compile_history', []).append(row['compile'])
                retry_number = len(row.get('compile_history', []))
                mode = 'asm' if result['id'] in args.blackmoon_ids else None
                if label == 'baseline-compile':
                    row['compile'] = compile_file(args, source, case_root, label + f'-retry-{retry_number}', blackmoon=mode)
                else:
                    row['compile'] = compile_repacked(args, source, copied, case_root, label + f'-retry-{retry_number}', blackmoon=mode)
                if label == 'baseline-compile':
                    result.setdefault('initial_baseline_compile', result['baseline_compile'])
                    result.setdefault('baseline_compile_history', []).append(result['baseline_compile'])
                    result['baseline_compile'] = row['compile']
                print(f"RETRY {result['id']} {label} {row['compile']['passed']}", flush=True)
            result['status'] = classify(result)
            save(root / 'result.json', result)
            save(args.output / 'results.json', results)
        summary = read(args.output / 'summary.json')
        summary['statuses'] = dict(Counter(r['status'] for r in results))
        save(args.output / 'summary.json', summary)
        write_report(args.output, results)
        return
    args.output.mkdir(parents=True, exist_ok=False)
    save(args.output / 'packager-builds.json', {
        str(p): digest(p) for p in (
            args.bin_root / 'Win32/Release/e-packager.exe',
            args.bin_root / 'x64/Release/e-packager.exe',
            args.bin_root / 'x64/Release/e-packager-x86.exe') if p.is_file()})
    copied_root = args.output / 'inputs'
    shutil.copytree(args.samples, copied_root)
    files = sorted(args.samples.rglob('*.e'))
    save(args.output / 'manifest.json', [{'path': str(p), 'sha256': digest(p)} for p in files])
    selected = {int(i) for i in args.ids.split(',')} if args.ids else None
    results = []
    started = time.time()
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(audit, args, i, p, copied_root / p.relative_to(args.samples),
                               args.output / f'{i:03}', repo) for i, p in enumerate(files, 1) if selected is None or i in selected]
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            save(args.output / 'results.json', sorted(results, key=lambda x: x['id']))
            print(f"{len(results)}/{len(futures)} {result['status']} {result['source']}", flush=True)
    save(args.output / 'summary.json', {'total': len(results), 'seconds': time.time() - started,
        'statuses': {s: sum(r['status'] == s for r in results) for s in sorted({r['status'] for r in results})},
        'originals_unchanged': all(digest(Path(m['path'])) == m['sha256'] for m in read(args.output / 'manifest.json'))})
    write_report(args.output, sorted(results, key=lambda r: r['id']))


if __name__ == '__main__':
    main()

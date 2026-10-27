"""在独立副本中审计 .e/.ec 的双架构无修改回包与页尾注释语义回包。"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import ctypes
from datetime import datetime
import difflib
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile
import time
import traceback


def save(path, value):
    path.write_bytes((json.dumps(value, ensure_ascii=False, indent=2) + '\r\n').encode('utf-8-sig'))


def hashes(path):
    data = path.read_bytes()
    return {'size': len(data), 'md5': hashlib.md5(data).hexdigest(),
            'sha256': hashlib.sha256(data).hexdigest()}


def decode(data):
    try:
        return data.decode('utf-8-sig')
    except UnicodeDecodeError:
        return data.decode('gb18030', errors='replace')


def invoke(command, root, label, timeout):
    """保留原始输出，限制时间并关闭标准输入，避免密码提示或崩溃窗口阻塞。"""
    log = root / (label + '.log')
    started = time.monotonic()
    timed_out = False
    with log.open('wb') as stream:
        process = subprocess.Popen([str(x) for x in command], cwd=root,
                                   stdin=subprocess.DEVNULL, stdout=stream,
                                   stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
            process.kill() if process.poll() is None else None
            code = process.wait(timeout=15)
    output = decode(log.read_bytes())
    status = 'timeout' if timed_out else 'crash' if code & 0x80000000 else 'ok' if code == 0 else 'error'
    row = {'command': [str(x) for x in command], 'pid': process.pid, 'cwd': str(root), 'exit': code,
           'exit_hex': f'0x{code & 0xffffffff:08X}', 'status': status,
           'seconds': round(time.monotonic() - started, 3), 'log': str(log),
           'diagnostic': output[-12000:] if status != 'ok' else ''}
    row['property_failures'] = [line for line in output.splitlines()
                               if 'control_property_decode_failed:' in line]
    row['unexamined_statements'] = [line for line in output.splitlines()
                                    if 'source_preserved_unexamined:' in line]
    save(root / (label + '.command.json'), row)
    return row


def normalize(text):
    # 忽略编辑器排版差异；不忽略注释、语句、声明、XML 属性或资源索引。
    return '\n'.join(line.strip() for line in text.splitlines() if line.strip())


def inventory(workspace, preserve_blank_lines=False):
    result = {}
    for folder in ('src', 'image', 'audio'):
        for path in sorted((workspace / folder).rglob('*')):
            if not path.is_file():
                continue
            key = path.relative_to(workspace).as_posix()
            data = path.read_bytes()
            if folder == 'src':
                text = data.decode('utf-8-sig')
                value = text.replace('\r\n', '\n').replace('\r', '\n') if preserve_blank_lines else normalize(text)
                result[key] = {'kind': 'text', 'value': value}
            elif path.name == 'list.json':
                result[key] = {'kind': 'index', 'value': json.loads(data.decode('utf-8-sig'))}
            else:
                result[key] = {'kind': 'binary', 'value': hashlib.sha256(data).hexdigest()}
    return result


def unresolved_tokens(workspace):
    """记录原始行号，同时检查声明和窗口 XML 中的支持库占位符。"""
    found = []
    for path in sorted((workspace / 'src').rglob('*')):
        if path.is_file() and path.suffix.lower() in ('.txt', '.xml'):
            for number, line in enumerate(path.read_text(encoding='utf-8-sig').splitlines(), 1):
                tokens = re.findall(r'_Lib[\w-]*', line)
                if tokens:
                    found.append({'path': path.relative_to(workspace).as_posix(),
                                  'line': number, 'tokens': tokens, 'text': line})
    return found


def compare(expected, actual, root):
    differences = []
    patches = []
    for path in sorted(expected.keys() | actual.keys()):
        left, right = expected.get(path), actual.get(path)
        if left == right:
            continue
        kind = 'added' if left is None else 'missing' if right is None else left['kind']
        differences.append({'path': path, 'kind': kind})
        if left and right and kind == 'text':
            patches.extend(list(difflib.unified_diff(left['value'].splitlines(),
                right['value'].splitlines(), fromfile='expected/' + path,
                tofile='actual/' + path, lineterm=''))[:160])
        else:
            patches.append(f'{kind}: {path}')
    if differences:
        (root / 'content.diff.txt').write_bytes(('\r\n'.join(patches) + '\r\n').encode('utf-8-sig'))
    return differences


def append_comments(workspace, index):
    modified = []
    for path in sorted((workspace / 'src').rglob('*.txt')):
        data = path.read_bytes()
        text = data.decode('utf-8-sig')
        if not re.search(r'^\s*\.(?:程序集|类)(?:\s|$)', text, re.M):
            continue
        marker = f"' module-roundtrip-audit-{index:04}-{len(modified):04}"
        text = text.replace('\r\n', '\n').replace('\r', '\n')
        text += ('' if text.endswith('\n') else '\n') + marker + '\n'
        path.write_bytes(text.replace('\n', '\r\n').encode('utf-8-sig'))
        modified.append({'path': path.relative_to(workspace).as_posix(), 'marker': marker,
                         'methods': len(re.findall(r'^\s*\.子程序(?:\s|$)', text, re.M)),
                         'before_sha256': hashlib.sha256(data).hexdigest(),
                         'after_sha256': hashes(path)['sha256']})
    return modified


def decode_in_source_directory(exe, edited, source, decoded, root, label, timeout):
    # 支持库可能就在原工程旁；移到证据目录拆包会误加载另一个同名库。
    # 使用输入副本旁的唯一临时文件，不覆盖任何原有文件。
    with tempfile.NamedTemporaryFile(prefix='__roundtrip_audit_', suffix='.e',
                                     dir=source.parent, delete=False) as stream:
        staged = Path(stream.name)
        stream.write(edited.read_bytes())
    try:
        return invoke([exe, 'unpack', staged, decoded, '--main-only'], root, label, timeout)
    finally:
        staged.unlink()


def audit(args, item, arch):
    root = args.output / 'cases' / f'{item["id"]:04}' / arch
    root.mkdir(parents=True)
    row = {'id': item['id'], 'source': item['relative'], 'arch': arch,
           'directory': str(root), 'issues': [], 'stages': {}}
    exe = args.bin_root / arch / 'Release/e-packager.exe'
    source = args.output / 'inputs' / item['relative']
    workspace = root / 'workspace'
    def stage(label, command):
        result = invoke([exe] + command, root, label, args.timeout)
        row['stages'][label] = result
        if result['status'] != 'ok':
            row['issues'].append(label + '_' + result['status'])
        if result['property_failures']:
            row['issues'].append(label + '_property_decode_failed')
        if result['unexamined_statements']:
            row['issues'].append(label + '_unexamined_statements')
        return result['status'] == 'ok'
    try:
        if not item['size']:
            row['status'] = 'skip_empty'
            return row
        if hashes(source) != {key: item[key] for key in ('size', 'md5', 'sha256')}:
            raise RuntimeError('input copy hash mismatch')
        if not stage('unpack', ['unpack', source, workspace]):
            return row
        placeholders = unresolved_tokens(workspace)
        row['unresolved_tokens'] = placeholders
        if placeholders:
            row['issues'].append('unpack_unresolved_tokens')
        dependency_file = workspace / 'project/.module.json'
        if dependency_file.exists():
            row['dependencies'] = json.loads(dependency_file.read_text(encoding='utf-8-sig')).get('dependencies', [])
        baseline = root / ('baseline' + source.suffix.lower())
        if stage('baseline_pack', ['pack', workspace, baseline]):
            row['baseline_hashes'] = hashes(baseline)
            row['baseline_identical'] = baseline.read_bytes() == source.read_bytes()
            if not row['baseline_identical']:
                row['issues'].append('baseline_bytes_changed')
        row['modified_pages'] = append_comments(workspace, item['id'])
        if not row['modified_pages']:
            row['issues'].append('no_assembly_or_class_to_edit')
            return row
        expected = inventory(workspace, getattr(args, 'preserve_blank_lines', False))
        edited = root / 'edited.e'
        if not stage('semantic_pack', ['pack', workspace, edited]):
            return row
        row['edited_hashes'] = hashes(edited)
        if not edited.stat().st_size:
            row['issues'].append('semantic_empty_output')
            return row
        row['edited_bytes_changed'] = edited.read_bytes() != source.read_bytes()
        if not row['edited_bytes_changed']:
            row['issues'].append('semantic_unchanged_bytes')
        decoded = root / 'decoded'
        decoded_stage = decode_in_source_directory(exe, edited, source, decoded, root, 'reunpack', args.timeout)
        row['stages']['reunpack'] = decoded_stage
        if decoded_stage['property_failures']:
            row['issues'].append('reunpack_property_decode_failed')
        if decoded_stage['status'] != 'ok':
            row['issues'].append('reunpack_' + decoded_stage['status'])
            return row
        actual = inventory(decoded, getattr(args, 'preserve_blank_lines', False))
        row['reunpack_unresolved_tokens'] = unresolved_tokens(decoded)
        if row['reunpack_unresolved_tokens']:
            row['issues'].append('reunpack_unresolved_tokens')
        row['differences'] = compare(expected, actual, root)
        row['missing_markers'] = [page['path'] for page in row['modified_pages']
            if page['marker'] not in actual.get(page['path'], {}).get('value', '')]
        if row['missing_markers']:
            row['issues'].append('comment_lost')
        if row['differences']:
            row['issues'].append('content_changed')
        stage('second_pack', ['pack', decoded, root / 'second.e'])
    except Exception:
        row['issues'].append('audit_error')
        row['error'] = traceback.format_exc()
    finally:
        row.setdefault('status', 'passed' if not row['issues'] else 'failed')
        save(root / 'result.json', row)
    return row


def report(args, manifest, rows, unchanged):
    summary = {'files': len(manifest), 'architecture_runs': len(rows),
               'statuses': dict(Counter(r['status'] for r in rows)),
               'issues': dict(Counter(i for r in rows for i in r['issues'])),
               'originals_unchanged': unchanged, 'architectures': {}}
    lines = ['# 工程目录拆包与语义回包审计', '',
             f'- 输入：`{args.source}`', f'- 证据目录：`{args.output}`',
             f'- 文件数：{len(manifest)}；每条命令超时：{args.timeout} 秒。',
             '- 每个类／程序集页底部追加一行唯一注释；保留原生元数据。',
             '- 未修改回包逐字节、MD5、SHA256 比较；修改回包再次拆包，比较源码、窗口、资源及注释。',
             ('- 源码仅统一换行符，逐字比较并保留空行和缩进。' if args.preserve_blank_lines else
              '- 源码比较仅忽略空行和行首尾空白。') + '资源二进制 SHA256 比较，索引 JSON 结构比较。',
             '- 再拆包临时文件位于输入副本旁，以保留工程目录的支持库查找上下文。',
             '- 不手动删除快照；页尾注释改变源码摘要，回包器保留声明身份并禁止复用旧执行代码。',
             '- 首次和再次拆包均扫描源码/XML 的 `_Lib*`，记录原始行号；控件属性工作进程失败单列为问题。',
             '- .ec 原样回包输出 .ec；修改后输出可编辑 .e，重新编译成 .ec 不属于本轮自动审计。',
             '- 本次不执行 IDE 编译；拆包与内容比较通过不代表 IDE 编译或运行正确。',
             f'- 原文件内容复核：{unchanged}。', '',
             '| 架构 | 总数 | 拆包成功 | 原样回包字节一致 | 修改回包成功 | 再拆包成功 | 二次回包成功 | 无记录问题 |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for arch in args.architectures:
        selected = [r for r in rows if r['arch'] == arch]
        counts = {'total': len(selected), 'passed': sum(r['status'] == 'passed' for r in selected),
                  'baseline_identical': sum(r.get('baseline_identical', False) for r in selected)}
        for stage in ('unpack', 'semantic_pack', 'reunpack', 'second_pack'):
            counts[stage] = sum(r['stages'].get(stage, {}).get('status') == 'ok' for r in selected)
        summary['architectures'][arch] = counts
        lines.append(f'| {arch} | {counts["total"]} | {counts["unpack"]} | {counts["baseline_identical"]} | '
                     f'{counts["semantic_pack"]} | {counts["reunpack"]} | {counts["second_pack"]} | {counts["passed"]} |')
    lines += ['', '## 问题计数（可重叠）', '']
    lines += [f'- `{key}`：{value}' for key, value in summary['issues'].items()]
    lines += ['', '## 逐例问题', '', '| ID | 架构 | 相对路径 | 问题 | 证据 |', '| --- | --- | --- | --- | --- |']
    for row in rows:
        if row['status'] != 'passed':
            path = row['source'].replace('|', '&#124;')
            evidence = (Path(row['directory']) / 'result.json').as_posix()
            lines.append(f'| {row["id"]:04} | {row["arch"]} | {path} | {", ".join(row["issues"]) or row["status"]} | [JSON]({evidence}) |')
    save(args.output / 'results.json', rows)
    save(args.output / 'summary.json', summary)
    (args.output / 'report.md').write_bytes(('\r\n'.join(lines) + '\r\n').encode('utf-8-sig'))
    return summary


def recheck_context(args):
    """对早期把产物移出工程目录的审计补验，保留首次结果及日志。"""
    metadata = json.loads((args.output / 'run.json').read_text(encoding='utf-8-sig'))
    args.source = Path(metadata['source'])
    args.preserve_blank_lines = metadata.get('preserve_blank_lines', False)
    manifest = json.loads((args.output / 'manifest.json').read_text(encoding='utf-8-sig'))
    rows = json.loads((args.output / 'results.json').read_text(encoding='utf-8-sig'))
    for arch in args.architectures:
        if hashes(args.bin_root / arch / 'Release/e-packager.exe')['sha256'] != metadata['binaries'][arch]['sha256']:
            raise RuntimeError('补验必须使用与首次审计一致的工具')
    def check(row):
        if not row.get('edited_hashes') or row['stages']['semantic_pack']['status'] != 'ok':
            return row
        root = Path(row['directory'])
        if (root / 'decoded-context').exists():
            raise RuntimeError(f'补验输出已存在：{root}')
        row['initial_decode'] = {key: row.get(key) for key in ('issues', 'status', 'differences', 'missing_markers')}
        row['initial_decode']['stage'] = row['stages'].get('reunpack')
        row['issues'] = [i for i in row['issues'] if i not in ('comment_lost', 'content_changed') and not i.startswith('reunpack_')]
        exe = args.bin_root / row['arch'] / 'Release/e-packager.exe'
        source = args.output / 'inputs' / row['source']
        decoded = root / 'decoded-context'
        stage = decode_in_source_directory(exe, root / 'edited.e', source, decoded, root, 'reunpack-context', args.timeout)
        row['stages']['reunpack'] = stage
        if stage['status'] != 'ok':
            row['issues'].append('reunpack_' + stage['status'])
        else:
            actual = inventory(decoded, args.preserve_blank_lines)
            row['differences'] = compare(inventory(root / 'workspace', args.preserve_blank_lines), actual, decoded)
            row['missing_markers'] = [p['path'] for p in row['modified_pages']
                if p['marker'] not in actual.get(p['path'], {}).get('value', '')]
            if row['missing_markers']:
                row['issues'].append('comment_lost')
            if row['differences']:
                row['issues'].append('content_changed')
        row['status'] = 'failed' if row['issues'] else 'passed'
        save(root / 'result.json', row)
        print(f'context {row["id"]:04} {row["arch"]} {row["status"]}', flush=True)
        return row
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        rows = list(pool.map(check, rows))
    changed = [m['relative'] for m in manifest if hashes(args.source / m['relative']) !=
               {key: m[key] for key in ('size', 'md5', 'sha256')}]
    metadata['context_recheck'] = {'finished': datetime.now().isoformat(),
        'script_sha256': hashes(Path(__file__))['sha256'], 'changed_originals': changed}
    save(args.output / 'run.json', metadata)
    summary = report(args, manifest, rows, not changed)
    print(json.dumps(summary, ensure_ascii=False), flush=True)
    return 1 if changed or any(r['status'] == 'failed' for r in rows) else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('C:/Users/aiqin/OneDrive/Code/模块'))
    parser.add_argument('--output', type=Path, required=True, help='必须为不存在的新目录')
    parser.add_argument('--bin-root', type=Path, default=Path(__file__).resolve().parent.parent / 'bin')
    parser.add_argument('--architectures', nargs='+', choices=['Win32', 'x64'], default=['Win32', 'x64'])
    parser.add_argument('--ids', help='完整排序列表中的编号，逗号分隔；用于复现')
    parser.add_argument('--sample-per-extension', type=int, help='每种扩展名随机抽取指定数量的非空文件')
    parser.add_argument('--seed', type=int, default=20261027, help='随机抽样种子，写入审计记录')
    parser.add_argument('--preserve-blank-lines', action='store_true', help='源码逐字比较，仅统一换行符')
    parser.add_argument('--extensions', nargs='+', choices=['.e', '.ec'], default=['.e'],
                        help='选择待测扩展名；默认仅 .e，以保持既有审计编号')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--support-library', type=Path, action='append', default=[],
                        help='仅复制到审计副本的支持库')
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--recheck-decode-context', action='store_true',
                        help='在已有完整审计上补验输入副本目录内的再拆包；保留首次结果')
    args = parser.parse_args()
    args.source, args.output, args.bin_root = (p.resolve() for p in (args.source, args.output, args.bin_root))
    if args.output == args.source or args.output.is_relative_to(args.source):
        parser.error('输出目录不能位于原始输入目录中')
    if args.workers < 1 or args.timeout < 1:
        parser.error('workers 和 timeout 必须为正数')
    if args.sample_per_extension is not None and (args.sample_per_extension < 1 or args.ids):
        parser.error('sample-per-extension 必须为正数，且不能与 ids 同用')
    binaries = {arch: args.bin_root / arch / 'Release/e-packager.exe' for arch in args.architectures}
    for exe in binaries.values():
        if not exe.is_file():
            parser.error(f'未找到构建产物：{exe}')
    if os.name == 'nt':
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    if args.recheck_decode_context:
        return recheck_context(args)
    args.output.mkdir(parents=True, exist_ok=False)
    originals = sorted((p for p in args.source.rglob('*') if p.is_file() and p.suffix.lower() in args.extensions),
                       key=lambda p: p.relative_to(args.source).as_posix().casefold())
    ids = {int(i) for i in args.ids.split(',')} if args.ids else None
    if args.sample_per_extension:
        rng = random.Random(args.seed)
        ids = set()
        for extension in dict.fromkeys(args.extensions):
            candidates = [i for i, p in enumerate(originals, 1)
                          if p.suffix.lower() == extension and p.stat().st_size > 0
                          and 'kanec' not in p.name.casefold()]
            if len(candidates) < args.sample_per_extension:
                parser.error(f'{extension} 非空文件不足 {args.sample_per_extension} 个')
            ids.update(rng.sample(candidates, args.sample_per_extension))
    manifest = [dict(id=i, relative=p.relative_to(args.source).as_posix(), **hashes(p))
                for i, p in enumerate(originals, 1) if ids is None or i in ids]
    if not manifest:
        parser.error('没有选中的工程文件')
    save(args.output / 'manifest.json', manifest)
    metadata = {'started': datetime.now().isoformat(), 'source': str(args.source),
                'timeout': args.timeout, 'workers': args.workers, 'extensions': args.extensions,
                'sample_per_extension': args.sample_per_extension, 'seed': args.seed,
                'sample_excluded_name_contains': ['KanEc'] if args.sample_per_extension else [],
                'preserve_blank_lines': args.preserve_blank_lines,
                'binaries': {arch: dict(path=str(exe), **hashes(exe)) for arch, exe in binaries.items()},
                'script_sha256': hashes(Path(__file__))['sha256']}
    revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=Path(__file__).resolve().parent.parent,
                              capture_output=True, text=True)
    metadata['git_commit'] = revision.stdout.strip() if revision.returncode == 0 else None
    save(args.output / 'run.json', metadata)
    print(f'Copying source tree, selected {len(manifest)} project files', flush=True)
    # 整棵依赖树只复制一次，保留相对 .ec 等路径；工具只收到副本路径。
    shutil.copytree(args.source, args.output / 'inputs')
    if args.support_library:
        library_dir = args.output / 'inputs/lib'
        library_dir.mkdir(exist_ok=True)
        metadata['staged_support_libraries'] = []
        for library in args.support_library:
            destination = library_dir / library.name
            if destination.exists() and destination.read_bytes() != library.read_bytes():
                raise RuntimeError(f'支持库重名冲突：{destination}')
            shutil.copy2(library, destination)
            metadata['staged_support_libraries'].append(dict(path=str(library.resolve()), **hashes(library)))
        save(args.output / 'run.json', metadata)
    rows = []
    total = len(manifest) * len(args.architectures)
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(audit, args, item, arch) for item in manifest for arch in args.architectures]
        for future in as_completed(futures):
            row = future.result()
            rows.append(row)
            print(f'{len(rows)}/{total} {row["id"]:04} {row["arch"]} {row["status"]} {",".join(row["issues"])}', flush=True)
            with (args.output / 'progress.jsonl').open('a', encoding='utf-8') as stream:
                stream.write(json.dumps(row, ensure_ascii=False) + '\n')
    rows.sort(key=lambda r: (r['id'], r['arch']))
    changed = [item['relative'] for item in manifest if hashes(args.source / item['relative']) !=
               {key: item[key] for key in ('size', 'md5', 'sha256')}]
    metadata.update(finished=datetime.now().isoformat(), changed_originals=changed)
    metadata['changed_binaries'] = [arch for arch, exe in binaries.items()
        if hashes(exe)['sha256'] != metadata['binaries'][arch]['sha256']]
    save(args.output / 'run.json', metadata)
    summary = report(args, manifest, rows, not changed)
    print(json.dumps(summary, ensure_ascii=False), flush=True)
    return 1 if changed or metadata['changed_binaries'] or any(r['status'] == 'failed' for r in rows) else 0


if __name__ == '__main__':
    raise SystemExit(main())

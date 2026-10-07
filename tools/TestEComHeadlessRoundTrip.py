"""用真实 EC 依赖验证双架构回包、连续 update 和 IDE 无头编译。"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from TestEComUpdateRoundTrip import read_json, run, source_text, unpack, write


def compile_project(launcher, ide, source, artifact, target):
    report = artifact.with_suffix(artifact.suffix + '.json')
    result = subprocess.run([
        str(launcher), 'headless-compile', str(ide), str(source), str(artifact),
        '--target', target, '--result', str(report), '--timeout', '60',
    ], capture_output=True, timeout=110)
    artifact.with_suffix(artifact.suffix + '.log').write_bytes(result.stdout + result.stderr)
    assert report.exists(), f'Missing compile report: {report}'
    data = read_json(report)
    assert result.returncode == 0 and data.get('ok'), f'Headless compile failed: {report}'
    assert data.get('compile_result', {}).get('artifact_verified'), f'Unverified artifact: {report}'
    assert artifact.is_file() and artifact.stat().st_size > 0, f'Empty artifact: {artifact}'
    print(f'PASS headless {artifact.name} ({artifact.stat().st_size} bytes)', flush=True)


def main():
    sys.stdout.reconfigure(errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ide', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    output = (args.output or repo / 'temp' / f'issue9-headless-{uuid.uuid4().hex}').resolve()
    output.mkdir(parents=True, exist_ok=False)
    decoder = repo / 'bin/Win32/Release/e-packager.exe'
    original = output / 'original.e'
    shutil.copy2(repo / 'eproj/e-console-exe-new-proj.e', original)
    module_workspace = output / 'module-workspace'
    unpack(decoder, original, module_workspace)
    # 控制台模板转换为 EC 前，IDE 仍要求有合法启动子程序。
    write(module_workspace / 'src/程序集1.txt', '''.版本 2
.程序集 公共库
.子程序 _启动子程序, 整数型
返回 (0)
.子程序 公开取值, 整数型, 公开
返回 (9)
.子程序 私有取值, 整数型
返回 (8)
''')
    module_source = output / 'module.e'
    module = output / 'module.ec'
    run(decoder, 'pack', module_workspace, module_source)
    compile_project(args.launcher, args.ide, module_source, module, 'ecom')
    exports = output / 'module-decoded'
    unpack(decoder, module, exports)
    header = (exports / 'header/header.txt').read_text(encoding='utf-8-sig')
    assert '公开取值' in header and '私有取值' not in header

    summaries = []
    for arch in ('Win32', 'x64'):
        tool = repo / f'bin/{arch}/Release/e-packager.exe'
        root = output / arch
        root.mkdir()
        workspace = root / 'workspace'
        unpack(decoder, original, workspace)
        run(tool, 'update', workspace, '--add-ecom', module)
        main_page = '.版本 2\n.程序集 程序集1\n.子程序 _启动子程序, 整数型\n返回 (公开取值 ())\n'
        write(workspace / 'src/程序集1.txt', main_page)
        baseline = root / 'baseline.e'
        run(tool, 'pack', workspace, baseline)
        baseline_exe = root / 'baseline.exe'
        compile_project(args.launcher, args.ide, baseline, baseline_exe, 'win_console_exe')

        edited_workspace = root / 'edited-workspace'
        unpack(decoder, baseline, edited_workspace)
        unchanged = root / 'unchanged.e'
        run(tool, 'pack', edited_workspace, unchanged)
        assert baseline.read_bytes() == unchanged.read_bytes(), 'Baseline roundtrip changed bytes'
        write(edited_workspace / 'src/新增页.txt', '''.版本 2
.程序集 新增页
.子程序 修改取值, 整数型, 公开
返回 (33)
''')
        write(edited_workspace / 'src/程序集1.txt', main_page.replace('公开取值 ()', '公开取值 () + 修改取值 ()'))
        for _ in range(2):
            run(tool, 'update', edited_workspace)
        edited = root / 'edited.e'
        run(tool, 'pack', edited_workspace, edited)
        assert edited.read_bytes() != baseline.read_bytes(), 'Stale native snapshot reused'
        decoded = root / 'decoded'
        unpack(decoder, edited, decoded)
        actual = source_text(decoded).replace('＋', '+')
        assert '公开取值 () + 修改取值 ()' in actual and '返回 (33)' in actual
        edited_exe = root / 'edited.exe'
        compile_project(args.launcher, args.ide, edited, edited_exe, 'win_console_exe')
        compile_project(args.launcher, args.ide, edited, root / 'edited.ec', 'ecom')
        # 再解包 IDE 编译出的 EC，确认新增公开子程序确实进入模块导出表。
        compiled_module = root / 'compiled-module'
        unpack(decoder, root / 'edited.ec', compiled_module)
        exported_header = (compiled_module / 'header/header.txt').read_text(encoding='utf-8-sig')
        assert '修改取值' in exported_header, 'New public method missing from compiled EC'
        summaries.append(dict(architecture=arch, baseline_compiled=True,
                              exe_compiled=True, ec_compiled=True, changes_preserved=True,
                              new_method_exported=True))
        print(f'PASS {arch}: real EC import, update twice, EXE/EC compile, new export verified', flush=True)
    write(output / 'results.json', json.dumps(summaries, ensure_ascii=False, indent=2))
    print(f'Results: {output}', flush=True)


if __name__ == '__main__':
    main()

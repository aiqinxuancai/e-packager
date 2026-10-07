"""回归验证普通程序集导出，以及 update 后修改确实进入回包产物。"""
import argparse
import base64
import struct
import sys
import json
from pathlib import Path
import shutil
import subprocess
import uuid


def write(path, text):
    path.write_bytes(b'\xef\xbb\xbf' + text.replace('\r\n', '\n').replace('\n', '\r\n').encode())


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def run(tool, *args, succeeds=True):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True)
    output = (result.stdout + result.stderr).decode('mbcs', errors='replace')
    if succeeds:
        assert result.returncode == 0, output
    return result, output


def unpack(tool, source, workspace):
    # 原生支持库为 x86，统一用 x86 解码以保留命令名；回包分别验证两种架构。
    decoder = tool.parents[2] / 'Win32/Release/e-packager.exe'
    run(decoder, 'unpack', source, workspace)


def source_text(workspace):
    return '\n'.join(p.read_text(encoding='utf-8-sig') for p in (workspace / 'src').glob('*.txt'))


def check_update(tool, template, root, case):
    workspace = root / 'workspace'
    unpack(tool, template, workspace)
    snapshot = (workspace / 'project/.native_source.bin').read_bytes()
    marker = 'issue9_marker'
    if case == 'page':
        write(workspace / 'src/新增页.txt', f'.版本 2\n.程序集 新增页\n.子程序 {marker}, 整数型\n返回 (9)\n')
    elif case == 'comment':
        path = workspace / 'src/程序集1.txt'
        write(path, path.read_text(encoding='utf-8-sig') + f"\n' {marker}\n")
    elif case == 'constant':
        path = workspace / 'src/.常量.txt'
        write(path, (path.read_text(encoding='utf-8-sig').strip() or '.版本 2') + f'\n.常量 {marker}, 9\n')
    elif case == 'resource':
        resource = root / 'resource.bin'
        resource.write_bytes(bytes(range(256)))
        run(tool, 'update', workspace, '--add-image', f'{marker}={resource}')
    # 连续 update 也不能让已失效的快照重新有效。
    for _ in range(2):
        run(tool, 'update', workspace)
    if case != 'resource':
        assert (workspace / 'project/.native_source.bin').read_bytes() == snapshot
    output = root / 'packed.e'
    run(tool, 'pack', workspace, output)
    if case == 'unchanged':
        assert output.read_bytes() == template.read_bytes(), 'Unchanged update lost byte-identical roundtrip'
        return
    assert output.read_bytes() != template.read_bytes(), 'Edited source silently reused original bytes'
    decoded = root / 'decoded'
    unpack(tool, output, decoded)
    if case == 'resource':
        resources = read_json(decoded / 'image/list.json')['items']
        item = next(item for item in resources if item['logicalName'] == marker)
        assert (decoded / item['relativePath']).read_bytes() == bytes(range(256))
    else:
        assert marker in source_text(decoded), 'Edit missing from decoded output'


def check_dependency(tool, template, root):
    workspace = root / 'workspace'
    module = workspace / 'ecom/fixture'
    unpack(tool, template, workspace)
    unpack(tool, template, module)
    # 不使用程序集公开标记；普通程序集按子程序的公开属性导出。
    write(module / 'src/程序集1.txt', '''.版本 2
.程序集 公共库, , , 普通程序集
.子程序 公开取值, 整数型, 公开
返回 (9)
.子程序 私有取值, 整数型
返回 (8)
''')
    write(module / 'src/私有类.txt', '''.版本 2
.程序集 私有类, <对象>
.子程序 类内取值, 整数型, 公开
返回 (7)
''')
    metadata = workspace / 'project/.module.json'
    data = read_json(metadata)
    data['dependencies'].append(dict(kind='ecom', name='fixture', path='fixture.ec', localWorkspace='ecom/fixture'))
    write(metadata, json.dumps(data, ensure_ascii=False, indent=2))
    main = workspace / 'src/程序集1.txt'
    for name in ('公开取值', '私有取值', '类内取值'):
        write(main, f'.版本 2\n.程序集 程序集1\n.子程序 _启动子程序, 整数型\n返回 ({name} ())\n')
        output = root / f'{name}.e'
        result, diagnostic = run(tool, 'pack', workspace, output, succeeds=False)
        if name != '公开取值':
            assert result.returncode != 0 and 'function_not_found' in diagnostic, diagnostic
            continue
        assert result.returncode == 0, diagnostic
        decoded = root / 'decoded'
        unpack(tool, output, decoded)
        assert '公开取值 ()' in source_text(decoded)
        # 检查实际调用目标是导入方法，避免仅检查 pack 的成功状态。
        maps = read_json(decoded / 'project/.native_source_map.json')
        method = next(m for page in maps for m in page['methods'] if m['name'] == '_启动子程序')
        expression = base64.b64decode(method['expressionData'])
        assert expression[18] == 0x21 and struct.unpack_from('<h', expression, 23)[0] == -2


def main():
    sys.stdout.reconfigure(errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    output = (args.output or repo / 'temp' / f'issue9-{uuid.uuid4().hex}').resolve()
    output.mkdir(parents=True, exist_ok=False)
    original = output / 'original.e'
    shutil.copy2(repo / 'eproj/e-console-exe-new-proj.e', original)
    decoder = repo / 'bin/Win32/Release/e-packager.exe'
    seed = output / 'seed'
    unpack(decoder, original, seed)
    # 用仅含核心语法的原生基线，避免 x64 环境依赖 x86 支持库命令表。
    write(seed / 'src/程序集1.txt', '.版本 2\n.程序集 程序集1\n.子程序 _启动子程序, 整数型\n返回 (0)\n')
    template = output / 'baseline.e'
    run(decoder, 'pack', seed, template)
    failures = []
    for arch in ('Win32', 'x64'):
        tool = repo / f'bin/{arch}/Release/e-packager.exe'
        for case in ('unchanged', 'page', 'comment', 'constant', 'resource', 'dependency'):
            root = output / arch / case
            root.mkdir(parents=True)
            try:
                if case == 'dependency':
                    check_dependency(tool, template, root)
                else:
                    check_update(tool, template, root, case)
                print(f'PASS {arch} {case}', flush=True)
            except AssertionError as error:
                failures.append(f'{arch} {case}: {error}')
                print(f'FAIL {failures[-1]}', flush=True)
    print(f'Results: {output}; failures={len(failures)}')
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()

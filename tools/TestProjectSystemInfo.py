"""验证工程类型、系统信息、旧目录升级及真实 IDE 编译。"""
import argparse
import copy
import json
from pathlib import Path
import shutil
import struct
import sys
import uuid

from TestEComUpdateRoundTrip import read_json, run, source_text, write
from TestEComHeadlessRoundTrip import compile_project


def system_bytes(path):
    data = path.read_bytes()
    assert data[:8] == b'CNWTEPRG', path
    offset = 8
    while offset + 100 <= len(data):
        key = struct.unpack_from('<I', data, offset + 8)[0]
        size = struct.unpack_from('<i', data, offset + 56)[0]
        assert 0 <= size <= len(data) - offset - 100
        if key == 0x02007319:
            return data[offset + 100:offset + 100 + size]
        offset += 100 + size
    raise AssertionError(f'Missing system info: {path}')


def save_meta(workspace, meta):
    write(workspace / 'project/_meta.json', json.dumps(meta, ensure_ascii=False, indent=2))


def main():
    sys.stdout.reconfigure(errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module-source', type=Path, required=True)
    parser.add_argument('--legacy-tool', type=Path)
    parser.add_argument('--ide', type=Path)
    parser.add_argument('--launcher', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    assert bool(args.ide) == bool(args.launcher)
    repo = Path(__file__).resolve().parent.parent
    output = (args.output or repo / 'temp' / f'issue11-{uuid.uuid4().hex}').resolve()
    output.mkdir(parents=True, exist_ok=False)
    decoder = repo / 'bin/Win32/Release/e-packager.exe'
    original = output / 'module.e'
    shutil.copy2(args.module_source, original)
    expected = system_bytes(original)
    assert struct.unpack_from('<i', expected, 24)[0] == 1000
    results = []

    def passed(arch, name):
        results.append(dict(architecture=arch, case=name, passed=True))
        write(output / 'results.json', json.dumps(results, indent=2))
        print(f'PASS {arch}: {name}', flush=True)

    for arch in ('Win32', 'x64'):
        tool = repo / f'bin/{arch}/Release/e-packager.exe'
        root = output / arch
        root.mkdir()
        workspace = root / 'workspace'
        run(decoder, 'unpack', original, workspace)
        baseline = root / 'baseline.e'
        run(tool, 'pack', workspace, baseline)
        assert baseline.read_bytes() == original.read_bytes()
        passed(arch, 'unchanged-byte-identical')
        meta = read_json(workspace / 'project/_meta.json')
        page = workspace / meta['sourceFiles'][0]['relativePath']
        original_text = page.read_text(encoding='utf-8-sig')
        write(page, original_text + "\n' issue11-comment\n")
        edited = root / 'edited.e'
        run(tool, 'pack', workspace, edited)
        assert system_bytes(edited) == expected and edited.read_bytes() != original.read_bytes()
        decoded = root / 'decoded'
        run(decoder, 'unpack', edited, decoded)
        assert 'issue11-comment' in source_text(decoded)
        passed(arch, 'edited-module-system-info-preserved')

        resource = root / 'resource.bin'
        resource.write_bytes(b'issue11-resource')
        run(tool, 'update', workspace, '--add-image', f'issue11_resource={resource}')
        for _ in range(2):
            run(tool, 'update', workspace)
        updated = root / 'updated.e'
        run(tool, 'pack', workspace, updated)
        assert system_bytes(updated) == expected
        assert not (workspace / 'project/.native_source.bin').exists()
        passed(arch, 'resource-update-without-native-bytes')
        if args.ide:
            compile_project(args.launcher, args.ide, edited, root / 'edited.ec', 'auto')
            compile_project(args.launcher, args.ide, updated, root / 'updated.ec', 'auto')
            for artifact in ('edited.ec', 'updated.ec'):
                report = read_json(root / (artifact + '.json'))
                assert report['compile_result']['target'] == 'ecom', report
            passed(arch, 'headless-auto-detects-module')

        # 只改元数据也必须使快照失效，保留未知类型、版本和保留字节。
        metadata_ws = root / 'metadata'
        run(decoder, 'unpack', original, metadata_ws)
        pristine = read_json(metadata_ws / 'project/_meta.json')
        custom = copy.deepcopy(pristine)
        info = custom['systemInfo']
        info.update(compileMajor=4, compileMinor=9, compileType=1002,
                    reserved=[-1, 2, 3, 4, 5, 6, 7, 2147483647], extensionBytes=[0, 255, 127])
        save_meta(metadata_ws, custom)
        result_file = root / 'metadata.e'
        run(tool, 'pack', metadata_ws, result_file)
        actual = system_bytes(result_file)
        assert struct.unpack_from('<hh', actual) == (4, 9)
        assert struct.unpack_from('<i', actual, 24)[0] == 1002
        assert list(struct.unpack_from('<8i', actual, 28)) == info['reserved']
        assert actual[60:] == bytes(info['extensionBytes'])
        passed(arch, 'metadata-only-edit-invalidates-snapshot')

        for field, value in (('compileType', 2**40), ('compileType', True),
                             ('compileMinor', 32768), ('reserved', [0]),
                             ('extensionBytes', [256]), ('extensionBytes', 'bad')):
            invalid = copy.deepcopy(pristine)
            invalid['systemInfo'][field] = value
            save_meta(metadata_ws, invalid)
            result, diagnostic = run(tool, 'pack', metadata_ws, root / 'invalid.e', succeeds=False)
            assert result.returncode != 0 and 'project_system_info_invalid' in diagnostic
        invalid = copy.deepcopy(pristine)
        invalid['projectSubsystem'] = 'console'
        save_meta(metadata_ws, invalid)
        result, diagnostic = run(tool, 'pack', metadata_ws, root / 'invalid.e', succeeds=False)
        assert result.returncode != 0 and 'project_subsystem_not_applicable' in diagnostic
        passed(arch, 'invalid-metadata-and-module-subsystem-rejected')

        # 使用相同最小语义源码检查 DLL / EXE 类型及主动子系统转换。
        write(metadata_ws / 'src/公开接口.txt', '.版本 2\n.程序集 公开接口\n.子程序 Issue11Value, 整数型, 公开\n返回 (11)\n')
        for kind, subsystem in ((0, 'windows'), (1, 'console'), (2, 'unknown')):
            data = copy.deepcopy(pristine)
            data['systemInfo']['compileType'] = kind
            data['projectSubsystem'] = subsystem
            save_meta(metadata_ws, data)
            file = root / f'type-{kind}.e'
            run(tool, 'pack', metadata_ws, file)
            assert struct.unpack_from('<i', system_bytes(file), 24)[0] == kind
            if kind == 2 and args.ide:
                artifact = root / 'type-2.dll'
                compile_project(args.launcher, args.ide, file, artifact, 'auto')
                assert read_json(root / 'type-2.dll.json')['compile_result']['target'] == 'win_dll'
                passed(arch, 'headless-auto-detects-dll')
        data['systemInfo']['compileType'] = 1
        data['projectSubsystem'] = 'windows'
        save_meta(metadata_ws, data)
        converted = root / 'converted.e'
        run(tool, 'pack', metadata_ws, converted)
        assert struct.unpack_from('<i', system_bytes(converted), 24)[0] == 0
        passed(arch, 'dll-exe-types-and-explicit-subsystem-conversion')

        if args.legacy_tool:
            legacy = root / 'legacy'
            run(args.legacy_tool, 'unpack', original, legacy)
            old_meta = read_json(legacy / 'project/_meta.json')
            assert 'systemInfo' not in old_meta and 'nativeBundleDigestVersion' not in old_meta
            migrated = root / 'legacy.e'
            run(tool, 'pack', legacy, migrated)
            assert migrated.read_bytes() == original.read_bytes()
            run(tool, 'update', legacy)
            run(tool, 'pack', legacy, migrated)
            assert migrated.read_bytes() == original.read_bytes()
            old_page = legacy / old_meta['sourceFiles'][0]['relativePath']
            write(old_page, old_page.read_text(encoding='utf-8-sig') + "\n' legacy-edited\n")
            run(tool, 'update', legacy)
            run(tool, 'pack', legacy, migrated)
            assert system_bytes(migrated) == expected and migrated.read_bytes() != original.read_bytes()
            passed(arch, 'legacy-directory-upgrade-and-update')
    print(f'Results: {output}')


if __name__ == '__main__':
    main()

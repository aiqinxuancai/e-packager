"""回归验证覆盖解包清理旧源码/窗口，保留手工文件并保持字节往返。"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import shutil
import subprocess
import uuid

from TestEComUpdateRoundTrip import read_json, write


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', nargs='+', default=['Win32', 'x64'])
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    output = repo / 'temp' / f'repeated-unpack-{uuid.uuid4().hex}'
    output.mkdir(parents=True)
    print(output, flush=True)
    for arch in args.arch:
        tool = repo / f'bin/{arch}/Release/e-packager.exe'
        root = output / arch
        root.mkdir()
        sequence = 0

        def run(*arguments, error=None):
            nonlocal sequence
            sequence += 1
            result = subprocess.run([str(tool), *map(str, arguments)], capture_output=True, timeout=120)
            raw = result.stdout + result.stderr
            log = root / f'{sequence:02d}-{arguments[0]}.log'
            log.write_bytes(raw)
            assert (result.returncode == 0) if error is None else (result.returncode != 0 and error.encode() in raw), str(log)

        original = root / 'v2.e'
        shutil.copy2(repo / 'eproj/e-console-exe-new-proj.e', original)
        seed = root / 'seed'
        run('unpack', original, seed, '--main-only')
        write(seed / 'src/已删除程序集.txt', '.版本 2\n.程序集 已删除程序集\n.子程序 旧方法, 整数型\n返回 (17)\n')
        v1 = root / 'v1.e'
        run('pack', seed, v1)
        workspace = root / 'workspace'
        run('unpack', v1, workspace, '--main-only')
        old = read_json(workspace / 'project/_meta.json')
        stale = next(x for x in old['sourceFiles'] if x['logicalName'] == '已删除程序集')
        note = workspace / 'src/笔记.md'
        note.write_bytes(b'keep me')
        run('unpack', original, workspace, '--main-only')
        assert not (workspace / stale['relativePath']).exists(), 'Deleted assembly survived repeated unpack'
        assert note.read_bytes() == b'keep me'
        packed = root / 'roundtrip.e'
        run('pack', workspace, packed)
        assert packed.read_bytes() == original.read_bytes(), 'Unmodified roundtrip changed bytes'
        run('unpack', packed, root / 'check', '--main-only')
        assert all(x['logicalName'] != '已删除程序集' for x in read_json(root / 'check/project/_meta.json')['sourceFiles'])

        # 未登记的手工程序集跨覆盖解包保留，仍然能够自动发现回包。
        manual = workspace / 'src/手工新增.txt'
        write(manual, '.版本 2\n.程序集 手工新增\n.子程序 新方法, 整数型\n返回 (23)\n')
        run('unpack', original, workspace, '--main-only')
        assert manual.exists()
        run('pack', workspace, root / 'manual.e')
        run('unpack', root / 'manual.e', root / 'manual-check', '--main-only')
        assert any(x['logicalName'] == '手工新增' for x in read_json(root / 'manual-check/project/_meta.json')['sourceFiles'])

        # 窗口 XML 和关联程序集同样需要清理；兼容旧元数据目录。
        forms = root / 'forms'
        run('unpack', repo / 'eproj/e-window-exe-new-proj.e', forms, '--main-only')
        old_meta = forms / 'project/_meta.json'
        old_forms = read_json(old_meta)['formFiles']
        assert old_forms
        old_meta.rename(forms / 'src/_meta.json')
        run('unpack', original, forms, '--main-only')
        assert all(not (forms / x['relativePath']).exists() for x in old_forms)
        run('pack', forms, root / 'forms-roundtrip.e')
        assert (root / 'forms-roundtrip.e').read_bytes() == original.read_bytes()
        # 子目录中的旧路径应清理，已丢失的旧文件不阻止重新解包。
        nested = root / 'nested'
        run('unpack', v1, nested, '--main-only')
        meta_path = nested / 'project/_meta.json'
        meta = read_json(meta_path)
        entry = next(x for x in meta['sourceFiles'] if x['logicalName'] == '已删除程序集')
        before = nested / entry['relativePath']
        entry['relativePath'] = 'src/子目录/旧名字.txt'
        after = nested / entry['relativePath']
        after.parent.mkdir()
        before.rename(after)
        write(meta_path, json.dumps(meta, ensure_ascii=False))
        run('unpack', v1, nested, '--main-only')
        assert not after.exists() and before.exists()
        before.unlink()
        run('unpack', original, nested, '--main-only')

        # 元数据无效时拒绝覆盖；越界路径不允许参与删除。
        for invalid in ('{broken', json.dumps({'sourceFiles': [{'relativePath': 'src/../../outside.txt'}]})):
            meta_before = meta_path.read_bytes()
            write(meta_path, invalid)
            broken = meta_path.read_bytes()
            run('unpack', original, nested, '--main-only', error='previous_')
            assert meta_path.read_bytes() == broken
            meta_path.write_bytes(meta_before)

        # 删除失败必须返回错误并保留旧清单，解除文件占用后可重试成功。
        locked = root / 'locked'
        run('unpack', v1, locked, '--main-only')
        locked_meta = locked / 'project/_meta.json'
        old_bytes = locked_meta.read_bytes()
        blocked = locked / stale['relativePath']
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                        wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel32.CreateFileW(str(blocked), 0x80000000, 3, None, 3, 0, None)
        assert handle != wintypes.HANDLE(-1).value, ctypes.get_last_error()
        try:
            run('unpack', original, locked, '--main-only', error='remove_obsolete_source_file_failed')
            assert locked_meta.read_bytes() == old_bytes
            assert blocked.exists()
        finally:
            kernel32.CloseHandle(handle)
        run('unpack', original, locked, '--main-only')
        assert not blocked.exists()

        print(arch, 'PASS', flush=True)


if __name__ == '__main__':
    main()

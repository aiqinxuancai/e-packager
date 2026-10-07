"""回归原生事件双表合并、语义回包及重复 XML 检查；只操作临时副本。"""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET


RESOURCE = 0x04007319
EVENTS = 0x0A007319


class Reader:
    def __init__(self, data, offset=0):
        self.data, self.offset = data, offset

    def integer(self):
        value = struct.unpack_from('<i', self.data, self.offset)[0]
        self.offset += 4
        return value

    def skip(self, size):
        self.offset += size

    def text(self):
        self.offset = self.data.index(0, self.offset) + 1

    def dynamic(self):
        self.skip(self.integer())


def sections(data):
    assert data[:8] == b'CNWTEPRG'
    offset = 8
    while offset < len(data):
        header = bytearray(data[offset:offset + 100])
        assert struct.unpack_from('<I', header)[0] == 353465113
        key = struct.unpack_from('<I', header, 8)[0]
        size = struct.unpack_from('<i', header, 56)[0]
        yield key, header, data[offset + 100:offset + 100 + size]
        offset += 100 + size


def checksum(data):
    value = 0
    for i, byte in enumerate(data):
        value ^= byte << ((i % 4) * 8)
    return value


def emit_section(header, body):
    header = bytearray(header)
    struct.pack_into('<II', header, 52, checksum(body), len(body))
    struct.pack_into('<I', header, 4, checksum(header[8:]))
    return header + body


def resource_events(data, replacements=None):
    """按实际资源块边界读写事件，不依赖控件名称或事件类型。"""
    reader = Reader(data)
    count = reader.integer() // 8
    forms = [reader.integer() for _ in range(count)]
    reader.skip(count * 4)
    edits, bindings = [], []
    for form in forms:
        reader.skip(8)
        reader.dynamic()
        reader.dynamic()
        count = reader.integer()
        size_position = reader.offset
        size = reader.integer()
        block_start = reader.offset
        units = [reader.integer() for _ in range(count)]
        offsets_position = reader.offset
        offsets = [reader.integer() for _ in range(count)]
        payload = reader.offset
        new_items = []
        for unit, offset in zip(units, offsets):
            start = payload + offset
            item = Reader(data, start)
            length = item.integer()
            end = start + 4 + length
            kind = item.integer()
            item.skip(20)
            item.text()
            if kind == 65539:
                item.text()
                item.skip(12)
                item.text()
                method = item.integer()
                if method:
                    bindings.append((form, unit, 0, method))
                new_items.append(data[start:end])
                continue
            item.text()
            item.skip(28)
            item.skip(item.integer() * 4)
            item.dynamic()
            item.text()
            item.skip(12)
            event_position = item.offset
            events = [(item.integer(), item.integer()) for _ in range(item.integer())]
            bindings.extend((form, unit, event, method) for event, method in events)
            if replacements is None:
                new_items.append(data[start:end])
            else:
                events = replacements.get((form, unit), events)
                encoded = struct.pack('<i', len(events)) + b''.join(
                    struct.pack('<ii', *event) for event in events)
                rebuilt = bytearray(data[start:event_position] + encoded + data[item.offset:end])
                struct.pack_into('<i', rebuilt, 0, len(rebuilt) - 4)
                new_items.append(rebuilt)
        new_offsets, position = [], 0
        for item in new_items:
            new_offsets.append(position)
            position += len(item)
        block = data[block_start:offsets_position] + struct.pack('<' + 'i' * count, *new_offsets)
        block += b''.join(new_items)
        edits.append((size_position, block_start + size, struct.pack('<i', len(block)) + block))
        reader.offset = block_start + size
    for start, end, replacement in reversed(edits):
        data = data[:start] + replacement + data[end:]
    return bindings, data


def xml_events(directory):
    result = []
    for path in sorted((directory / 'src').rglob('*.xml')):
        for parent in ET.parse(path).iter():
            for child in parent:
                if child.tag.endswith('.事件'):
                    result.append((parent.get('名称'), child.get('索引'), child.get('处理器')))
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packager', type=Path, required=True, help='Win32 Release executable')
    parser.add_argument('--x64-packager', type=Path)
    parser.add_argument('--project', type=Path, default=Path('eproj/e-window-exe-full.e'))
    parser.add_argument('--repro', type=Path, help='可选：IDE 保存的 issue #31 实际复现文件')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='e-packager-events-') as temporary:
        root = Path(temporary)

        def run(executable, *arguments, success=True):
            result = subprocess.run([str(executable.resolve()), *map(str, arguments)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if success:
                assert result.returncode == 0, result.stdout.decode('utf-8', errors='replace')
            else:
                assert result.returncode != 0 and b'duplicate_event' in result.stdout, result.stdout

        original = args.project.read_bytes()
        native = list(sections(original))
        indexed = list(struct.iter_unpack('<iiii', next(b for k, _, b in native if k == EVENTS)))
        assert indexed, 'Sample needs indexed control events'
        baseline = root / 'baseline'
        run(args.packager, 'unpack', args.project.resolve(), baseline, '--main-only')
        expected = xml_events(baseline)
        for stale in [False, True]:
            replacements = {}
            for form, unit, event, method in indexed:
                replacements.setdefault((form, unit), []).append((event, 0x04FFFFFF if stale else method))
            fixture = bytearray(original[:8])
            for key, header, body in native:
                if key == RESOURCE:
                    _, body = resource_events(body, replacements)
                fixture += emit_section(header, body)
            source = root / f'dual-{stale}.e'
            source.write_bytes(fixture)
            for arch, executable in [('x86', args.packager), ('x64', args.x64_packager)]:
                if executable:
                    directory = root / f'dual-{stale}-{arch}'
                    run(executable, 'unpack', source, directory, '--main-only')
                    assert xml_events(directory) == expected
            if not stale:
                # 兼容仅有资源段事件表的旧工程，不能无条件丢弃资源事件。
                resource_only = root / 'resource-only.e'
                resource_only.write_bytes(original[:8] + b''.join(
                    emit_section(h, b) for k, h, b in sections(fixture) if k != EVENTS))
                legacy = root / 'resource-only'
                run(args.packager, 'unpack', resource_only, legacy, '--main-only')
                assert xml_events(legacy) == expected

        source = args.repro.resolve() if args.repro else root / 'dual-True.e'
        directory = root / 'rebuild'
        run(args.packager, 'unpack', source, directory, '--main-only')
        assert xml_events(directory) == expected
        code = directory / 'src' / '窗口程序集_启动窗口.txt'
        text = code.read_text(encoding='utf-8-sig')
        assert '标签1.标题' in text
        text = text.replace('标签1.标题', "' 事件绑定回归测试\n标签1.标题", 1)
        code.write_bytes(b'\xef\xbb\xbf' + text.replace('\r\n', '\n').replace('\n', '\r\n').encode())
        rebuilt = root / 'rebuilt.e'
        run(args.packager, 'pack', directory, rebuilt)
        rebuilt_sections = {k: b for k, _, b in sections(rebuilt.read_bytes())}
        assert not resource_events(rebuilt_sections[RESOURCE])[0], 'Resource table must not duplicate bindings'
        assert len(rebuilt_sections[EVENTS]) == len(indexed) * 16
        after = root / 'after'
        run(args.packager, 'unpack', rebuilt, after, '--main-only')
        assert xml_events(after) == expected
        for arch, executable in [('x86', args.packager), ('x64', args.x64_packager)]:
            if executable:
                output = root / f'roundtrip-{arch}.e'
                run(executable, 'pack', after, output)
                assert output.read_bytes() == rebuilt.read_bytes()
        # 用户实际写入两条相同 XML 事件仍应被拒绝，不能靠放宽校验修复。
        xml = next((directory / 'src').glob('*.xml'))
        text = xml.read_text(encoding='utf-8-sig')
        event_line = next(line for line in text.splitlines() if '.事件 ' in line)
        xml.write_bytes(b'\xef\xbb\xbf' + text.replace(event_line, event_line + '\n' + event_line, 1)
                       .replace('\r\n', '\n').replace('\n', '\r\n').encode())
        run(args.packager, 'pack', directory, root / 'invalid.e', success=False)
        # 菜单单击事件也只存辅助索引，验证新建菜单不会丢失绑定。
        menu_xml = next((after / 'src').glob('*.xml'))
        text = menu_xml.read_text(encoding='utf-8-sig')
        menu = ('  <窗口.菜单><菜单 名称="回归菜单" 标题="回归菜单">'
                '<菜单.事件 名称="单击" 处理器="窗口程序集_启动窗口::_按钮1_被单击" />'
                '</菜单></窗口.菜单>\n')
        menu_xml.write_bytes(b'\xef\xbb\xbf' + text.replace('</窗口>', menu + '</窗口>')
                             .replace('\r\n', '\n').replace('\n', '\r\n').encode())
        menu_source = root / 'menu.e'
        run(args.packager, 'pack', after, menu_source)
        menu_sections = {k: b for k, _, b in sections(menu_source.read_bytes())}
        assert not resource_events(menu_sections[RESOURCE])[0]
        assert len(menu_sections[EVENTS]) == (len(indexed) + 1) * 16
        menu_after = root / 'menu-after'
        run(args.packager, 'unpack', menu_source, menu_after, '--main-only')
        assert xml_events(menu_after) == sorted(expected + [
            ('回归菜单', None, '窗口程序集_启动窗口::_按钮1_被单击')])
        print(json.dumps({'ok': True, 'events': len(expected), 'cases': [
            'identical_tables', 'stale_resource_ids', 'resource_only', 'semantic_rebuild',
            'single_event_table', 'byte_roundtrip', 'reject_duplicate_xml', 'menu_event']}, ensure_ascii=False))


if __name__ == '__main__':
    main()

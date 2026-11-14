"""以 IDE read_real_file 的真实页为独立基准，对比拆包代码；支持保存基准后离线重测。"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import urllib.request


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Client:
    def __init__(self, endpoint):
        self.endpoint, self.session, self.sequence = endpoint, None, 0
        self.rpc('initialize', {'protocolVersion': '2025-03-26', 'capabilities': {},
                               'clientInfo': {'name': 'ide-page-parity', 'version': '1'}})

    def rpc(self, method, params):
        self.sequence += 1
        headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream'}
        if self.session:
            headers['Mcp-Session-Id'] = self.session
        body = json.dumps(dict(jsonrpc='2.0', id=self.sequence, method=method, params=params),
                          ensure_ascii=False).encode('utf-8')
        request = urllib.request.Request(self.endpoint, body, headers)
        with urllib.request.urlopen(request, timeout=180) as response:
            self.session = response.headers.get('Mcp-Session-Id', self.session)
            text = response.read().decode('utf-8')
        if text.startswith('event:') or text.startswith('data:'):
            text = next(line[5:].lstrip() for line in text.split('\n') if line.startswith('data:'))
        result = json.loads(text)
        if 'error' in result:
            raise RuntimeError(result['error'])
        return result['result']

    def call(self, name, arguments):
        result = self.rpc('tools/call', dict(name=name, arguments=arguments))
        data = result.get('structuredContent') or json.loads(result['content'][0]['text'])
        if result.get('isError') or not data.get('ok'):
            raise RuntimeError((name, data))
        return data


def capture(args, output):
    client = Client(args.endpoint)
    instances = client.call('list_instances', {})
    save(output / 'instances.json', instances)
    if args.instance_id:
        client.call('select_instance', {'instance_id': args.instance_id})
    info = client.call('get_current_eide_info', {})
    if Path(info['source_file_path']).resolve() != args.source.resolve():
        raise RuntimeError('IDE 当前工程与 --source 不同；应先打开指定文件或选择对应实例')
    before = sha(args.source)
    save(output / 'info.json', info)
    save(output / 'refresh.json', client.call('refresh_workspace_mirror', {'mode': 'main_only'}))
    files, offset, generation = [], 0, None
    while True:
        query = dict(path='src', limit=5000, offset=offset)
        if offset:
            query['mirror_generation'] = generation
        listing = client.call('list_files', query)
        files.extend(listing['files'])
        generation = listing['mirror_generation']
        if not listing['has_more']:
            break
        offset = listing['next_offset']
    pages = []
    for relative in files:
        if not relative.endswith('.txt'):
            continue
        offset, code_hash, lines, responses = 0, None, [], []
        while True:
            response = client.call('read_real_file', dict(file_path=relative, limit=20000, offset=offset))
            responses.append(response)
            if response.get('code_kind') not in ('real_source', 'fixed_table_real_page'):
                raise RuntimeError('拒绝将镜像代码作为 IDE 独立基准')
            if code_hash and response['code_hash'] != code_hash:
                raise RuntimeError('分页期间 IDE 真实页发生变化')
            code_hash = response['code_hash']
            for line in response['content'].split('\n'):
                if not line:
                    continue
                match = re.fullmatch(r'(\d+)\t(.*)', line)
                if not match or int(match[1]) != len(lines) + 1:
                    raise RuntimeError(('真实页分页行号不连续', relative, line))
                lines.append(match[2])
            if not response['has_more']:
                break
            offset = response['next_offset']
        save(output / 'responses' / (relative + '.json'), responses)
        pages.append(dict(path=relative, code_hash=code_hash, text='\n'.join(lines) + '\n',
                          trace=responses[0].get('trace', '')))
    if sha(args.source) != before:
        raise RuntimeError('读取过程中源文件发生变化')
    oracle = dict(source=str(args.source.resolve()), source_sha256=before, ide=info, pages=pages,
                  provenance='AutoLinker MCP read_real_file; mirror used only to enumerate/map pages')
    save(output / 'oracle.json', oracle)
    return oracle


def normalize(text):
    # read_real_file 明确省略支持库头并补 .版本；仅排除这些头和空行、行尾排版。
    # 不忽略代码内部空格、引号、前缀、声明字段或真实文本内容。
    result, quoted = [], False
    for line in text.replace('\r\n', '\n').split('\n'):
        started_inside = quoted
        if not quoted and (not line.strip() or line.strip() == '.版本 2' or line.startswith('.支持库 ')):
            continue
        for character in line:
            if not quoted and character == "'":
                break
            if character == '“' and not quoted:
                quoted = True
            elif character == '”' and quoted:
                quoted = False
        result.append(line if quoted or started_inside else line.rstrip())
    return '\n'.join(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--packager', type=Path, required=True)
    parser.add_argument('--e-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--oracle', type=Path)
    parser.add_argument('--endpoint', default='http://127.0.0.1:19207/mcp')
    parser.add_argument('--instance-id')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    oracle = json.loads(args.oracle.read_text(encoding='utf-8-sig')) if args.oracle else capture(args, args.output)
    before = sha(args.source)
    if before != oracle['source_sha256']:
        raise RuntimeError('源文件哈希与 IDE 基准不符，必须重新读取真实页')
    command = [str(args.packager.resolve()), 'unpack', str(args.source.resolve()),
               str((args.output / 'unpacked').resolve()), '--main-only', '--e-dir', str(args.e_dir.resolve())]
    result = subprocess.run(command, cwd=args.e_dir, capture_output=True, timeout=180)
    (args.output / 'unpack.log').write_bytes(result.stdout + result.stderr)
    save(args.output / 'command.json', dict(command=command, exit_code=result.returncode,
                                           packager_sha256=sha(args.packager)))
    if result.returncode:
        raise RuntimeError('拆包失败，见 unpack.log')
    rows = []
    for page in oracle['pages']:
        actual = (args.output / 'unpacked' / page['path']).read_text(encoding='utf-8-sig')
        expected = page['text']
        equal = normalize(actual) == normalize(expected)
        if not equal:
            diff = '\n'.join(difflib.unified_diff(normalize(expected).split('\n'), normalize(actual).split('\n'),
                                                 fromfile='IDE/' + page['path'], tofile='unpack/' + page['path'], lineterm=''))
            path = args.output / 'diff' / (page['path'] + '.diff')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(diff, encoding='utf-8')
        rows.append(dict(path=page['path'], equal=equal))
    assert sha(args.source) == before
    summary = dict(pages=len(rows), equal=sum(row['equal'] for row in rows), results=rows,
                   source_sha256=before, original_unchanged=True, oracle=str(args.oracle or args.output / 'oracle.json'))
    save(args.output / 'summary.json', summary)
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if all(row['equal'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())

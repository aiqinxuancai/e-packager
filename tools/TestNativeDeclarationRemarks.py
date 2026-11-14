"""验证声明备注的前后空格、制表符、逗号及多行内容经过语义回包不丢失。"""
import argparse
import json
from pathlib import Path
import subprocess


def write(path, value):
    path.write_bytes(value.replace('\n', '\r\n').encode('utf-8-sig'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output.resolve()
    commands = []

    def run(*parts):
        command = [str(args.exe), *map(str, parts)]
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=120)
        commands.append(dict(command=command, exit=result.returncode,
                             stderr=result.stderr.decode('utf-8', 'replace')))
        (root / 'commands.json').write_text(json.dumps(commands, ensure_ascii=False, indent=2), encoding='utf-8-sig')
        assert result.returncode == 0, commands[-1]

    repo = Path(__file__).resolve().parent.parent
    workspace = root / 'workspace'
    run('unpack', repo / 'eproj/e-console-exe-new-proj.e', workspace, '--main-only')
    program = ('.版本 2\n\n.程序集 程序集1, , ,   程序集备注  \t\n'
               '.程序集变量 集变量, 整数型, , ,   集变量备注  \t\n\n'
               '.子程序 _启动子程序, 整数型, ,   入口备注, 逗号  \t\n'
               '.局部变量 局变量, 整数型, , ,   局变量备注  \t\n\n返回 (0)\n\n'
               '.子程序 备注测试, 整数型, 公开,   方法备注  \t\n'
               '.参数 参数值, 整数型, ,   参数备注  \t\n\n返回 (参数值)\n')
    dll = ('.版本 2\n\n.DLL命令 测试取进程, 整数型, "kernel32.dll ", "GetCurrentProcessId ", ,   首行备注  \t\n'
           '  续行备注, 逗号  \t\n  \n')
    write(workspace / 'src/程序集1.txt', program)
    write(workspace / 'src/.DLL声明.txt', dll)
    run('pack', workspace, root / 'edited.e')
    run('unpack', root / 'edited.e', root / 'decoded', '--main-only')
    for name, expected in [('程序集1.txt', program), ('.DLL声明.txt', dll)]:
        actual = (root / 'decoded/src' / name).read_text(encoding='utf-8-sig')
        for line in expected.splitlines():
            if '备注' in line or (line and not line.strip()):
                assert line in actual.splitlines(), (name, repr(line), repr(actual))
    run('pack', root / 'decoded', root / 'second.e')
    assert (root / 'second.e').read_bytes() == (root / 'edited.e').read_bytes()
    # 非声明编辑不得因备注空白改变而重新分配声明身份。
    page = root / 'decoded/src/程序集1.txt'
    write(page, page.read_text(encoding='utf-8-sig') + "\n' identity check\n")
    run('pack', root / 'decoded', root / 'semantic.e')
    run('unpack', root / 'semantic.e', root / 'semantic', '--main-only')
    before = json.loads((root / 'decoded/project/.native_symbol_map.json').read_text(encoding='utf-8-sig'))
    after = json.loads((root / 'semantic/project/.native_symbol_map.json').read_text(encoding='utf-8-sig'))
    assert [d['id'] for d in before['dlls']] == [d['id'] for d in after['dlls']]
    assert '"kernel32.dll ", "GetCurrentProcessId ",' in (root / 'semantic/src/.DLL声明.txt').read_text(encoding='utf-8-sig')

    print('PASS native declaration remarks')


if __name__ == '__main__':
    main()

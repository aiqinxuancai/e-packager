"""验证条件编译重名声明及第三次同分支重复的拒绝。"""
import argparse
from pathlib import Path
from TestSamplesSemanticAudit import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packager', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    repo = Path(__file__).resolve().parent.parent
    workspace = root / 'workspace'
    assert run([args.packager, 'unpack', repo / 'eproj/e-console-exe-new-proj.e', workspace], root / 'unpack.log') == 0
    page = next((workspace / 'src').glob('程序集*.txt'))
    source = ".版本 2\n.程序集 程序集1\n.子程序 _启动子程序, 整数型\n{locals}\n返回 (0)\n"
    first = '.局部变量 值, 整数型, , , $(TEST)\n'
    second = '.局部变量 值, 整数型, , , $(!TEST)\n'
    cases = [('branches', first + second, True),
             ('repeat-second-branch', first + second + second, False),
             ('unconditional', first + second + '.局部变量 值, 整数型\n', False)]
    for name, declarations, valid in cases:
        page.write_bytes(source.format(locals=declarations).replace('\n', '\r\n').encode('utf-8-sig'))
        log = root / (name + '.log')
        code = run([args.packager, 'pack', workspace, root / (name + '.e')], log)
        if valid:
            assert code == 0, log.read_text(encoding='utf-8-sig')
        else:
            assert code != 0 and 'method_variable_duplicate' in log.read_text(encoding='utf-8-sig'), name
        print('PASS', name, flush=True)


if __name__ == '__main__':
    main()

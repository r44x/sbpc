"""Makes the two Windows release zips from a Release build. Standard library only.

    python program/scripts/package_windows.py <build dir with sbpc.exe> <version> <out dir>

Writes <out>/SBPC-<version>-windows-x64.zip (data in %APPDATA%\\SBPC) and
<out>/SBPC-<version>-windows-x64-portable.zip (adds portable.txt: data next to sbpc.exe). Both hold
sbpc.exe, sbpc-cli.exe, sbpc-tcc.exe, tcc/, LICENSE, the bundled parts' licences and a README from
program/packaging/windows (@VERSION@ and @TCC_COMMIT@ filled in). No game data is ever involved.
Used by .github/workflows/release.yml; also runs by hand."""
import os
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TEMPLATES = os.path.join(ROOT, 'program', 'packaging', 'windows')


def tcc_commit():
    try:
        return subprocess.check_output(['git', '-C', os.path.join(ROOT, 'libs', 'tinycc'), 'rev-parse', 'HEAD'],
                                       text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return 'see libs/tinycc in the SBPC repository'


def readme(name, version, commit):
    text = open(os.path.join(TEMPLATES, name), encoding='utf-8').read()
    text = text.replace('@VERSION@', version).replace('@TCC_COMMIT@', commit)
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')   # Notepad-friendly


def files(build):
    """(path in the zip below the top folder, source path or bytes)"""
    out = [(n, os.path.join(build, n)) for n in ('sbpc.exe', 'sbpc-cli.exe', 'sbpc-tcc.exe')]
    tcc = os.path.join(build, 'tcc')
    for dirpath, _, names in os.walk(tcc):
        for n in sorted(names):
            full = os.path.join(dirpath, n)
            out.append(('tcc/' + os.path.relpath(full, tcc).replace(os.sep, '/'), full))
    out += [('LICENSE', os.path.join(ROOT, 'LICENSE')),
            ('licenses/SDL3/LICENSE.txt', os.path.join(ROOT, 'libs', 'SDL', 'LICENSE.txt')),
            ('licenses/TinyCC/COPYING', os.path.join(ROOT, 'libs', 'tinycc', 'COPYING')),
            ('licenses/Half Eighties/OFL.txt', os.path.join(ROOT, 'libs', 'half-eighties', 'OFL.txt'))]
    for name, src in out:
        if not os.path.isfile(src):
            sys.exit('missing: %s' % src)
    return out


def write_zip(path, top, entries):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, src in sorted(entries, key=lambda e: e[0]):
            arc = top + '/' + name
            if isinstance(src, bytes):
                z.writestr(arc, src)
            else:
                z.write(src, arc)
    print('wrote %s (%d files)' % (path, len(entries)))


def main():
    build, version, out = sys.argv[1], sys.argv[2].lstrip('v'), sys.argv[3]
    os.makedirs(out, exist_ok=True)
    commit = tcc_commit()
    common = files(build)
    name = 'SBPC-%s-windows-x64' % version
    write_zip(os.path.join(out, name + '.zip'), name,
              common + [('README.txt', readme('README.txt', version, commit))])
    name_p = name + '-portable'
    portable = open(os.path.join(TEMPLATES, 'portable.txt'), 'rb').read().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
    write_zip(os.path.join(out, name_p + '.zip'), name_p,
              common + [('README.txt', readme('README-portable.txt', version, commit)), ('portable.txt', portable)])


if __name__ == '__main__':
    main()

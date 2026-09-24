"""Rehearse the Windows job of .github/workflows/release.yml locally.

Runs every `run:` step of the build job, in order, the way a GitHub
runner would -- under PowerShell 7, with the workflow and step
environment, GITHUB_OUTPUT, and the prologue/epilogue GitHub wraps pwsh
steps in -- against a clean `git archive` of HEAD in a fresh build
folder.  A failure here is a failure you would otherwise only see after
pushing a tag.

Skipped: `uses:` actions, and the Rtools install step (it needs
administrator rights); the Rtools45 given with --rtools is used instead.

    python packaging/ci-dryrun.py --pwsh "C:/Program Files/PowerShell/7/pwsh.exe" \
                                  --rtools D:/rtools45 --root D:/rgui-ci

Needs Python with PyYAML, and PowerShell 7 (the portable zip will do).
A full run builds R from scratch: expect 15-60 minutes.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

try:
    import yaml
except ImportError:
    sys.exit("needs PyYAML: python -m pip install pyyaml")

here = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('--repo', default=os.path.dirname(here))
ap.add_argument('--pwsh', default='pwsh')
ap.add_argument('--rtools', required=True)
ap.add_argument('--root', default='D:\\rgui-ci',
                help='work folder, wiped on every run; no spaces')
ap.add_argument('--tag', default='v0.0.0-dryrun', help='tag the run pretends to build')
a = ap.parse_args()

root = os.path.abspath(a.root)
repo = os.path.abspath(a.repo)
if ' ' in root:
    sys.exit('--root must not contain spaces (R cannot be built there)')
if len([p for p in root.replace('\\', '/').split('/') if p]) < 2 or \
        root.lower().startswith(repo.lower()) or repo.lower().startswith(root.lower()):
    sys.exit('--root must be a dedicated folder, not a drive root or near the repository')

src, build = os.path.join(root, 'src'), os.path.join(root, 'build')
for d in (src, build):
    if os.path.exists(d):
        shutil.rmtree(d)
os.makedirs(src)

sha = subprocess.check_output(['git', '-C', repo, 'rev-parse', 'HEAD'], text=True).strip()
tar = os.path.join(root, 'src.tar')
subprocess.check_call(['git', '-C', repo, 'archive', '--format=tar', '-o', tar, 'HEAD'])
subprocess.check_call([os.path.join(os.environ['SystemRoot'], 'System32', 'tar.exe'),
                       '-xf', tar, '-C', src])
os.remove(tar)
print('snapshot of %s in %s' % (sha[:7], src), flush=True)

wf = yaml.safe_load(open(os.path.join(src, '.github', 'workflows', 'release.yml'),
                         encoding='utf-8'))
env_base = {k: str(v) for k, v in wf['env'].items()}
env_base.update(BUILD_ROOT=build, RTOOLS_DIR=a.rtools)
outputs = {}

def expand(value):
    v = str(value)
    for k, r in (('${{ github.ref_type }}', 'tag'), ('${{ github.ref_name }}', a.tag),
                 ('${{ github.run_number }}', '1'), ('${{ github.sha }}', sha),
                 ('${{ steps.version.outputs.version }}', outputs.get('version', ''))):
        v = v.replace(k, r)
    return v

logdir = os.path.join(root, 'steplogs')
os.makedirs(logdir, exist_ok=True)
for i, step in enumerate(wf['jobs']['build']['steps']):
    name = step.get('name', 'step %d' % i)
    if 'uses' in step:
        print('[skip] %s (action)' % name, flush=True)
        continue
    if name.startswith('Install Rtools45'):
        print('[skip] %s (using %s)' % (name, a.rtools), flush=True)
        continue
    env = dict(os.environ)
    env.update(env_base)
    env.update({k: expand(v) for k, v in (step.get('env') or {}).items()})
    out = tempfile.NamedTemporaryFile(delete=False, suffix='.out')
    out.close()
    env['GITHUB_OUTPUT'] = out.name
    ps1 = os.path.join(root, 'step.ps1')
    with open(ps1, 'w', encoding='utf-8') as f:
        f.write("$ErrorActionPreference = 'stop'\n" + step['run'] +
                "\nif ((Test-Path -LiteralPath variable:\\LASTEXITCODE)) { exit $LASTEXITCODE }\n")
    log = os.path.join(logdir, '%02d.log' % i)
    t0 = time.time()
    with open(log, 'w', encoding='utf-8', errors='replace') as lf:
        r = subprocess.run([a.pwsh, '-NoProfile', '-NonInteractive', '-File', ps1],
                           cwd=src, env=env, stdout=lf, stderr=subprocess.STDOUT)
    for line in open(out.name, encoding='utf-8'):
        if '=' in line:
            k, v = line.strip().split('=', 1)
            outputs[k] = v
    os.unlink(out.name)
    print('[%s] %s  (%.0fs)  log: %s' % ('ok' if r.returncode == 0 else 'FAIL', name,
                                          time.time() - t0, log), flush=True)
    if r.returncode != 0:
        tail = open(log, encoding='utf-8', errors='replace').read().splitlines()[-25:]
        print('\n'.join('    ' + t for t in tail))
        sys.exit(1)

print('outputs:', outputs)
print('DRY RUN PASSED', flush=True)

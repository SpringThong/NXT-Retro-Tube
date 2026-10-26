#!/usr/bin/env python3
"""Build ota.json for a GitHub release of NXT Retro Tube and sign it.

  python3 tools/ota/make_manifest.py --repo SpringThong/NXT-Retro-Tube --tag v2.0 \
      --assets dist/ --notes notes_vi.txt [--notes-en notes_en.txt] [--key ota_private.pem]

--assets holds the release's OTA packages:
  ota-app-<channel>.zip   ota-ytdlp.zip   ota-deno.zip
Each has component.json ({"component": ..., "version": ...}) at its root.
Writes <assets>/ota.json and, with --key (or the OTA_SIGNING_KEY environment
variable holding the PEM text), <assets>/ota.json.sig (ECDSA P-256 / SHA-256, DER),
using the openssl command line tool. Standard library only.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import zipfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--assets', required=True)
    ap.add_argument('--notes', help='text file, Vietnamese release notes (one paragraph)')
    ap.add_argument('--notes-en', help='text file, English release notes')
    ap.add_argument('--key', help='PEM private key (else $OTA_SIGNING_KEY)')
    ap.add_argument('--base-url', help='override the download URL prefix (tests)')
    ap.add_argument('--reuse', help='an earlier ota.json: components not in --assets keep its entries (and URLs)')
    a = ap.parse_args()

    base = a.base_url or 'https://github.com/%s/releases/download/%s' % (a.repo, a.tag)
    comps, app_version = {}, None
    for name in sorted(os.listdir(a.assets)):
        if not (name.startswith('ota-') and name.endswith('.zip')):
            continue
        path = os.path.join(a.assets, name)
        with zipfile.ZipFile(path) as z:
            info = json.loads(z.read('component.json'))
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        comp = info['component']
        if comp in comps:
            sys.exit('duplicate component %s' % comp)
        comps[comp] = {'version': info['version'], 'url': '%s/%s' % (base, name),
                       'size': os.path.getsize(path), 'sha256': h.hexdigest()}
        if comp.startswith('app-'):
            if app_version and app_version != info['version']:
                sys.exit('app packages disagree on the version: %s / %s' % (app_version, info['version']))
            app_version = info['version']
        print('%-22s %-12s %10d  %s' % (comp, info['version'], comps[comp]['size'], name))
    if not app_version:
        sys.exit('no ota-app-*.zip in %s' % a.assets)
    if a.reuse:
        old = json.load(open(a.reuse, encoding='utf-8'))
        for comp, entry in old.get('components', {}).items():
            if comp not in comps:
                comps[comp] = entry
                print('%-22s %-12s %10d  (kept: %s)' % (comp, entry['version'], entry['size'], entry['url']))

    def text(p):
        return open(p, encoding='utf-8').read().strip() if p else ''
    manifest = {'format': 1, 'name': 'NXT Retro Tube', 'version': app_version, 'tag': a.tag,
                'notes_vi': text(a.notes), 'notes_en': text(a.notes_en) or text(a.notes),
                'components': comps}
    out = os.path.join(a.assets, 'ota.json')
    data = json.dumps(manifest, ensure_ascii=False, indent=1).encode('utf-8')
    with open(out, 'wb') as f:
        f.write(data)
    print('wrote', out)

    key_pem = open(a.key).read() if a.key else os.environ.get('OTA_SIGNING_KEY', '')
    if not key_pem.strip():
        print('no signing key: ota.json is NOT signed (devices will ignore it)')
        return
    with tempfile.NamedTemporaryFile('w', suffix='.pem', delete=False) as k:
        k.write(key_pem if key_pem.endswith('\n') else key_pem + '\n')
        kp = k.name
    try:
        subprocess.run(['openssl', 'dgst', '-sha256', '-sign', kp, '-out', out + '.sig', out], check=True)
    finally:
        os.unlink(kp)
    print('wrote', out + '.sig')


if __name__ == '__main__':
    main()

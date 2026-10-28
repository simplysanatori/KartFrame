#!/usr/bin/env python3
"""Install KartFrame's Steam library art for one shortcut.

  art.py --grid DIR --appid N [--home KARTFRAME_HOME] [--sgdb auto|yes|no]
  art.py --vdf shortcuts.vdf --appid N --icon PNG        set the shortcut's dock icon (Steam must be stopped)

Writes <appid>p.png (portrait), <appid>.png (wide), <appid>_hero.png, <appid>_logo.png and
<appid>_icon.png into Steam's grid folder, and the dock icon to KARTFRAME_HOME/art/icon.png.
Anything it replaces is first copied to ~/.local/share/icon-backups/kartframe-<date>/.

The portrait, wide and hero are Mario Kart Wii artwork from SteamGridDB (asset ids and authors in
CREDITS.md), downloaded here and used as they are; they are never shipped. The logo is the plain
KartFrame wordmark and the icon is ours. Only a slot whose download fails gets the text-free backdrop
in frame/art/set instead. The CDN links need no key; an API key ($SGDB_API_KEY, or the file api_key in
~/.config/steamgriddb) is only used to look an asset up again if its link has changed.
"""
import argparse, importlib.util, json, os, shutil, struct, subprocess, sys, tempfile, time
import urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser('~')
SGDB = 'https://www.steamgriddb.com/api/v2'
SGDB_GAME = 36054        # Mario Kart Wii
SLOTS = (('p', 'p'), ('wide', ''), ('hero', '_hero'), ('logo', '_logo'), ('icon', '_icon'))
# slot -> (SGDB endpoint, asset id, CDN link, size Steam wants, size the asset has if different)
ASSETS = {
    'p':    ('grids',  61323,  'https://cdn2.steamgriddb.com/grid/984852334d07789b395be2f7122d7019.png', (600, 900), None),
    'wide': ('grids',  263653, 'https://cdn2.steamgriddb.com/grid/651f5d50bc95e5ea8ea5b79673b533c9.png', (920, 430), None),
    'hero': ('heroes', 185189, 'https://cdn2.steamgriddb.com/hero/3ace1262532bf6b34aead15cf7e1142f.png', (1920, 620), (3840, 1240)),
}


def log(*a):
    print('art:', *a, flush=True)


def sgdb_key():
    k = os.environ.get('SGDB_API_KEY', '').strip()
    p = os.path.join(HOME, '.config', 'steamgriddb', 'api_key')
    if not k and os.path.exists(p):
        k = open(p).read().strip()
    return k


def fetch(url, dest):
    req = urllib.request.Request(url, headers={'User-Agent': 'kartframe-art/1'})
    with open(dest, 'wb') as f:
        f.write(urllib.request.urlopen(req, timeout=120).read())


def lookup_url(endpoint, aid):
    """The current link of an asset through the API (needs a key)."""
    key = sgdb_key()
    if not key:
        raise RuntimeError('link is dead and there is no SteamGridDB API key')
    q = urllib.parse.urlencode({'nsfw': 'false', 'humor': 'false', 'mimes': 'image/png'})
    req = urllib.request.Request(f'{SGDB}/{endpoint}/game/{SGDB_GAME}?{q}',
                                 headers={'Authorization': 'Bearer ' + key, 'User-Agent': 'kartframe-art/1'})
    for i in json.load(urllib.request.urlopen(req, timeout=30)).get('data') or []:
        if i['id'] == aid:
            return i['url']
    raise RuntimeError(f'asset {aid} not offered by SteamGridDB any more')


def png_size(path):
    with open(path, 'rb') as f:
        h = f.read(24)
    if h[:8] != b'\x89PNG\r\n\x1a\n':
        raise RuntimeError('not a PNG')
    return struct.unpack('>II', h[16:24])


def get_asset(slot, tmp):
    """Download one SteamGridDB asset into tmp, check its size, return the path; raises on any problem."""
    endpoint, aid, url, want, alt = ASSETS[slot]
    dest = os.path.join(tmp, slot + '.png')
    try:
        fetch(url, dest)
    except Exception:                                      # noqa: BLE001 - dead link: ask the API
        fetch(lookup_url(endpoint, aid), dest)
    size = png_size(dest)
    if size == want:
        return dest
    if alt and size == alt:
        if not shutil.which('ffmpeg'):
            return dest                                    # Steam scales the 2x hero itself
        out = os.path.join(tmp, slot + '-s.png')           # bring the 2x hero to Steam's size
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', dest, '-vf',
                        'scale=%d:%d:flags=lanczos' % want, out], check=True, timeout=120)
        return out
    raise RuntimeError(f'asset {aid} is {size[0]}x{size[1]}, expected {want[0]}x{want[1]}')


def install(grid, appid, kfhome, sgdb):
    tmp = tempfile.mkdtemp(prefix='kf-art-')
    try:
        files = {name: os.path.join(HERE, 'set', name + '.png') for name in ASSETS}
        files['logo'] = os.path.join(HERE, 'logo.png')
        files['icon'] = os.path.join(HERE, 'icon-512.png')
        for n in ASSETS:
            if sgdb == 'no':
                continue
            try:
                files[n] = get_asset(n, tmp)
                log('%s: SteamGridDB %s asset %d' % (n, ASSETS[n][0][:-1], ASSETS[n][1]))
            except Exception as e:                       # noqa: BLE001 - this slot gets the shipped backdrop
                if sgdb == 'yes':
                    sys.exit(f'art: SteamGridDB art failed for {n}: {e}')
                log(f'{n}: using the shipped backdrop ({e})')
        os.makedirs(grid, exist_ok=True)
        bk = os.path.join(HOME, '.local/share/icon-backups', 'kartframe-' + time.strftime('%Y%m%d'))
        for name, suf in SLOTS:
            dest = os.path.join(grid, f'{appid}{suf}.png')
            # a leftover of another type (steam-art writes jpg/webp too) would shadow ours
            for ext in ('jpg', 'jpeg', 'webp', 'png'):
                old = os.path.join(grid, f'{appid}{suf}.{ext}')
                if os.path.exists(old):
                    if ext == 'png' and open(old, 'rb').read() == open(files[name], 'rb').read():
                        continue
                    os.makedirs(bk, exist_ok=True)
                    shutil.copy2(old, os.path.join(bk, os.path.basename(old)))
                    if old != dest:
                        os.remove(old)
            shutil.copyfile(files[name], dest + '.part')
            os.replace(dest + '.part', dest)
            log(dest)
        icon = os.path.join(kfhome, 'art', 'icon.png')
        os.makedirs(os.path.dirname(icon), exist_ok=True)
        shutil.copyfile(os.path.join(HERE, 'icon-512.png'), icon)
        log(icon + '  (dock icon)')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def set_icon_vdf(vdf, appid, icon, copy=False):
    """Offline edit of a shortcuts.vdf (Steam must not be running unless it is a copy)."""
    if not copy and subprocess.run(['pgrep', '-x', 'steam'], capture_output=True).returncode == 0:
        sys.exit('art: Steam is running; it rewrites shortcuts.vdf. Stop Steam first.')
    spec = importlib.util.spec_from_file_location('steam_cef', os.path.join(HERE, '..', 'steam-cef.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    d = m.vdf_load(vdf)
    hit = [s for s in d['shortcuts'].values() if s.get('appid') == appid]
    if not hit:
        sys.exit(f'art: no shortcut with appid {appid} in {vdf}')
    shutil.copy2(vdf, vdf + '.bak-kartframe-art')
    for s in hit:
        s['icon'] = icon
    m.vdf_save(vdf, d)
    log(f'icon of appid {appid} -> {icon}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--grid')
    ap.add_argument('--appid', type=int, required=True)
    ap.add_argument('--home', default=os.environ.get('KARTFRAME_HOME') or os.path.join(HOME, '.local/share/KartFrame'))
    ap.add_argument('--sgdb', choices=('auto', 'yes', 'no'), default=os.environ.get('KARTFRAME_ART_SGDB', 'auto'))
    ap.add_argument('--vdf')
    ap.add_argument('--icon')
    ap.add_argument('--copy', action='store_true', help='the vdf is a copied Steam folder, Steam may run')
    a = ap.parse_args()
    if a.vdf:
        set_icon_vdf(a.vdf, a.appid, a.icon or os.path.join(a.home, 'art', 'icon.png'), a.copy)
    else:
        if not a.grid:
            ap.error('--grid or --vdf required')
        install(a.grid, a.appid, a.home, a.sgdb)


if __name__ == '__main__':
    main()

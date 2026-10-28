#!/usr/bin/env python3
"""Install KartFrame's Steam library art for one shortcut.

  art.py --grid DIR --appid N [--home KARTFRAME_HOME] [--sgdb auto|yes|no]
  art.py --vdf shortcuts.vdf --appid N --icon PNG        set the shortcut's dock icon (Steam must be stopped)

Writes <appid>p.png (portrait), <appid>.png (wide), <appid>_hero.png, <appid>_logo.png and
<appid>_icon.png into Steam's grid folder, and the dock icon to KARTFRAME_HOME/art/icon.png.
Anything it replaces is first copied to ~/.local/share/icon-backups/kartframe-<date>/.

The set in frame/art/set is entirely our own artwork (the KartFrame logo over a drawn backdrop).
With a SteamGridDB API key ($SGDB_API_KEY, or the file api_key in ~/.config/steamgriddb) and ffmpeg, the
portrait, wide and hero are instead made from SteamGridDB's Mario Kart Wii key art (the ids are in
CREDITS.md) with the KartFrame logo laid over it; the art is downloaded here and never shipped.
Any failure falls back to the shipped set.
"""
import argparse, importlib.util, json, os, shutil, subprocess, sys, tempfile, time
import urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser('~')
SGDB = 'https://www.steamgriddb.com/api/v2'
SGDB_GAME = 36054        # Mario Kart Wii
SGDB_HERO = 185189       # text-free key art, 3840x1240 (see CREDITS.md)
SLOTS = (('p', 'p'), ('wide', ''), ('hero', '_hero'), ('logo', '_logo'), ('icon', '_icon'))


def log(*a):
    print('art:', *a, flush=True)


def sgdb_key():
    k = os.environ.get('SGDB_API_KEY', '').strip()
    p = os.path.join(HOME, '.config', 'steamgriddb', 'api_key')
    if not k and os.path.exists(p):
        k = open(p).read().strip()
    return k


def sgdb_hero(key, dest):
    q = urllib.parse.urlencode({'types': 'static', 'nsfw': 'false', 'humor': 'false', 'mimes': 'image/png'})
    req = urllib.request.Request(f'{SGDB}/heroes/game/{SGDB_GAME}?{q}',
                                 headers={'Authorization': 'Bearer ' + key, 'User-Agent': 'kartframe-art/1'})
    items = json.load(urllib.request.urlopen(req, timeout=30)).get('data') or []
    url = next((i['url'] for i in items if i['id'] == SGDB_HERO), None)
    if not url:
        raise RuntimeError(f'asset {SGDB_HERO} not offered by SteamGridDB any more')
    req = urllib.request.Request(url, headers={'User-Agent': 'kartframe-art/1'})
    with open(dest, 'wb') as f:
        f.write(urllib.request.urlopen(req, timeout=120).read())


def ffmpeg(*a):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', *a], check=True, timeout=120)


def compose_sgdb(tmp):
    """p.png, wide.png, hero.png from SteamGridDB key art into tmp; raises on any problem."""
    if not shutil.which('ffmpeg'):
        raise RuntimeError('ffmpeg not found')
    key = sgdb_key()
    if not key:
        raise RuntimeError('no SteamGridDB API key')
    src = os.path.join(tmp, 'src.png')
    sgdb_hero(key, src)
    logo = os.path.join(HERE, 'logo.png')
    ffmpeg('-i', src, '-vf', 'scale=1920:620:flags=lanczos', os.path.join(tmp, 'hero.png'))
    ffmpeg('-i', src, '-i', logo, '-filter_complex',
           '[0]crop=826:1240:2290:0,scale=600:900:flags=lanczos[b];[1]scale=560:-1:flags=lanczos[l];[b][l]overlay=20:28',
           os.path.join(tmp, 'p.png'))
    ffmpeg('-i', src, '-i', logo, '-filter_complex',
           '[0]crop=2653:1240:1000:0,scale=920:430:flags=lanczos[b];[1]scale=400:-1:flags=lanczos[l];[b][l]overlay=10:8',
           os.path.join(tmp, 'wide.png'))


def install(grid, appid, kfhome, sgdb):
    tmp = tempfile.mkdtemp(prefix='kf-art-')
    try:
        files = {name: os.path.join(HERE, 'set', name + '.png') for name, _ in SLOTS}
        files['logo'] = os.path.join(HERE, 'logo.png')
        files['icon'] = os.path.join(HERE, 'icon-512.png')
        if sgdb != 'no':
            try:
                compose_sgdb(tmp)
                for n in ('p', 'wide', 'hero'):
                    files[n] = os.path.join(tmp, n + '.png')
                log('portrait, wide and hero made from SteamGridDB asset %d' % SGDB_HERO)
            except Exception as e:                       # noqa: BLE001 - any failure means the shipped set
                if sgdb == 'yes':
                    sys.exit(f'art: SteamGridDB art failed: {e}')
                log(f'using the shipped art ({e})')
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

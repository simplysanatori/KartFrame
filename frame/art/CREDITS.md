# KartFrame library art: credits

Shipped in this folder, and original to KartFrame: `logo.svg`/`logo.png` (the KartFrame wordmark),
`icon.svg`/`icon-*.png` (the dock and library icon) and `set/` (the fallback portrait, wide and hero:
the logo and drawn backdrops). The lettering is set in [Lilita One](https://fonts.google.com/specimen/Lilita+One)
by Juan Montoreano (SIL Open Font License 1.1); the letter outlines are converted to paths in the SVGs.
The item box and rainbow ribbon are drawings of our own; Mario Kart Wii and its characters are
Nintendo's, and this is an unofficial fan project.

## SteamGridDB art (downloaded at install time, not redistributed)

`art.py` makes the portrait, wide and hero from this SteamGridDB asset when you have a SteamGridDB
API key and ffmpeg (game "Mario Kart Wii", SGDB game id 36054), and lays the KartFrame logo over it:

| Slot | SGDB asset | Author | Notes |
|---|---|---|---|
| hero, and the crops for portrait and wide | hero 185189 (3840x1240, no text) | MegaCrashFan96 | waifu2x upscale of hero 161458 |
| (source of the above) | hero 161458 (1920x620) | CaptainScarLeg | original |

Other assets looked at and not used: grid 730655 (Foxee17, text-free portrait on white), hero 7057
(Benuno), hero 101598 (BrochachoTheBro, blurred Luigi Circuit). Every other Mario Kart Wii portrait
and wide on SteamGridDB carries the Nintendo logo or third-party text, so none was used.

Without a key or without ffmpeg, `art.py` installs the shipped set instead. Without `--sgdb yes`
nothing is downloaded unless a key is found in `$SGDB_API_KEY` or `~/.config/steamgriddb/api_key`.

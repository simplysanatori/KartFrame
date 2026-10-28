# KartFrame library art: credits

Shipped in this folder, and original to KartFrame: `logo.svg`/`logo.png` (the plain KartFrame wordmark),
`icon.svg`/`icon-*.png` (the dock and library icon) and `set/` (a text-free backdrop of drawn sky,
clouds, item boxes and a rainbow, used only for a slot whose SteamGridDB download fails). The lettering
is set in [Lilita One](https://fonts.google.com/specimen/Lilita+One) by Juan Montoreano (SIL Open Font
License 1.1); the letter outlines are converted to paths in the SVGs. Mario Kart Wii and its characters
are Nintendo's, and this is an unofficial fan project.

## SteamGridDB art (downloaded at install time, not redistributed)

`art.py` installs these Mario Kart Wii assets (SteamGridDB game id 36054) as they are, with no overlay;
the hero is scaled from 3840x1240 to 1920x620:

| Slot | SGDB asset | Author | Notes |
|---|---|---|---|
| portrait 600x900 | grid 61323 | AVEX | key art: race scene, Mario Kart Wii title, no other text |
| wide 920x430 | grid 263653 | Sho. | title left, Luigi and Mario right, no other text |
| hero 1920x620 | hero 185189 (3840x1240) | MegaCrashFan96 | text-free; waifu2x upscale of hero 161458 (CaptainScarLeg) |

The logo slot is our wordmark (Steam draws it over the hero), the icon slot is ours.

Looked at and not used: portrait 200684 (Winchester7314), 66599, 858703 and 390641 (clean, but a
different scene than the hero), 692543 and 823102 (Nintendo, ESRB or Wii marks), 846471 and 860106
(a fan-project title); wide 840335, 268131, 578675 (marks or crops), 846472 and 860107 (a fan-project
title); hero 720 (LeMalgache, text-free), 7057, 162431 and 55735 (title or Wii mark),
101598 (blurred Luigi Circuit).

The CDN links are fixed in `art.py` and need no key. If a link dies, `art.py` looks the asset up again
with an API key from `$SGDB_API_KEY` or the file `api_key` in `~/.config/steamgriddb`; without a key, or
with `--sgdb no`, that slot gets the shipped backdrop.

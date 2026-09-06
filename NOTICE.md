# Notice

## What this repository contains

Source code written for this project, and nothing else. The MIT licence in `LICENSE`
covers that code.

## What it does not contain, and what that means for you

**No game assets.** There are no Spine skeletons (`.skel`, `.json`), no atlases, no
texture pages, no fonts, and no screenshots or captures of any game or its promotional
material. Not one byte. This is not an oversight to be corrected by a pull request —
any PR adding such a file will be closed.

**No Spine runtime.** spine-cpp is Esoteric Software's, distributed under their own
licence, and using it requires a Spine licence of your own. This project does not
redistribute it and does not grant you anything with respect to it. Read their terms
and decide for yourself: <https://esotericsoftware.com/spine-licensing>

**The scene table is a placeholder.** `adapter/scenes.h` describes how a scene is put
together and parses the config format, but the rows in its table are made up. The real
ones name a particular game's animations, which is a catalogue of that game's content
rather than anything this project authored.

## If you want to run something like this

You need art you have the right to use, and a Spine licence if you use the Spine
runtime to animate it. The adapter here is asset-agnostic on purpose: it takes whatever
skeleton you hand it.

## Trademarks and affiliation

This project is not affiliated with, endorsed by, or connected to any game publisher,
developer, or rights holder. Any names appearing in commit history or discussion are
used descriptively.

## Reporting a problem

If you hold rights in something you believe is present here, open an issue or contact
the repository owner and it will be removed promptly. The design intent is that there
is nothing to remove.

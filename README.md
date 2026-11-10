Ceyro

Linux only internal skin changer for **Counter-Strike 2**.

> ⚠️ **Warning**: This software technically falls into the "cheat" category
> and could result in a ban from the game; I take no responsibility for it.

## Requirements

- **CMake** >= 4.4.2
- **Ninja**
- A C++26 compiler with module support (GCC >= 14)
- **gdb** (used by `injekt.sh` to attach to the game and `dlopen` the library)
- **X11 development libraries** (the in-library HOME menu)

## Quick start

```sh
bash install.sh
sudo bash injekt.sh
```

See [HOW_TO_USE.md](HOW_TO_USE.md) for installation, injection, in-game compact GUI,
offset validation, and troubleshooting instructions.

### Skin customization

move ```config.json``` to ```~/.config/hamzex/config.json```

Find the skin you want on https://csgoskins.gg (or any source) and enter the number labeled "finish catalog" (found at the bottom left) into the "paint_kit" field.

valid values:

```
* Knives *
500 bayonet
503 css
505 flip
506 gut
507 karambit
508 m9
509 tactical
512 falchion
514 bowie
515 butterfly
516 push
517 cord
518 canis
519 ursus
521 outdoor
520 navaja
522 stiletto
523 talon
525 skeleton
526 kukri

* Agents *

I didn't feel like it; I'll add it later.

* Weapons *
ak47
m4a4
m4a1
aug
awp
ssg08
sg553
deagle
knife
mag7
mp9
mp7
mp5
usp
p2000
famas
galilar
scar20
g3sg1
glock
p250
five_seven
dual_berettas
tec9
cz75a
revolver
p90
mac10
bizon
ump45
xm1014
nova
m249
zeus
negev
sawedoff

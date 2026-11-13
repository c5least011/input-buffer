idk# Input Buffer Mobile

Every press and release is checked against a hidden "ghost" player before the
game sees it. If doing it right now would get you killed but a few ticks later
would not, the input is delayed by exactly that long. Otherwise it passes
through untouched.

## Features
- **Click modes** (cube, robot, UFO, ball, spider, swing): clicks are predicted
  through their whole arc; orbs only react to the click being predicted.
- **Ship & wave**: holds and releases are delayed only when death is imminent,
  so the controls never feel stiff.
- **Live trajectory**: always-on prediction of where you end up, plus the path
  of the last delayed click.
- **Dual & 2-player**: every player is its own buffer with its own mode and delay.
- **Gravity and speed portals** are predicted; anything it cannot predict
  (pads, mode portals...) ends the prediction instead of guessing.
- **Spam-safe**: events are queued in order, tiny taps are never lost.
- **Noclip-friendly** and fully configurable in-game (pause menu > BUF).
- **Autoplay (optional)**: a bot that plays by itself and learns from every
  death. Fully separate from the click assist.

## Notes
- Open the settings from the pause menu (BUF button).
- Tick delays are per game mode; 0 turns a mode off.
- This is an assist / fun tool. Do not use it for records or submissions
  where assists are not allowed.
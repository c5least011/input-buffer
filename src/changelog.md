# 1.0.0
made it
# 1.0.1
- improve auto play
- add d-block support
# 1.1.0
- Config tab
# 1.1.1
- fix some bug at 1-player in the new flip input detect 
- fix some mini bugs
# 1.1.2
- add sub-tick delay that improve the accuracy
# 1.1.3
- fix the wave in dual keep holding when the mod is off
# 1.1.4
- fix some gamemodes don't buffer correctly
# 1.2.0
- Click Between Frames compatibility: runs its update hook outermost, counts each physics tick once, no own sub-tick split while CBF is active (CBF's click time is used), held-back clicks are timed from the step CBF delivers them in
- late click rescue: a press / release that would be fatal now and cannot be saved by a delay is moved back in time (ghost replays the last ticks with the input, the real player takes over that state). Cube, ship, wave, robot. Settings: Delays tab (ticks per mode), Search tab (on/off)
- new settings: "Late click rescue", "CBF support"
# 1.2.1
- ground modes (cube, ball, robot, spider): buffer no longer fails on a long fall (air ticks are probed cheaply instead of a full look-ahead each); default max delay 8/6 -> 30 (saved values still at the old default are moved)
- late rescue: orbs are ignored; the press is delivered on the rewound state so the jump animation plays (no copyAttributes / rotation reset); the wave trail is cut back and redrawn along the replayed path
# 1.3.0
- orbs: all orb logic is one block (OrbState): per-run "orb used" flag instead of a global counter compared before / after; the chosen delay records whether it is the orb one
- log format: "saved +4 ticks | Cube | P1" (positive = delay, negative = late rescue rollback)
- "saved" ring: a white ring with a dark centre grows and fades around the player when a held-back input is released or a rollback happens (setting: Save ring effect)
# 1.3.1
- save mark: a small ring + tick count (+4 / -3) is left where the input was saved; it stays in the level (does not follow the player) until restart

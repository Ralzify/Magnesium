# Magnesium

Magnesium is a fork of [Erbium](https://github.com/plooshi/Erbium), an OGFN gameserver for chapters 1-5. I've created this fork to extend off of ploosh's work, and add some features of my own. Most credit goes to her for the base of the gameserver.

# Features/Changes
- New UI Overhaul : Buttons are evenly spaced, more information, and more options to toggle.
- Playlist Tab : Choose a playlist directly in the GUI, before the game starts.
- Start Match Button : When you're done configuring options, press this button to tell the gameserver to start preparing the match! Gives the user infinite time to do as they need :)
- Player Bot Tab : Customize player bot settings such as their name and health!
- Infinite Render : Want to hit a trickshot, but you seem to find yourself too far out of render distance to kill the player? Try turning this on! Extends the player render distance for the last player to join, so they won't go out of render!
- Arena Support : Proper Placement Points, Kill Points, and saving support.
- Many more commands added : When in game, run "cheat help" to see the full list!
- Default Settings Changes : Infinite Ammo & Infinite Materials default on for Late Game and switch off when Late Game is disabled; specialized modes can still override them.

# TODO:
- Fix 22.40 lategame crash
- Fix One Shot FX (GE per Playlist)
- Fix Randomize Lootpool ✅
- Players Tab ✅
- Auto Dump & Remove Tab / Fix Dumping ✅
- Pickaxe Stutter ✅
- Meters in Player Tab ✅
- Creative Tab in UI : Custom Plots ✅ (island loading is broken rn but when fixed this should work)
- Trickshot Tab ✅
- Logs Tab?
- Bus Start uses countdown over executeconsolecommand/phase -> avoids crash

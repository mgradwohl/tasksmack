submit and push so I can test on linux
Run the build
Make sure we honor the data refresh interval everywhere and keep rendering and data polling separate
Make sure that as soon as we show the panel for a process, we never take it away
Investigate if we really need PDH to scan for new processes every 5 seconds - the process panel already does this
Add documentation comments to clarify the caching architecture
Make the Linux socket stats cache TTL configurable (like PDH instance refresh)

Sweep the code for other constants that would be appropriate in the config
Moce the actions tab on the process panel to be the last tab

PS D:\source\tasksmack> cmake --build --preset win-debug
ninja: warning: premature end of file; recovering
[0/2] Re-checking globbed directories...
[156/542] Building C object D:/source/tasks
Make sure all colors and fonts come from the theme and are not manipulated in code

Sweep for dead code that should be removed or duplicate code that should be refactored into helpers and fix any issues from the problems panel. Do not remove Background Sampler.
Enhance and run tests
clang-tidy until clean
clang-format
precommit
submit and push and use the gh cli to create a PR based on the template

let's make my .gitconfig awesome

test on other windows and linux devices

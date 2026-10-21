# FAQ

**Is StageSignal ready for live shows?**
Not yet. The core library is tested, but the operator app is still a placeholder on `main`. The beta will say what is and is not wired. Read [Known Limitations](Known-Limitations) first.

**Does it work with my lights?**
Nobody has tested a named fixture, node or adapter yet. Art-Net and sACN packets match the layouts we wrote down, and the tests check that. A real receiver test is still needed for each device (L-02).

**Does it need the internet?**
No. It is offline first. It makes no update checks, no telemetry, no logins and no font or asset downloads. Broadcast only connects when you start it.

**Is it free?**
The source is AGPL-3.0-or-later. JUCE is used under its AGPL option. Whether a commercial JUCE licence is needed for closed redistribution is an owner decision (D-06).

**Why is Windows warning me?**
The beta is not code-signed (D-01). See [Getting Started](Getting-Started).

**Why does macOS say the app is damaged?**
It is quarantined by Gatekeeper because the beta is not notarised (D-02). Use `xattr -dr com.apple.quarantine` or right-click, then Open.

**Does it run as a VST3 or AU plugin?**
No. Only CLAP and the standalone app are deliverables (DEC-11).

**Which DAWs run the CLAP plugin?**
Only hosts the team has run are claimed. Bitwig, REAPER and FL Studio have CLAP support in their release notes. Each host still needs its own test (D-04).

**Can I use my own scenes?**
Scenes are built into the core for now. Adding one is a developer task (see [Adding an Effect](Adding-an-Effect)). A scene editor is not built.

**Will my show files still open after an update?**
The schema has a major and a minor version. A newer minor version loads and reports fields it does not know. A newer major version is refused. Older majors need a migration (see [Show File Format and Migration](Show-File-Format-and-Migration)).

**Does it detect tempo and key correctly on real music?**
Unknown. The tests use synthetic clicks and tones. Real-music accuracy depends on a labelled corpus that does not exist yet (L-03). The UI shows confidence and lets you override.

**Where do I report a bug?**
On the repository's issue tracker. Attach a support bundle if you can. Check the bundle preview first and remove anything you do not want to share.

**Can I use it on an 800 MHz machine?**
The core is designed to fit that budget, but nobody has measured it on that class of hardware (L-01). Vista and XP are no longer supported (DEC-03).

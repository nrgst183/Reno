VEGAS Pro and Reno: the scripts
===============================

Reno opens and saves .veg files directly, so these scripts are not needed for that. They are for checking Reno against
VEGAS and for Reno's older .reno files:

  Export to Reno.cs        writes the project open in VEGAS as a .reno next to the .veg, using VEGAS's own reading of
                           it. Use it to check Reno's .veg reader (see "Checking Reno against VEGAS").
  Import Reno Project.cs   asks for a .reno file and builds it in a new VEGAS project. (Alternatively, open the .reno
                           in Reno and Save As a .veg.)
  Reno Bridge.cs           runs jobs from a folder unattended (render, build a test project and save it as .veg,
                           describe, list templates and plug-ins ...). See BRIDGE.txt.

They are written for VEGAS Pro 18 (Tools > Scripting) in C# 5, and compile against VEGAS Pro 18's ScriptPortal.Vegas.dll
(tests/compare/bridge_test.sh checks that), but have not been run in VEGAS yet. If VEGAS shows a list of compile errors,
the script is wrong; report the error text.


Installing
----------
Either run the .cs file with Tools > Scripting > Run Script..., or copy the files to "Vegas Script Menu" in your
Documents folder (create it if needed) and use Tools > Scripting > Rescan Script Menu Folder.


Export to Reno
--------------
Open the project in VEGAS and run the script. It writes "<project name>.reno" next to the .veg and says where. Nothing in
VEGAS is changed. An unsaved project asks where to write the file.

What VEGAS's scripting cannot read is listed under "problems" at the end of the .reno: audio plug-in settings, bus and
master faders (a flat volume envelope on a bus is used), track heights and colours. Reno ignores the "vegas" entries.


Import Reno Project
-------------------
Run the script and pick a .reno file. VEGAS asks whether to save the open project, then builds the Reno project in a new
one (Edit > Undo takes it back). A window lists what could not be brought over; the list is also written next to the
.reno as "<name> - import problems.txt". Events whose media cannot be found (next to its path or the .reno) are left out.


Checking Reno against VEGAS
---------------------------
Reno's .veg reader was worked out from the bytes of real projects, and a few fields are guesses. To check them:

  1. In VEGAS, open each project and run Export to Reno. The most useful projects use compositing modes, track motion,
     pan/crop rotation, locked or muted events, audio events with elastique/classic settings or channel mapping,
     switched-off effects, buses with sends, and changed Project Properties (render quality, field order, pixel
     format, motion blur, deinterlace, ruler).
  2. Copy the .veg and .reno files somewhere Reno's test tools can reach and run:

        compare a.veg a.reno b.veg b.reno ...

     (tests/vegas/compare.cpp, built like the other test tools.)
  3. The output lists, field by field, how many values agree, examples that differ, and tables of raw .veg values next
     to VEGAS's own values.

Guesses the comparison settles:
  * the take offset's unit (Reno reads it as timeline time: media position = offset times the event's rate);
  * event flags (mute 0x02 and lock 0x08 are confirmed in vegas180.exe; loop), the video event flag for maintain aspect
    ratio (taken to be 0x200), OpenFX effect flags (switched off 0x01, applied before pan/crop 0x80), audio plug-in flags
    (switched off 0x01);
  * track flags (mute 0x02, solo 0x01);
  * where track motion keys keep position, size and rotation (only untouched track motion is recognised so far);
  * where Project Properties keep render quality, field order, pixel format, motion blur, deinterlacing and the ruler format;
  * the sense of pan/crop rotation.

The scripts also assume that track motion positions are pixels from the frame centre with y up, and that VEGAS's scripting
gives a take's offset as stored in the file (timeline time), not as a position in the media.


Checking the editing commands (RenoEdits.cs)
--------------------------------------------
RenoEdits.cs checks Split, Trim Start / End, Trim, Delete, Cut / Copy / Paste, Paste Insert, ripple, slip / slide trims,
AdjustStartLength, rate changes, looping events (loop_* scenarios: split at / before / after a loop point, Loop off, trims, fades)
and overlap cross-fades against VEGAS. First run: the API scenarios with sound worked; the video ones did not (the test AVI had a
wrong header size, now fixed; if VEGAS still refuses it the script uses a PNG sequence or generated media, and the log says which)
and every Vegas.InvokeCommand did nothing (the script now tries the keyboard-map contexts, pumps messages and falls back to the
scripting API: the op then has "viaApi": true). The log's first lines after "====" say which way worked: look for
"InvokeCommand works as section=".

  1. Save your work and close VEGAS projects you care about (the script closes the open project WITHOUT saving, once per
     scenario).
  2. Tools > Scripting > Run Script... and pick RenoEdits.cs. Pick or make an empty output folder when asked.
  3. Wait a few minutes (about 30 scenarios; it writes a 20 s WAV and AVI first). A message lists how many scenarios ran.
  4. Zip the output folder (one folder per scenario: before.veg, after.veg, result.json, plus run.log) and upload it.
  5. Then do the parts that need hand and eye work: docs/EDIT_CHECKLIST.md (C01-C40).

Back in Reno, tests/ui/editparity_replay.cpp redoes each scenario's operations on its before.veg in Reno and writes the same
JSON; tools/vegas/editparity_compare.py <VEGAS folder> <Reno folder> lists the differences field by field.

# Briefing voice-over

The deployment briefing's voice, put together from these recordings so it always says the site it is showing
(Assets/Data/briefing.json, Game/Mission/Briefing). Each folder holds a silent `placeholder.wav` as long as its line:
replace it with the recording (any name ending .wav; with several takes the first by name is used). A folder with no
recording is silent but still subtitled and timed.

Record each on its own, dry, with a little room either side, so they join cleanly: the words of a site's name are played
one after another.

## Phrases

What each slide's line says, from briefing.json -- the text is a placeholder to rewrite there first.

| Folder | Says |
|---|---|
| `Briefing/Phrases/begin` | Deployment briefing. |
| `Briefing/Phrases/destination` | Destination: |
| `Briefing/Phrases/site` | Site: |
| `Briefing/Phrases/conditions` | Surface conditions: |
| `Briefing/Phrases/objective` | Objective: locate the terminal, download the data, and bring the drive back to the shuttle. |
| `Briefing/Phrases/end` | Deploy when ready. |
| `Briefing/Phrases/map_given` | A map of the site is on file. |
| `Briefing/Phrases/map_missing` | There is no map of the site on file. |

## Words

Every word the site and planet names and the weather can use (from Assets/Data/sites.json). Adding a word to sites.json
means recording it here too.

| Folder | Says |
|---|---|
| `Briefing/Words/cryosphere` | cryosphere |
| `Briefing/Words/degrees` | degrees |
| `Briefing/Words/facility` | facility |
| `Briefing/Words/fair` | fair |
| `Briefing/Words/kepler` | kepler |
| `Briefing/Words/low` | low |
| `Briefing/Words/metres` | metres |
| `Briefing/Words/minus` | minus |
| `Briefing/Words/north` | north |
| `Briefing/Words/per` | per |
| `Briefing/Words/polar` | polar |
| `Briefing/Words/poor` | poor |
| `Briefing/Words/research` | research |
| `Briefing/Words/second` | second |
| `Briefing/Words/site` | site |
| `Briefing/Words/visibility` | visibility |
| `Briefing/Words/wind` | wind |

## Numbers

Numbers are said as their parts: 91 is "ninety" then "one"; 312 is "three", "hundred", "twelve". A site's number is said a
figure at a time: 06 is "zero", "six".

| Folder | Says |
|---|---|
| `Briefing/Numbers/0` | zero |
| `Briefing/Numbers/1` | one |
| `Briefing/Numbers/2` | two |
| `Briefing/Numbers/3` | three |
| `Briefing/Numbers/4` | four |
| `Briefing/Numbers/5` | five |
| `Briefing/Numbers/6` | six |
| `Briefing/Numbers/7` | seven |
| `Briefing/Numbers/8` | eight |
| `Briefing/Numbers/9` | nine |
| `Briefing/Numbers/10` | ten |
| `Briefing/Numbers/11` | eleven |
| `Briefing/Numbers/12` | twelve |
| `Briefing/Numbers/13` | thirteen |
| `Briefing/Numbers/14` | fourteen |
| `Briefing/Numbers/15` | fifteen |
| `Briefing/Numbers/16` | sixteen |
| `Briefing/Numbers/17` | seventeen |
| `Briefing/Numbers/18` | eighteen |
| `Briefing/Numbers/19` | nineteen |
| `Briefing/Numbers/20` | twenty |
| `Briefing/Numbers/30` | thirty |
| `Briefing/Numbers/40` | forty |
| `Briefing/Numbers/50` | fifty |
| `Briefing/Numbers/60` | sixty |
| `Briefing/Numbers/70` | seventy |
| `Briefing/Numbers/80` | eighty |
| `Briefing/Numbers/90` | ninety |
| `Briefing/Numbers/hundred` | hundred |

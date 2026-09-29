# Intercom

What the ship's intercom says at each moment (Assets/Data/intercom.json). Each moment has one line to start with, its
recording in the folder below -- a silent `placeholder.wav` for now: replace it with the recording, and write the line's
subtitle into intercom.json ("subtitle"). More lines for a moment: add another folder (`<moment>_02`) and another entry
in its list; one is picked, the same on every machine.

| Moment | Folder |
|---|---|
| arrival | `Intercom/arrival_01` |
| arrival_no_map | `Intercom/arrival_no_map_01` |
| power_out | `Intercom/power_out_01` |
| download_started | `Intercom/download_started_01` |
| download_done | `Intercom/download_done_01` |
| launch | `Intercom/launch_01` |
| recovered | `Intercom/recovered_01` |
| not_recovered | `Intercom/not_recovered_01` |
| left_behind | `Intercom/left_behind_01` |
| orbit | `Intercom/orbit_01` |
| deploy | `Intercom/deploy_01` |
| docked | `Intercom/docked_01` |
| orders | `Intercom/orders_01` |

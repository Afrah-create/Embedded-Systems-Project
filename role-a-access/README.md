# Role A — Access Control Core

Entrance terminal for the CoCIS smart building coursework. This folder is only Role A. Rooms, Wi-Fi, MQTT, the SD card and the management menus belong to the other roles.

The firmware is ESP-IDF. It runs on one ESP32 in Wokwi. Storage and the access schedule are stubbed in `components/platform_stubs/stubs.c` so this project builds and demos on its own. The coordinator replaces that file. The headers in `components/interfaces/include` stay as they are.

## What this role owns

| Task | Code |
| --- | --- |
| Entry and exit RFID readers | `components/access_core/mfrc522.c`, `rfid_reader.c` |
| `rfid_in_task`, `rfid_out_task` | `rfid_reader.c` |
| `access_task` grant/deny | `access.c` |
| `door_task` servos, LEDs, buzzer | `door.c` |
| Inside count and per-user flag | `occupancy.c` |
| Anti-passback, tailgating, denial lockout | `access.c`, `door.c` |

Readers only debounce a card and post a scan. `access_task` is the only task that grants or denies. Door outputs are a third task, so a 3 second door hold does not block the next decision.

## UID format

Text form used by every role: uppercase hex, no separators.

| Wokwi card | UID | Stub user |
| --- | --- | --- |
| Blue | `01020304` | Alice, starts outside |
| Green | `11223344` | Bob, starts outside |
| Yellow | `55667788` | Carol, starts inside |
| Red | `AABBCCDD` | not registered |

`01:02:03:04` is accepted on the serial harness and stored as `01020304`.

## Pins

GPIO 4 is free for Role B's SD-card chip-select. SCK 18, MISO 19 and MOSI 23 are the shared SPI bus. Role B adds its own chip-select and must not take GPIO 5 or GPIO 16.

| Function | GPIO |
| --- | --- |
| Entry RFID CS / RST | 5 / 21 |
| Exit RFID CS / RST | 16 / 17 |
| Entry servo / exit servo | 13 / 25 |
| Entry green / red | 26 / 32 |
| Exit green / red | 33 / 14 |
| Buzzer | 27 |
| Tailgate entry / exit | 34 / 35 |

GPIO 34 and 35 have no internal pull-up. The 10 kΩ resistors in `diagram.json` pull them high. A press reads low.

## Build and simulate

ESP-IDF 5.x and the Wokwi extension for VS Code or Cursor.

```powershell
. $env:IDF_PATH\export.ps1
cd role-a-access
idf.py set-target esp32
idf.py build
```

`wokwi.toml` points at `build/flasher_args.json` and `build/role_a_access.elf`.

On a ThinkPad, F1 turns the keyboard light on and does not open the command list. Use either of these instead:

* Press `Ctrl+Shift+P`, type `Wokwi: Start Simulator`, and press Enter.
* Press `Ctrl+Alt+W`.

On the diagram, click the left reader (`rfid_in`) or the right reader (`rfid_out`). Keys, once that reader is focused: `b` blue, `g` green, `y` yellow, `r` red, `t` tap. Use Tap, not Hold. A halted card stays asleep until it leaves the field and is tapped again.

If a reader prints `version=0x00` or `version=0xFF`, the SPI link to that module is not answering. Serial `IN` and `OUT` still go through the same queue and the same decision function.

## Demo script

Run these on a fresh simulation. Carol starts inside, so the count is 1.

1. Tap blue on the entry reader. Expect `GRANTED`, count 2, entry servo to 90°, green LED. After 3 seconds the servo returns to 0°.
2. Tap blue on entry again. Expect `ALREADY_INSIDE`, red LED, short beep, door stays shut.
3. Tap blue on the exit reader. Expect `GRANTED`, count 1.
4. Tap blue on exit again. Expect `NOT_INSIDE`.
5. Type `CLOSED`, then tap green on entry. Expect `OUTSIDE_HOURS`.
6. Type `EMERGENCY ON`, then tap yellow on exit. Carol is inside, so exit is granted even though the building is closed. Tap yellow on entry. Entry stays denied.
7. Type `EMERGENCY OFF` and `OPEN`.
8. Type `LOCKOUT`. The red card is denied three times, then `tag_blocked` is published, and the fourth scan is `TAG_BLOCKED`.
9. Press **Tail IN** with no recent grant. Expect `TAILGATE`, a beep, and no door movement.
10. Tap a valid entry card and, while the green LED is on, press **Tail IN**. Expect `ignored (valid grant still open)`.
11. Restart the simulation and type `RAPID`. Alice and Bob enter and leave. The count returns to 1. Each scan produces one `LOG` line.

`STATUS` shows the count, who is inside, and any active block. `UNBLOCK` clears the test lockout.

## What the coordinator replaces

* Delete or stop linking `components/platform_stubs/stubs.c`.
* Provide real definitions of the functions in:
  * `storage_api.h` — Role B (`user_lookup`, `set_inside`, `log_event`, `storage_count_inside`)
  * `schedule_api.h` — Role D (`is_building_open`, `exit_allowed`, `emergency_active`, `publish`)
  * `time_api.h` — Role D (SNTP, East Africa Time)
  * `console_api.h` — Role E, if the management console takes the serial port
* Keep the struct layouts in `access_types.h`. `set_inside` must update RAM and return. It is called while the occupancy mutex is held, so it must not wait on the SD card.
* `main/console.c` is the Role A test harness. Role E replaces it with the real menus. `main/app_main.c` shows the required startup order: mutexes and queues first, then tasks. Role E owns the final `app_main`.
* Do not link the stub and the real storage code at the same time. The symbols would be duplicated.

Topics this role publishes:

| Topic | When |
| --- | --- |
| `cocis/entrance/decision` | every grant or deny |
| `cocis/entrance/alert` | tag lockout or tailgate |
| `cocis/entrance/door` | door opened or closed |
| `cocis/entrance/schedule` | stub `OPEN` / `CLOSED` |
| `cocis/entrance/emergency` | stub emergency on or off |

Every access decision produces exactly one `LOG` line. A publish line is the network event, not a second log. A dropped scan prints `SCAN_DROPPED` and is not logged, because no decision was made. The scan queue length is 8 and the send does not wait.

## What to say in the presentation

* Scans go through a queue so the reader task never decides, and a slow door or log cannot stall the SPI read. Two scans at the same moment are two queue items. `access_task` handles them one at a time.
* The inside count uses a mutex, not a binary semaphore. The mutex guards shared data and FreeRTOS inherits the priority of `access_task` if a lower-priority task is holding it. A binary semaphore does not do that, so a medium-priority task could block the door decision (priority inversion).
* The tailgate button is a falling-edge interrupt. The interrupt only calls `xQueueSendFromISR`. `tailgate_task` does the debounce and the alert. SPI cannot run inside an interrupt, so the RFID readers are tasks.
* Outside the schedule, both entry and exit are denied. Emergency mode, activated here by the test command and later by Role E, allows exit for someone who is already inside. It does not allow entry, and it does not let an unknown tag out.
* Three denied attempts on one UID block that UID for 30 seconds. Further attempts refresh the 30 seconds. A later granted exit clears the block. Emergency exit is still allowed for a registered person who is inside, so the lockout cannot trap them.

# GRML 6-Category Base Offset Catalog (`SOVIET64.exe` `1.2.0.9+`)

This catalog documents every declarative offset section in [config/grml_offsets.ini](../config/grml_offsets.ini), grouped by category. Mods can consume any entry at runtime via `host->ResolveNamedOffset("<section>", &out_value)` and declare dependencies in `<Mod>.grml.json` via `"required_offsets"` / `"optional_offsets"`.

---

## Categories

### `Core` (`required = true` for loader essentials)
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Meaning | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `in_game_flag` | `rip_rel32` | `0x11D8DF` → `0x9DEC98` | `const volatile uint8_t*` (`1` = in-game, `0` = main menu) | Main loop flag check |
| `main_menu_ready_state` | `rip_rel32` | `0x11DB42` → `0x9DDC75` | `const uint8_t*` (`g_menuActive`) | Main menu loop |

> **Note:** GRML's DX11 `IDXGISwapChain::Present` / `ResizeBuffers` hook is resolved dynamically via a dummy `D3D11CreateDeviceAndSwapChain` vtable lookup in `overlay_hub.cpp`, requiring no `.text` AOB signature.

### `TimeAndSim`
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Meaning | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `game_speed_state` | `rip_rel32` | `0x11D7B2` → `0xA1F4EC` | `int32_t*` (`0` = paused, `1` = 1x, `2` = >>) | `C3D_TIMER` main loop |
| `c3d_timer_paused` | `rip_rel32` | `0x11D7B2` → `0xA1EEFC` | `uint8_t*` (`C3D_TIMER + 0x0C` paused flag) | `C3D_TIMER` main loop |
| `sim_step` | `call_rel32` | `0x11D8DF` → `0x32AEB0` | `void (__fastcall*)(void* gameObj)` (`PreSimUpdate`) | Main loop sim call |
| `calendar_clock` | `rip_rel32` | `0x31E3F6` → `0xA2FED4` | Calendar/date state struct | `"$DATE_YEAR"` |

### `Economy`
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Meaning | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `resource_vector` | `rip_rel32` | `0xEBA24` → `0xA2B270` | `ResourceVector*` (`begin`, `end`, `cap`) | World-market price table |
| `player_treasury` | `rip_rel32` | `0x40B558` → `0xA1EF20` | Economy/treasury state root | `"$COST_RUB"` |
| `border_customs_tick` | `function_start` | `0x6BDD00` | `void (__fastcall*)(Vehicle*, ...)` (`CargoTransferTick`) | Customs cargo write |
| `waste_threshold_block` | `function_start` | `0x1B7599` | Waste threshold float load block (`+5` mixed, `+47` fraction) | `FindWasteTarget` |

### `EntityManagers`
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Meaning | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `building_manager` | `rip_rel32` | `0x5DB6` → `0x9DDC88` | `void**` Building manager singleton | `"%s/buildings.bin"` |
| `vehicle_manager` | `rip_rel32` | `0x4538E2` → `0xA2E760` | `void**` Vehicle manager singleton | `"%s/vehicles.bin"` |
| `citizen_manager` | `rip_rel32` | `0x31DF34` → `0xA31820` | `void**` Citizen manager singleton | `"$DATE_YEAR"` |
| `evaluate_dist_task` | `function_start` | `0x1FE750` | `EvaluateDistTask(GameObject*, Task0x80*)` | Road & Rail DOs |
| `dispatch_co_task` | `function_start` | `0x169440` | `bool (__fastcall*)(GameObject*, COTask64*, Building*)` | `DispatchCOTask` |
| `can_vehicle_take_task` | `function_start` | `0x1F8C20` | `bool (__fastcall*)(GameObject*, Task0x80*, Vehicle*, DistOffice*)` | `CanVehicleTakeDistTask` |
| `get_veh_cap` | `call_rel32` | `0x1F8D67` → `0x6F1E00` | `float (__fastcall*)(Vehicle*, ResourceRecord*, int, int)` | `GetVehicleResourceCapacity` |
| `road_node_has_snow` | `function_start` | `0x5A3460` | `bool (__fastcall*)(RoadSearchRequest*, RoadNode*)` | Snowplow dispatch |
| `find_water_target` | `function_start` | `0x1956F0` | `Building* (__fastcall*)(GameObject*, Building*)` | Water truck dispatch |
| `find_sewage_target` | `function_start` | `0x195800` | `Building* (__fastcall*)(GameObject*, Building*)` | Sewage truck dispatch |
| `road_vehicle_tick` | `function_start` | `0x6CCDC0` | `RoadVehicleTick(Vehicle*, ...)` | Road vehicle tick |
| `road_vehicle_bld_move` | `function_start` | `0x6ECF00` | `RoadVehicleBuildingMove(Vehicle*, ...)` | Building bay move |
| `road_vehicle_parking_tick` | `function_start` | `0x7025B0` | `RoadVehicleParkingTick(Vehicle*, ...)` | Customs/parking bays |
| `apron_entry_gate` | `function_start` | `0x6CD7B8` | Apron entry gate check inside `RoadVehicleTick` | `bld_active_mover_off` |
| `apron_exit_gate` | `function_start` | `0x6CF1B9` | Apron exit gate check inside `RoadVehicleTick` | `bld_active_mover_off` |

### `Templates`
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Meaning | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `resource_stride` | `disp32` | `0xEBA24` → `0x380` | `uint32_t` `sizeof(ResourceRecord)` (`0x380` on 1.2.x, `0x340` on 1.1.x) | World-market price table |
| `building_template_registry` | `rip_rel32` | `0x1360DD` → `0xA30AF0` | Building template registry | `"buildings_types/%s.ini"` |

### `SaveLoad` & `CameraAndUI`
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Meaning | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `save_game_write` | `function_start` | `0x450F50` | Save serialization entry | `"%s/buildings.bin"` |
| `save_header_read` | `function_start` | `0x4549D0` | Save header reader/writer | `"%s/header.bin"` |
| `load_save_game` | `function_start` | `0x455300` | Save loader entry | `"%s/vehicles.bin"` |
| `camera_controller` | `rip_rel32` | `0x8402AF` → `0x9DC08C` | Camera controller / viewport state | `"%s/header.bin"` |

---

## Contributing New Offsets

1. **Discover candidate RVAs** using `.rdata` string XREFs or MSVC RTTI vtables:
   ```powershell
   .venv\Scripts\python scripts\harvest_anchors.py --exe "<PathToSOVIET64.exe>" --filter <keyword>
   ```
2. **Generate and upsert** a unique (`count == 1`) signature with a 2-instruction safety margin:
   ```powershell
   .venv\Scripts\python scripts\gen_sig.py `
     --exe "<PathToSOVIET64.exe>" `
     --rva 0x<RVA> `
     --name <section_name> `
     --mode <function_start|rip_rel32|call_rel32|disp32> `
     --category <Core|TimeAndSim|Economy|EntityManagers|Templates|SaveLoad|CameraAndUI> `
     --anchor "<string_or_rtti>" `
     --write
   ```
3. **Verify** all entries in `config/grml_offsets.ini` before opening a PR:
   ```powershell
   .venv\Scripts\python scripts\gen_sig.py --exe "<PathToSOVIET64.exe>" --verify-all
   ```

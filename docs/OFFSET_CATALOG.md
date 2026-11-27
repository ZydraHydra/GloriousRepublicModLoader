# GRML Base Offset Catalog (`SOVIET64.exe` `1.2.0.9+`)

This catalog documents every declarative offset section in [config/grml_offsets.ini](../config/grml_offsets.ini), grouped by category (`Core`, `TimeAndSim`, `Economy`, `EntityManagers`, `Templates`, `SaveLoad`, `CameraAndUI`, `LogisticsAndRouting`, `RailAndTerrain`, `UtilitiesAndClimate`, `ProgressionAndRules`, and `WorkshopAndVFS`). Mods can consume any entry at runtime via `host->ResolveNamedOffset("<section>", &out_value)` and declare dependencies in `<Mod>.grml.json` via `"required_offsets"` / `"optional_offsets"`.

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

### `LogisticsAndRouting` (`required = false`)
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Signature | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `TransportLineManager` | `rip_rel32` | `0x457C19` → `0xA317D8` | `void*` (`TransportLineManager*` — line/schedule registry loaded from `%s/lines.bin`) | `"%s/lines.bin"` |

**Deferred / Runtime Tracing Candidates (`LogisticsAndRouting`):**
- `AssignRefuelWaypoint` — Event-driven fuel detour assignment (`bool __fastcall AssignRefuelWaypoint(Vehicle* rcx, Building* rdx, void* r8)`). Requires live breakpoint tracing on fuel-level threshold inside `road_vehicle_tick` (`0x6CCDC0`).
- `AssignRepairWaypoint` — Event-driven repair/tech-service detour assignment (`bool __fastcall AssignRepairWaypoint(Vehicle* rcx, Building* rdx, void* r8)`). Closest UI/editor anchor: `"editor/button_repair.png"` (`0x2B8149`, `0x63A389`); runtime dispatch lives in `road_vehicle_tick` (`0x6CCDC0`).
- `DistributionOfficeManager` — Distribution Office buildings are stored in `building_manager` (`0x9DDC88`) and dispatched via `evaluate_dist_task` (`0x1FE750`); CO/VR preset tools serialize via `0xA2C400` (`"%s/toolvr.bin"` at `0x458620`).
- `CalcRouteDistance` — Network path-distance calculation helper (`float __fastcall CalcRouteDistance(void* rcx, const Vec3* rdx, const Vec3* r8, uint32_t r9d)`). Requires runtime call-stack tracing from `evaluate_dist_task` (`0x1FE750`) / `can_vehicle_take_task` (`0x1F8C20`).

### `RailAndTerrain` (`required = false`)
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Signature | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `RailBlockManager` | `rip_rel32` | `0x45746D` → `0xA2F238` | `RailBlock**` (`std::vector<RailBlock*>` begin; `end` at `0xA2F240`) | `"%s/semaphores.bin"` |
| `TerrainHeightGrid` | `rip_rel32` | `0x012067` → `0x8B1060` | `float (__fastcall**)(C3D_TERRAIN* rcx, C3DVECTOR3 pos)` (IAT slot for `C3D_TERRAIN::CollisionTerrainHeightOverPoint`; `C3D_TERRAIN*` at `[g_GameWorld + 0xF78]`) | `C3D_TERRAIN::CollisionTerrainHeightOverPoint` |

**Deferred / Runtime Tracing Candidates (`RailAndTerrain`):**
- `TrainReservationUpdate` — Event-driven block reservation transition (`void __fastcall TrainReservationUpdate(RailBlock* rcx, Train* rdx, uint8_t r8b)`). Closest verified anchors: `RailBlockManager` (`0xA2F238`, 25 `.text` XREFs including `0x118E1B`, `0x278F25`, `0x454580`) and `"rail_semaphore"` (`0x439020`).

### `UtilitiesAndClimate` (`required = false`)
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Signature | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `ElectricGridManager` | `rip_rel32` | `0x456FF6` → `0xA2E9D8` | `void*` (`ElectricGridManager*` high-tension grid; low-tension at `0xA2E9E0`) | `"%s/electro_high.bin"` |
| `HeatingAndPipeManager` | `rip_rel32` | `0x27478A` → `0xA2EA58` | `void*` (`HeatingAndPipeManager*` district heating & pipe network) | `"%s/heating.bin"` |

**Deferred / Runtime Tracing Candidates (`UtilitiesAndClimate`):**
- `WeatherAndSeasonState` — Ambient temperature, snow depth, and weather state struct. Closest static anchors: `0x5F8401` (`"snow"` threshold comparison on `[rbx + 0xE64]` / `[rbx + 0xE68]` against `0xA33DD0`/`0xA33DD4`) and `0x5F784F` (`"weathers/overcast1.ini"`).

### `ProgressionAndRules` (`required = false`)
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Signature | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `ResearchManager` | `rip_rel32` | `0x310AB9` → `0xA30738` | `ResearchNode*` (`std::vector<ResearchNode>` begin, stride `0x138`; `end` at `0xA30740`) | `"research/research.ini"` |

**Deferred / Runtime Tracing Candidates (`ProgressionAndRules`):**
- `DifficultyFlags` — Active save Realistic Mode / rule bitfield. Closest verified anchor: `save_header_read` (`0x4549D0`, loading `0x20C` bytes of `header.bin` into `0xA33D04`..`0xA33D50`); exact bitfield flags require live save-setting diffing.
- `SpaceProgramState` — `1.2.0.9` Space Program milestone/launch state. Closest static anchors: `0x37A7D0` (`"space"`, reading `[rcx + 0xCD6C]` and `[rcx + 0xC6FC]`), `0x413FC0` (`"Assignment of rocket part failed %s"`), and `0x1DC1F0` (`"rocket_start"`).

### `WorkshopAndVFS` (`required = false`)
| Section | Resolve Mode | Verified `1.2.0.9` RVA / Target | Expected C Type / Signature | Anchor XREF |
| :--- | :--- | :--- | :--- | :--- |
| `WorkshopItemRegistry` | `rip_rel32` | `0x88270D` → `0xA9FE88` | `std::map<uint64_t, std::vector<int>>*` (maps `784150` Steam Workshop IDs to registered template indices) | `"workshop_subscribed/%llu/workshopconfig.ini"` |

**Deferred / Runtime Tracing Candidates (`WorkshopAndVFS`):**
- `ScriptIniParser` — In-memory `.ini` / `script.ini` parser entry point. Closest static anchors: `0x40B271` (`"%s/script.ini"` loader loop inside `0x40B150`, with `ScriptObject::Reset` at `0x409180`) and `0x005E19` (`C3D_TERRAIN::InitializeFromFolder` parsing `%s/script.ini` via `0x5330`).

---

## Contributing New Offsets

1. **Discover candidate RVAs** using `.rdata` string XREFs or MSVC RTTI vtables:
   ```powershell
   .venv\Scripts\python scripts\harvest_anchors.py --exe "<PathToSOVIET64.exe>" --category <CategoryName>
   ```
2. **Generate and upsert** a unique (`count == 1`) signature with a 2-instruction safety margin:
   ```powershell
   .venv\Scripts\python scripts\gen_sig.py `
     --exe "<PathToSOVIET64.exe>" `
     --rva 0x<RVA> `
     --name <section_name> `
     --mode <function_start|rip_rel32|call_rel32|disp32> `
     --category <Core|TimeAndSim|Economy|EntityManagers|Templates|SaveLoad|CameraAndUI|LogisticsAndRouting|RailAndTerrain|UtilitiesAndClimate|ProgressionAndRules|WorkshopAndVFS> `
     --anchor "<string_or_rtti>" `
     --write
   ```
3. **Verify** all entries in `config/grml_offsets.ini` before opening a PR:
   ```powershell
   .venv\Scripts\python scripts\gen_sig.py --exe "<PathToSOVIET64.exe>" --verify-all
   ```

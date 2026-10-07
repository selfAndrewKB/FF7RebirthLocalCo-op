# Address matching 1.004 → 1.005

- addresses in the mod's code: **577** (448 code, 129 data)
- matched: **477**; not found: **100**
- matched ranges: 474; code-match conflicts with call targets: 2
- C++ table: 531 ranges, 7284 moved fields; overlaps with a different delta (later range cut): 0; matches the table does not reproduce: 0

| how | count |
|---|---|
| function | 259 |
| referenced by matched code | 75 |
| window at the address | 65 |
| slot found by its translated pointer | 28 |
| call target from matched code | 13 |
| function where its callers say | 12 |
| window ending at the address | 9 |
| 0x8 past an address matched code references | 3 |
| 0x18 past an address matched code references | 3 |
| 0x10 past an address matched code references | 3 |
| 0x20 past an address matched code references | 2 |
| slot found by its translated pointer and 6 neighbours | 1 |
| slot found by its translated pointer and 3 neighbours | 1 |
| slot found by its translated pointer and 7 neighbours | 1 |
| slot found by its translated pointer and 9 neighbours | 1 |
| 0x40 past an address matched code references | 1 |

## Not found

| address | kind | names / files | note |
|---|---|---|---|
| 0x1408063DC | code | - (player_two/field_pickup.cpp) | ambiguous: own match 0x1410D813C, callers 0x1410D82B4 |
| 0x1408B49D0 | code | - (player_two/charapop_watch.cpp) | not found |
| 0x1408B4ABC | code | - (player_two/charapop_watch.cpp) | not found |
| 0x1409156B4 | code | kProcess (renderer/me_feedback_once.cpp) | not found |
| 0x140915958 | code | kCopy (renderer/me_feedback_once.cpp) | not found |
| 0x140915E38 | code | kProcessCall (renderer/me_feedback_once.cpp) | not found |
| 0x1409F73E4 | code | kLeaderCall (hud/area_icons_two.cpp) | not found |
| 0x1409F7439 | code | kFirstControllerCall (hud/area_icons_two.cpp) | not found |
| 0x1409F94FA | code | - (hud/hud_trace.cpp) | not found |
| 0x140A3D613 | code | kSnapshotCall (player_two/command_menu_tick.cpp) | not found |
| 0x140B1975C | code | - (player_two/party_warp.cpp) | not found |
| 0x140BEE266 | code | kFrameNumberGate (platform/engine_addresses.h) | not found |
| 0x140C07A3C | code | kAcquire (player_two/interact_watch.cpp) | not found |
| 0x140C81DE0 | code | kLeader (hud/area_icons_two.cpp) | not found |
| 0x140C8F058 | code | - (player_two/lock_on.cpp) | not found |
| 0x140C8F9C9 | code | kMarkerNameCall (player_two/lock_on.cpp) | not found |
| 0x140C95CC0 | code | kRenderToCompositeLayer (hud/second_image.cpp) | not found |
| 0x140C960A2 | code | kAcquireCall (hud/second_image.cpp) | not found |
| 0x140C960D9 | code | kEntryCall (hud/hud_addresses.h) | not found |
| 0x140C96258 | code | kSubmitCall (hud/hud_addresses.h) | not found |
| 0x140CF05F8 | code | kHidden (renderer/me_feedback_once.cpp) | not found |
| 0x140D1791C | code | kAddCompositePasses (hud/second_image.cpp) | not found |
| 0x140D9DB40 | code | - (player_two/wheelie_ride.cpp) | not found |
| 0x140DBA287 | code | kForcedCall (hud/enemy_gauges.cpp) | not found |
| 0x140DBA664 | code | kTargetedCall (hud/enemy_gauges.cpp) | not found |
| 0x140DBE165 | code | kAcquireCall (player_two/interact_watch.cpp) | not found |
| 0x140E55E40 | code | - (player_two/engine_facts.h) | not found |
| 0x140E74141 | code | - (player_two/pad_buttons.cpp) | not found |
| 0x140E74312 | code | - (player_two/pad_buttons.cpp) | not found |
| 0x140E745A0 | code | - (player_two/pad_buttons.cpp) | not found |
| 0x140E7468D | code | - (player_two/pad_buttons.cpp) | not found |
| 0x140E7477E | code | - (player_two/pad_buttons.cpp) | not found |
| 0x140E76904 | code | kForced (hud/enemy_gauges.cpp) | not found |
| 0x140F52378 | code | kSetupCall (player_two/command_menu_two.cpp) | not found |
| 0x140F59C50 | code | kSetPlayerContext (hud/hud_roots.cpp) | not found |
| 0x140FC6C95 | code | - (hud/minimap_once.cpp) | not found |
| 0x140FC6CAE | code | kAcquireCall (hud/minimap.cpp, hud/minimap_once.cpp) | not found |
| 0x140FC6CE9 | code | kFindOrAddCall (hud/minimap.cpp) | not found |
| 0x140FC8A98 | code | kFindOrAddNamedLayerEntry (hud/hud_addresses.h) | not found |
| 0x140FDD1F0 | code | kRenderMain (renderer/me_feedback_once.cpp) | not found |
| 0x140FDE3C1 | code | kAddCompositePassesCall (hud/second_image.cpp) | not found |
| 0x1410340A7 | code | - (hud/area_icons_two.cpp) | not found |
| 0x1410340B1 | code | - (hud/area_icons_two.cpp) | not found |
| 0x14106DA88 | code | kRelayout (hud/minimap_labels.cpp) | not found |
| 0x14111A301 | code | kOutputLoadActionSequence (platform/engine_addresses.h) | not found |
| 0x14111A377 | code | kOutputLoadActionStore (platform/engine_addresses.h) | not found |
| 0x14111A37E | code | kOutputLoadActionResume (platform/engine_addresses.h) | not found |
| 0x141203880 | code | - (player_two/lock_on.cpp) | not found |
| 0x141203BEC | code | - (player_two/lock_on.cpp) | not found |
| 0x14131FE21 | code | kGateCall (player_two/combat_rumble.cpp) | not found |
| 0x14131FE78 | code | kCameraCall (player_two/combat_rumble.cpp) | not found |
| 0x14131FF12 | code | - (player_two/combat_rumble.cpp) | not found |
| 0x1413243E9 | code | - (player_two/combat_rumble.cpp) | not found |
| 0x141504270 | code | - (player_two/engine_facts.h) | not found |
| 0x141695750 | code | - (player_two/framing_watch.cpp) | not found |
| 0x141695794 | code | - (player_two/framing_watch.cpp) | not found |
| 0x1416957D8 | code | - (player_two/framing_watch.cpp) | not found |
| 0x1417267F0 | code | kLeaderTargetTick (hud/hud_updates.cpp) | not found |
| 0x14183BD94 | code | - (player_two/party_warp.cpp) | not found |
| 0x141846EEC | code | - (player_two/combat_rumble.cpp) | not found |
| 0x141846F3C | code | - (player_two/combat_rumble.cpp) | not found |
| 0x14184709C | code | - (player_two/combat_rumble.cpp) | not found |
| 0x1418470D2 | code | - (player_two/combat_rumble.cpp) | not found |
| 0x141847117 | code | - (player_two/combat_rumble.cpp) | not found |
| 0x141884358 | code | - (player_two/wheelie_ride.cpp) | not found |
| 0x1418EB3D4 | code | kWeakIsValid (views/whole_screen.cpp) | ambiguous: own match 0x140BF51CC, callers 0x140834A60 |
| 0x1420B8090 | code | - (renderer/family_boundary.cpp) | not found |
| 0x142242DD1 | code | - (player_two/combat_rumble.cpp) | not found |
| 0x1422BA490 | code | kQuery (player_two/interact_watch.cpp) | not found |
| 0x1423E30C0 | code | - (player_two/camera_override.cpp) | not found |
| 0x1423E3730 | code | - (player_two/camera_override.cpp) | not found |
| 0x1423EF160 | code | kUpdateCall (player_two/interact_watch.cpp) | not found |
| 0x1423EF4A1 | code | - (hud/hud_trace.cpp) | not found |
| 0x14295F0A5 | code | kPanelClearedSite (player_two/lock_on.cpp) | not found |
| 0x14295F0AD | code | kPanelClearedResume (player_two/lock_on.cpp) | not found |
| 0x14295F0BE | code | kPanelClearedCall (player_two/lock_on.cpp) | not found |
| 0x14295F0C3 | code | kPanelClearedSkipTo (player_two/lock_on.cpp) | not found |
| 0x14295F139 | code | kPanelSetCall (player_two/lock_on.cpp) | not found |
| 0x142ADCBCA | code | kSetterCall (player_two/camera_distance.cpp) | not found |
| 0x142E714EC | code | kNewRenderTarget (hud/minimap.cpp) | not found |
| 0x1431C9638 | code | - (player_two/party_warp.cpp) | not found |
| 0x1431D851C | code | - (player_two/party_warp.cpp) | not found |
| 0x1433FDFD0 | code | - (player_two/engine_facts.h) | not found |
| 0x14360FBD4 | code | - (player_two/wheelie_ride.cpp) | not found |
| 0x14360FDD0 | code | - (player_two/wheelie_ride.cpp) | not found |
| 0x1439D6A84 | code | - (player_two/lock_on.cpp) | not found |
| 0x143A08A54 | code | - (player_two/command_menu_two.cpp) | not found |
| 0x144F561F0 | code | kRemoveLocalPlayer (platform/engine_addresses.h) | not found |
| 0x144F56202 | code | kRemoveLocalPlayerControllerTest (platform/engine_addresses.h) | not found |
| 0x1451B96CC | code | kUpdateResourceImmediate (hud/minimap.cpp) | not found |
| 0x14583F980 | data | kScooterVt (player_two/wheelie_ride.cpp, player_two/wheelie_watch.cpp) | slot holds 0x1412063E0, which did not match |
| 0x1467C85A0 | data | kEndPlayerControllerVTable (player_two/engine_facts.h) | slot holds 0x143D06F58, which did not match |
| 0x149117AF8 | data | kStreamingManagers (renderer/me_feedback_once.cpp) | not found |
| 0x149347278 | data | kVendingPreset (player_two/vending.cpp) | not found |
| 0x149349880 | data | kInstanceOne (hud/area_icons_two.cpp) | not found |
| 0x1493498C8 | data | kJobBoardSlot (views/whole_screen.cpp) | not found |
| 0x14934D020 | data | kCompositeCollisionNameGuard (player_two/engine_facts.h) | not found |
| 0x14934D028 | data | kCompositeCollisionNameGlobal (player_two/engine_facts.h) | not found |
| 0x14939BC48 | data | kAnimWorld (player_two/charapop_watch.cpp) | not found |
| 0x14939C2F8 | data | kStockNameStatic (hud/minimap.cpp) | not found |

## Where unmatched code went (structural match: displacements and immediates blanked)

Only for the report: these are **not** in the table. `changes` are instructions whose struct offsets, vtable slots or constants differ; `diverged` is where the instruction stream itself changed.

- 0x1408063DC (-): no structural match
- 0x1408B49D0 (-): no structural match
- 0x1408B4ABC (-): no structural match
- 0x1409156B4 (kProcess): no structural match
- 0x140915958 (kCopy): no structural match
- 0x140915E38 (kProcessCall) → likely 0x140BC0ECC (function 0x140BC0E0C): 6 changed instructions
  - +0x8A: `mov rcx, qword ptr [rbx + 0x218]` → `mov rcx, qword ptr [rbx + 0x210]`
  - +0xA0: `mov rcx, qword ptr [rbx + 0x218]` → `mov rcx, qword ptr [rbx + 0x210]`
  - +0xB4: `test byte ptr [rax + 0x220], 1` → `test byte ptr [rax + 0x218], 1`
  - +0xC5: `mov al, byte ptr [rbx + 0x220]` → `mov al, byte ptr [rbx + 0x218]`
  - +0xD3: `mov al, byte ptr [rbx + 0x220]` → `mov al, byte ptr [rbx + 0x218]`
  - +0xDF: `mov byte ptr [rbx + 0x220], al` → `mov byte ptr [rbx + 0x218], al`
- 0x14096ECAC (kApplyTransform): no structural match
- 0x1409E7EF8 (kBeginInitResource): no structural match
- 0x1409F72B8 (kFrameUpdate): no structural match
- 0x1409F73E4 (kLeaderCall): no structural match
- 0x1409F7439 (kFirstControllerCall): no structural match
- 0x1409F94FA (-): no structural match
- 0x140A3D294 (-): no structural match
- 0x140A3D613 (kSnapshotCall): no structural match
- 0x140A402F4 (-) → likely 0x1409B0678 (function 0x1409B0678): 1 changed instructions
  - +0x14: `call qword ptr [rax + 0x520]` → `call qword ptr [rax + 0x528]`
- 0x140A514AC (kIsAnyPlaying): no structural match
- 0x140B1975C (-): no structural match
- 0x140BEE266 (kFrameNumberGate) → likely 0x1409E9D9E (function 0x1409E9CBC): 3 changed instructions; diverges at +0x72 of 0x45F
  - +0x2F: `mov rbx, r8` → `mov rdi, r8`
  - +0x41: `mov rdi, rax` → `mov rbx, rax`
  - +0x4C: `mov rcx, rdi` → `mov rcx, rbx`
- 0x140C07A3C (kAcquire) → likely 0x141531EE0 (function 0x141531EE0): 7 changed instructions; diverges at +0x1BE of 0x493
  - +0x0: `mov qword ptr [rsp + 0x18], rbx` → `mov qword ptr [rsp + 0x20], rbx`
  - +0x2E: `mov rbx, rcx` → `mov rsi, rcx`
  - +0x84: `movsxd rax, dword ptr [rbx + 0x128]` → `movsxd rax, dword ptr [rsi + 0x128]`
  - +0x91: `lea rax, [rbx + 0x118]` → `lea rax, [rsi + 0x118]`
  - +0xB3: `mov rcx, qword ptr [rbx + 0xe0]` → `mov rcx, qword ptr [rsi + 0xe0]`
  - +0x175: `mov rbx, qword ptr [r11 + 0x50]` → `mov rbx, qword ptr [r11 + 0x58]`
  - +0x19D: `mov rax, qword ptr [rbx + 0xe0]` → `mov rax, qword ptr [rsi + 0xe0]`
- 0x140C10F94 (kMarkByKey): no structural match
- 0x140C81DE0 (kLeader): no structural match
- 0x140C8F058 (-): no structural match
- 0x140C8F9C9 (kMarkerNameCall): no structural match
- 0x140C95CC0 (kRenderToCompositeLayer): no structural match
- 0x140C960A2 (kAcquireCall): no structural match
- 0x140C960D9 (kEntryCall): no structural match
- 0x140C96258 (kSubmitCall): no structural match
- 0x140CF05F8 (kHidden): no structural match
- 0x140D1791C (kAddCompositePasses): no structural match
- 0x140D9DB40 (-) → likely 0x1410DA188 (function 0x1410D8570): 21 changed instructions; diverges at +0x886 of 0x4489
  - +0x13: `mov eax, 0x6a0` → `mov eax, 0x6b0`
  - +0x20: `vmovaps xmmword ptr [rsp + 0x690], xmm6` → `vmovaps xmmword ptr [rsp + 0x6a0], xmm6`
  - +0x29: `vmovaps xmmword ptr [rsp + 0x680], xmm7` → `vmovaps xmmword ptr [rsp + 0x690], xmm7`
  - +0x32: `vmovaps xmmword ptr [rsp + 0x670], xmm8` → `vmovaps xmmword ptr [rsp + 0x680], xmm8`
  - +0x3B: `vmovaps xmmword ptr [rsp + 0x660], xmm9` → `vmovaps xmmword ptr [rsp + 0x670], xmm9`
  - +0x44: `vmovaps xmmword ptr [rsp + 0x650], xmm10` → `vmovaps xmmword ptr [rsp + 0x660], xmm10`
  - +0x4D: `vmovaps xmmword ptr [rsp + 0x640], xmm11` → `vmovaps xmmword ptr [rsp + 0x650], xmm11`
  - +0x56: `vmovaps xmmword ptr [rsp + 0x630], xmm12` → `vmovaps xmmword ptr [rsp + 0x640], xmm12`
  - +0x5F: `vmovaps xmmword ptr [rsp + 0x620], xmm13` → `vmovaps xmmword ptr [rsp + 0x630], xmm13`
  - +0x68: `vmovaps xmmword ptr [rsp + 0x610], xmm14` → `vmovaps xmmword ptr [rsp + 0x620], xmm14`
  - +0x71: `vmovaps xmmword ptr [rsp + 0x600], xmm15` → `vmovaps xmmword ptr [rsp + 0x610], xmm15`
  - +0x84: `mov qword ptr [rsp + 0x5f0], rax` → `mov qword ptr [rsp + 0x600], rax`
- 0x140DBA287 (kForcedCall): no structural match
- 0x140DBA664 (kTargetedCall): no structural match
- 0x140DBE165 (kAcquireCall) → likely 0x1410B2E19 (function 0x1410B2684): 0 changed instructions; diverges at +0x76 of 0x9F6
- 0x140E55E40 (-) → likely 0x140D51C60 (function 0x140D51C60): 140 changed instructions
  - +0x13: `mov eax, 0x590` → `mov eax, 0x570`
  - +0x20: `vmovaps xmmword ptr [rsp + 0x580], xmm6` → `vmovaps xmmword ptr [rsp + 0x560], xmm6`
  - +0x29: `vmovaps xmmword ptr [rsp + 0x570], xmm7` → `vmovaps xmmword ptr [rsp + 0x550], xmm7`
  - +0x32: `vmovaps xmmword ptr [rsp + 0x560], xmm8` → `vmovaps xmmword ptr [rsp + 0x540], xmm8`
  - +0x3B: `vmovaps xmmword ptr [rsp + 0x550], xmm9` → `vmovaps xmmword ptr [rsp + 0x530], xmm9`
  - +0x44: `vmovaps xmmword ptr [rsp + 0x540], xmm10` → `vmovaps xmmword ptr [rsp + 0x520], xmm10`
  - +0x4D: `vmovaps xmmword ptr [rsp + 0x530], xmm11` → `vmovaps xmmword ptr [rsp + 0x510], xmm11`
  - +0x56: `vmovaps xmmword ptr [rsp + 0x520], xmm12` → `vmovaps xmmword ptr [rsp + 0x500], xmm12`
  - +0x5F: `vmovaps xmmword ptr [rsp + 0x510], xmm13` → `vmovaps xmmword ptr [rsp + 0x4f0], xmm13`
  - +0x68: `vmovaps xmmword ptr [rsp + 0x500], xmm14` → `vmovaps xmmword ptr [rsp + 0x4e0], xmm14`
  - +0x71: `vmovaps xmmword ptr [rsp + 0x4f0], xmm15` → `vmovaps xmmword ptr [rsp + 0x4d0], xmm15`
  - +0x84: `mov qword ptr [rsp + 0x4e0], rax` → `mov qword ptr [rsp + 0x4c0], rax`
- 0x140E66A14 (-): no structural match
- 0x140E74141 (-): no structural match
- 0x140E74312 (-): no structural match
- 0x140E745A0 (-): no structural match
- 0x140E7468D (-): no structural match
- 0x140E7477E (-): no structural match
- 0x140E76904 (kForced): no structural match
- 0x140F52378 (kSetupCall) → likely 0x1413FBBF8 (function 0x1413FBB9C): 1 changed instructions
  - +0x46: `mov byte ptr [rbx + 0x2d1], 0` → `mov byte ptr [rbx + 0x2d9], 0`
- 0x140F59C50 (kSetPlayerContext): no structural match
- 0x140F5D854 (kClearAll): no structural match
- 0x140FC6C95 (-): no structural match
- 0x140FC6CAE (kAcquireCall): no structural match
- 0x140FC6CE9 (kFindOrAddCall): no structural match
- 0x140FC873C (kSubmitLayerSlot): no structural match
- 0x140FC8A98 (kFindOrAddNamedLayerEntry): no structural match
- 0x140FDD1F0 (kRenderMain): no structural match
- 0x140FDE3C1 (kAddCompositePassesCall): no structural match
- 0x1410340A7 (-) → likely 0x1410D238F (function 0x1410D1670): 3 changed instructions; diverges at +0x70 of 0x1452
  - +0x59: `mov dword ptr [rsp + 0x74], edx` → `mov dword ptr [rsp + 0x64], edx`
  - +0x5D: `mov qword ptr [rsp + 0xb8], rcx` → `mov qword ptr [rsp + 0xc0], rcx`
  - +0x68: `mov qword ptr [rsp + 0xa0], r15` → `mov qword ptr [rsp + 0x90], r15`
- 0x1410340B1 (-) → likely 0x1410D2399 (function 0x1410D1670): 3 changed instructions; diverges at +0x70 of 0x1452
  - +0x59: `mov dword ptr [rsp + 0x74], edx` → `mov dword ptr [rsp + 0x64], edx`
  - +0x5D: `mov qword ptr [rsp + 0xb8], rcx` → `mov qword ptr [rsp + 0xc0], rcx`
  - +0x68: `mov qword ptr [rsp + 0xa0], r15` → `mov qword ptr [rsp + 0x90], r15`
- 0x14106DA88 (kRelayout): no structural match
- 0x14111A301 (kOutputLoadActionSequence): no structural match
- 0x14111A377 (kOutputLoadActionStore): no structural match
- 0x14111A37E (kOutputLoadActionResume): no structural match
- 0x1411BBAD8 (-): no structural match
- 0x141203880 (-): no structural match
- 0x141203BEC (-): no structural match
- 0x14131FE21 (kGateCall): no structural match
- 0x14131FE78 (kCameraCall): no structural match
- 0x14131FF12 (-): no structural match
- 0x1413243E9 (-) → likely 0x140D44F31 (function 0x140D44BF0): 1 changed instructions; diverges at +0x2DA of 0x372
  - +0x16D: `call qword ptr [rax + 0x460]` → `call qword ptr [rax + 0x468]`
- 0x141504270 (-): no structural match
- 0x14161B408 (-): no structural match
- 0x141695750 (-): no structural match
- 0x141695794 (-): no structural match
- 0x1416957D8 (-): no structural match
- 0x1417267F0 (kLeaderTargetTick) → likely 0x1437495F0 (function 0x1437495F0): 1 changed instructions; diverges at +0x87 of 0x24E
  - +0x35: `call qword ptr [rax + 0x160]` → `call qword ptr [rax + 0x168]`
- 0x14183BD94 (-) → likely 0x14183CD64 (function 0x14183CD64): 0 changed instructions; diverges at +0x13B of 0x1C0
- 0x141846EEC (-): no structural match
- 0x141846F3C (-): no structural match
- 0x14184709C (-): no structural match
- 0x1418470D2 (-): no structural match
- 0x141847117 (-): no structural match
- 0x141884358 (-): no structural match
- 0x1418EB3D4 (kWeakIsValid): no structural match
- 0x1419058B4 (-): no structural match
- 0x1420B8090 (-): no structural match
- 0x142242DD1 (-): no structural match
- 0x1422BA490 (kQuery): no structural match
- 0x1423E30C0 (-): no structural match
- 0x1423E3730 (-): no structural match
- 0x1423EF160 (kUpdateCall): no structural match
- 0x1423EF4A1 (-): no structural match
- 0x14295F0A5 (kPanelClearedSite): no structural match
- 0x14295F0AD (kPanelClearedResume): no structural match
- 0x14295F0BE (kPanelClearedCall): no structural match
- 0x14295F0C3 (kPanelClearedSkipTo): no structural match
- 0x14295F139 (kPanelSetCall): no structural match
- 0x142ADCBCA (kSetterCall): no structural match
- 0x142E714EC (kNewRenderTarget): no structural match
- 0x1431C9638 (-): no structural match
- 0x1431D851C (-): no structural match
- 0x1433FDFD0 (-): no structural match
- 0x14360FBD4 (-): no structural match
- 0x14360FDD0 (-): no structural match
- 0x1439D6A84 (-): no structural match
- 0x143A08A54 (-): no structural match
- 0x144F561F0 (kRemoveLocalPlayer): no structural match
- 0x144F56202 (kRemoveLocalPlayerControllerTest): no structural match
- 0x1451B96CC (kUpdateResourceImmediate): no structural match

## vtable slot indices (vtable start = nearest `lea` target at or below the slot)

| slot old | slot new | vtable old → new | index old → new | start maps | names |
|---|---|---|---|---|---|
| 0x1457936B8 | 0x1455E9DF8 | 0x1457936B8 → 0x1455E9DF8 | 0 → 0 | True | kControllerVTable |
| 0x1457936C8 | 0x1455E9E08 | 0x1457936C8 → 0x1455E9E08 | 0 → 0 | True | kPaintVTable |
| 0x1457936D0 | 0x1455E9E10 | 0x1457936D0 → 0x1455E9E10 | 0 → 0 | True | kGcObjectVTable |
| 0x1457936F0 | 0x1455EA0C8 | 0x1457936F0 → 0x1455EA0C8 | 0 → 0 | True | kTextBlockVTable |
| 0x1457936F8 | 0x1455EA0D0 | 0x1457936F0 → 0x1455EA0C8 | 1 → 1 | True | - |
| 0x145795ED8 | 0x145611740 | 0x145795ED8 → 0x145611740 | 0 → 0 | True | kBattlePlayerVTable, kInputComponentVt |
| 0x145795F48 | 0x1456117B0 | 0x145795ED8 → 0x145611740 | 14 → 14 | True | - |
| 0x1457A6720 | 0x1455F61A0 | 0x1457A6718 → 0x1455F6198 | 1 → 1 | None | kRenderEntry |
| 0x1457A6C90 | 0x14566EF08 | 0x1457A6C90 → 0x14566EF08 | 0 → 0 | True | kItemVtable |
| 0x1457C5908 | 0x145674518 | 0x1457C58B0 → 0x1456744C0 | 11 → 11 | None | - |
| 0x1457C5930 | 0x145674540 | 0x1457C58B0 → 0x1456744C0 | 16 → 16 | None | - |
| 0x1457F0988 | 0x145644C98 | 0x1457F0988 → 0x145644C98 | 0 → 0 | True | kOctreeVtable |
| 0x1457FA8B0 | 0x1456CF758 | 0x1457FA8B0 → 0x1456CF758 | 0 → 0 | True | - |
| 0x1457FAB28 | 0x1456CF9D8 | 0x1457FA8B0 → 0x1456CF758 | 79 → 80 **moved** | True | (sum) |
| 0x1457FAB30 | 0x1456CF9E0 | 0x1457FA8B0 → 0x1456CF758 | 80 → 81 **moved** | True | (sum) |
| 0x1457FAB38 | 0x1456CF9E8 | 0x1457FA8B0 → 0x1456CF758 | 81 → 82 **moved** | True | (sum) |
| 0x14580C8F8 | 0x145657190 | 0x14580C8F8 → 0x145657190 | 0 → 0 | True | kFollowStatePlayerVTable |
| 0x1458898E8 | 0x1456E9710 | 0x1458898E8 → 0x1456E9710 | 0 → 0 | True | - |
| 0x145889B60 | 0x1456E9990 | 0x1458898E8 → 0x1456E9710 | 79 → 80 **moved** | True | (sum) |
| 0x14589EE58 | 0x1456FFC00 | 0x14589EE58 → 0x1456FFC00 | 0 → 0 | True | - |
| 0x14589F0D0 | 0x1456FFE80 | 0x14589EE58 → 0x1456FFC00 | 79 → 80 **moved** | True | (sum) |
| 0x14589F0D8 | 0x1456FFE88 | 0x14589EE58 → 0x1456FFC00 | 80 → 81 **moved** | True | (sum) |
| 0x14589F0E0 | 0x1456FFE90 | 0x14589EE58 → 0x1456FFC00 | 81 → 82 **moved** | True | (sum) |
| 0x1458DE3E8 | 0x14574D468 | 0x1458DE150 → 0x14574D1C8 | 83 → 84 **moved** | None | - |
| 0x145CB1650 | 0x145B1D230 | 0x145CB1650 → 0x145B1D230 | 0 → 0 | True | - |
| 0x145CB2918 | 0x145B1E488 | 0x145CB2918 → 0x145B1E488 | 0 → 0 | True | - |
| 0x145CB2C00 | 0x145B1E1A0 | 0x145CB2C00 → 0x145B1E1A0 | 0 → 0 | True | - |
| 0x145CE2C50 | 0x145B4BA30 | 0x145CE2C50 → 0x145B4BA30 | 0 → 0 | True | kCameraModuleVt |
| 0x145CE7158 | 0x145B51B60 | 0x145CE7158 → 0x145B51B60 | 0 → 0 | True | kMoveStateVTable |
| 0x145CE71B0 | 0x145B51BB8 | 0x145CE7158 → 0x145B51B60 | 11 → 11 | True | - |
| 0x145CE71D8 | 0x145B51BE0 | 0x145CE7158 → 0x145B51B60 | 16 → 16 | True | - |
| 0x145D40588 | 0x145BA5CD0 | 0x145D40580 → 0x145BA5CC8 | 1 → 1 | True | - |
| 0x145D40598 | 0x145BA5CE0 | 0x145D40580 → 0x145BA5CC8 | 3 → 3 | True | kKeyUpEntry |
| 0x145D6C490 | 0x145BCADA8 | 0x145D6C068 → 0x145BCA978 | 133 → 134 **moved** | None | - |
| 0x145D6FA30 | 0x145BCED58 | 0x145D6FA30 → 0x145BCED58 | 0 → 0 | True | kCameraManagerVTable |
| 0x145D6FD68 | 0x145BCF098 | 0x145D6FA30 → 0x145BCED58 | 103 → 104 **moved** | True | - |
| 0x145D700C8 | 0x145BCF3F8 | 0x145D6FA30 → 0x145BCED58 | 211 → 212 **moved** | True | - |
| 0x145D769B8 | 0x145BD6710 | 0x145D769B8 → 0x145BD6710 | 0 → 0 | True | kBroncoCoordVt |
| 0x145D76DF0 | 0x145BD6B50 | 0x145D76DF0 → 0x145BD6B50 | 0 → 0 | True | kCoordVt, kScooterCoordVt |
| 0x145D770E0 | 0x145BD6E48 | 0x145D76DF0 → 0x145BD6B50 | 94 → 95 **moved** | True | - |
| 0x145D77220 | 0x145BD6F90 | 0x145D77220 → 0x145BD6F90 | 0 → 0 | True | kBuggyCoordVt |
| 0x145EB1AF8 | 0x145D11A58 | 0x145EB1AF8 → 0x145D11A58 | 0 → 0 | True | kBinned3Vtable |
| 0x146621CF8 | 0x1464AF7B8 | 0x146621CF8 → 0x1464AF7B8 | 0 → 0 | True | kVertexBufferVtable |
| 0x146621D50 | 0x1464AF810 | 0x146621D50 → 0x1464AF810 | 0 → 0 | True | kIndexBufferVtable |
| 0x14668D6C0 | 0x14651B160 | 0x14668D6C0 → 0x14651B160 | 0 → 0 | True | - |
| 0x14668D938 | 0x14651B3E0 | 0x14668D6C0 → 0x14651B160 | 79 → 80 **moved** | True | (sum) |
| 0x1466B4928 | 0x146542338 | 0x1466B4928 → 0x146542338 | 0 → 0 | True | kBattleOperatorVTable |
| 0x1466B4938 | 0x146542348 | 0x1466B4928 → 0x146542338 | 2 → 2 | True | - |
| 0x1466B4948 | 0x146542358 | 0x1466B4928 → 0x146542338 | 4 → 4 | True | - |
| 0x1466B5CB8 | 0x1465437A8 | 0x1466B5CB8 → 0x1465437A8 | 0 → 0 | True | kFieldOperatorVTable |
| 0x1466B5CC8 | 0x1465437B8 | 0x1466B5CB8 → 0x1465437A8 | 2 → 2 | True | - |
| 0x1466B5CD8 | 0x1465437C8 | 0x1466B5CB8 → 0x1465437A8 | 4 → 4 | True | - |
| 0x1466B5D10 | 0x146543800 | 0x1466B5CB8 → 0x1465437A8 | 11 → 11 | True | - |
| 0x1466B6F90 | 0x146544740 | 0x1466B6F80 → 0x146544730 | 2 → 2 | True | kListenerSlot |
| 0x1466B70D0 | 0x1465446D0 | 0x1466B70D0 → 0x1465446D0 | 0 → 0 | True | kModuleVTable |
| 0x1466B9D88 | 0x146547858 | 0x1466B9D88 → 0x146547858 | 0 → 0 | True | kScooterMoveVt |
| 0x1466B9E08 | 0x1465478D8 | 0x1466B9D88 → 0x146547858 | 16 → 16 | True | - |
| 0x1466B9E18 | 0x1465478E8 | 0x1466B9E18 → 0x1465478E8 | 0 → 0 | True | kChocoboMoveVt |
| 0x1466B9E20 | 0x1465478F0 | 0x1466B9E18 → 0x1465478E8 | 1 → 1 | True | - |
| 0x1466B9E70 | 0x146547940 | 0x1466B9E18 → 0x1465478E8 | 11 → 11 | True | - |
| 0x1466B9E98 | 0x146547968 | 0x1466B9E18 → 0x1465478E8 | 16 → 16 | True | - |
| 0x146725260 | 0x1465AFEE0 | 0x146724F30 → 0x1465AFBA8 | 102 → 103 **moved** | None | - |
| 0x146748958 | 0x1465D1F28 | 0x146748628 → 0x1465D1BF0 | 102 → 103 **moved** | None | - |
| 0x14676E5D8 | 0x1465F8248 | 0x14676E5D8 → 0x1465F8248 | 0 → 0 | True | kCirclePaintVTable |
| 0x14676E860 | 0x1465F8270 | 0x14676E860 → 0x1465F8270 | 0 → 0 | True | kOctagonPaintVTable |
| 0x1467C8448 | 0x146653D08 | 0x1467C80E0 → 0x146653998 | 109 → 110 **moved** | None | kNavAreaTickSlot |
| 0x1467C9220 | 0x146654AE0 | 0x1467C85A0 → 0x146653E58 | 400 → 401 **moved** | None | - |
| 0x1467C9230 | 0x146654AF0 | 0x1467C85A0 → 0x146653E58 | 402 → 403 **moved** | None | kSendSlot |
| 0x1467EB420 | 0x1466766C0 | 0x1467EB420 → 0x1466766C0 | 0 → 0 | True | kChocoboCoordVt |
| 0x146959478 | 0x1467E3410 | 0x146959448 → 0x1467E33E0 | 6 → 6 | None | kPadSlot |
| 0x14697BFA8 | 0x1468012C0 | 0x14697BFA8 → 0x1468012C0 | 0 → 0 | True | kContextVtable |
| 0x14697C098 | 0x1468013B0 | 0x14697BFA8 → 0x1468012C0 | 30 → 30 | True | kContextVtable + kSubmitSlot |
| 0x14698B4D0 | 0x1468101B0 | 0x14698B490 → 0x146810170 | 8 → 8 | None | kBeginRenderingViewFamilyEntry |
| 0x146BB33D8 | 0x146A18768 | 0x146BB33D8 → 0x146A18768 | 0 → 0 | True | kLocalPlayerVTable |
| 0x146BB3650 | 0x146A189E8 | 0x146BB33D8 → 0x146A18768 | 79 → 80 **moved** | True | kGetViewPointEntry |

## Conflicts (own match vs call targets from other matched code)

- 0x1408063DC: own 0x1410D813C, callers say 0x1410D82B4 (9/9)
- 0x1418EB3D4: own 0x140BF51CC, callers say 0x140834A60 (29/29)

## All matches

| old | new | delta | kind | how | names |
|---|---|---|---|---|---|
| 0x140000000 | 0x140000000 | +0x0 | data | referenced by matched code (4/4 refs agree) (found by scan) | kIdaImageBase |
| 0x14078C2F4 | 0x1408DAE0C | +0x14eb18 | code | window at the address | - |
| 0x14079F7F0 | 0x140764EA8 | -0x3a948 | code | function where its callers say (23/23 agree) | kFreeString |
| 0x1407BB584 | 0x1410E17B4 | +0x926230 | code | function, equal for 0x9E0 of 0xB4E | kSummonBody |
| 0x1407BC858 | 0x1410E41AC | +0x927954 | code | function, equal for 0x115 of 0x19A | - |
| 0x1407BC991 | 0x1410E42E5 | +0x927954 | code | window at the address | kSummonCall |
| 0x1407BCAF0 | 0x1410E33C4 | +0x9268d4 | code | function, equal for 0x1C1 of 0xCB7 | - |
| 0x1407BDB64 | 0x1410D5270 | +0x91770c | code | window at the address | - |
| 0x1407BDE58 | 0x1410D62F0 | +0x918498 | code | function, equal for 0xCF of 0x62F | - |
| 0x1407BF308 | 0x1411B4D64 | +0x9f5a5c | code | function | kPadSet |
| 0x1407BF920 | 0x1411B41CC | +0x9f48ac | code | function | kSend |
| 0x1407C057C | 0x1409F60B4 | +0x235b38 | code | function | - |
| 0x1407C3DAC | 0x140DD81C0 | +0x614414 | code | function, equal for 0x23A of 0x729 | kActionDown |
| 0x1408057E0 | 0x140891854 | +0x8c074 | code | function, equal for 0x54 of 0x38B | kLayoutCells |
| 0x14080701C | 0x1409B2AE8 | +0x1abacc | code | function | - |
| 0x14080708C | 0x14092E0FC | +0x127070 | code | function | - |
| 0x1408070C4 | 0x14092E134 | +0x127070 | code | window at the address | kLeaderType |
| 0x14081FD74 | 0x1409183E0 | +0xf866c | code | function | - |
| 0x14083F3D8 | 0x14079F980 | -0x9fa58 | code | function | kMalloc |
| 0x1408428F0 | 0x14078CD70 | -0xb5b80 | code | function | - |
| 0x140843C44 | 0x14078BFD0 | -0xb7c74 | code | function, equal for 0xF2 of 0x218 | kRemove |
| 0x140843F10 | 0x140B7BB44 | +0x337c34 | code | function, equal for 0x8C of 0x266A | kHolderUpdate |
| 0x140844016 | 0x140B7BC48 | +0x337c32 | code | window at the address | kLeaderTypeCall |
| 0x1408446C5 | 0x140B7C29F | +0x337bda | code | window at the address | kRemoveArgs |
| 0x1408446CC | 0x140B7C2A6 | +0x337bda | code | window at the address | kRemoveCall |
| 0x1408446D1 | 0x140B7C2AB | +0x337bda | code | window at the address | kRemoveAfter |
| 0x1408447B5 | 0x140B7C38F | +0x337bda | code | window at the address | kUpdateArgs |
| 0x1408447BF | 0x140B7C399 | +0x337bda | code | window at the address | kUpdateCall |
| 0x1408447C4 | 0x140B7C39E | +0x337bda | code | window at the address | kUpdateAfter |
| 0x1408448FA | 0x140B7C4CD | +0x337bd3 | code | window ending at the address | kPieceUnloadArgs |
| 0x140844905 | 0x140B7C4D8 | +0x337bd3 | code | window ending at the address | kPieceUnloadCall |
| 0x14084490A | 0x140B7C4DD | +0x337bd3 | code | window ending at the address | kPieceUnloadAfter |
| 0x1408607C5 | 0x140808949 | -0x57e7c | code | window at the address | - |
| 0x140860829 | 0x140808AEE | -0x57d3b | code | window at the address | - |
| 0x14087F15C | 0x140C2162C | +0x3a24d0 | code | window at the address | - |
| 0x14087F460 | 0x140896EA8 | +0x17a48 | code | function | - |
| 0x140887AE0 | 0x1409772F8 | +0xef818 | code | function where its callers say (4/4 agree) | - |
| 0x140887B00 | 0x1419584EC | +0x10d09ec | code | function | - |
| 0x1408A18D1 | 0x141896A0F | +0xff513e | code | window at the address | - |
| 0x1408A264E | 0x14103165A | +0x78f00c | code | function | - |
| 0x1408A559C | 0x141434C14 | +0xb8f678 | code | function | - |
| 0x1408A6A74 | 0x140945B1C | +0x9f0a8 | code | function | - |
| 0x1408E242C | 0x140930D10 | +0x4e8e4 | code | window at the address | - |
| 0x140963839 | 0x14088AF81 | -0xd88b8 | code | function, equal for 0x1BB of 0x2D9 | kFilterOffCall |
| 0x14096384B | 0x14088AF93 | -0xd88b8 | code | function, equal for 0x1BB of 0x2D9 | kSoundOffCall |
| 0x140963943 | 0x14088B08B | -0xd88b8 | code | window ending at the address | kFilterOnCall |
| 0x140964230 | 0x140977A90 | +0x13860 | code | function | - |
| 0x14096ECAC | 0x140B12FA8 | +0x1a42fc | code | call target from matched code (2/2 agree) | kApplyTransform |
| 0x140970724 | 0x140B12B74 | +0x1a2450 | code | function | kInvalidate |
| 0x14098E221 | 0x1408865C9 | -0x107c58 | code | function, equal for 0x110 of 0x193 | kDtorQuadsLea |
| 0x14098E25D | 0x140886605 | -0x107c58 | code | function, equal for 0x110 of 0x193 | kDtorRunsLea |
| 0x14098E35C | 0x140886704 | -0x107c58 | code | function where its callers say (4/4 agree) | kQuadsDtor |
| 0x14098E3B4 | 0x14088675C | -0x107c58 | code | function where its callers say (2/2 agree) | kRunsDtor |
| 0x14098EB7C | 0x1411A7E08 | +0x81928c | code | function | kSetLabelText |
| 0x14098EBAB | 0x1411A7E37 | +0x81928c | code | function | kSetTextArgs |
| 0x14098EBB1 | 0x1411A7E3D | +0x81928c | code | function | kSetTextCall |
| 0x14098EBB6 | 0x1411A7E42 | +0x81928c | code | function | kSetTextAfter |
| 0x14098EC24 | 0x1411A7EB0 | +0x81928c | code | function, equal for 0x34 of 0x176 | kSetText |
| 0x1409E7EF8 | 0x1408F26F0 | -0xf5808 | code | call target from matched code (2/2 agree) | kBeginInitResource |
| 0x1409EF81C | 0x1409B25D4 | -0x3d248 | code | function | - |
| 0x1409EFE50 | 0x1409B2888 | -0x3d5c8 | code | function | - |
| 0x1409EFF24 | 0x1409B295C | -0x3d5c8 | code | function | - |
| 0x1409EFF7C | 0x1409B29B4 | -0x3d5c8 | code | function, by position | - |
| 0x1409F52B0 | 0x140BC7998 | +0x1d26e8 | code | window at the address | kContainScale |
| 0x1409F72B8 | 0x1409B5D90 | -0x41528 | code | call target from matched code (1/1 agree) | kFrameUpdate |
| 0x1409FA4F6 | 0x1409B9134 | -0x413c2 | code | window at the address | - |
| 0x140A2E488 | 0x1408B1124 | -0x17d364 | code | function | - |
| 0x140A2E4B4 | 0x1408B1150 | -0x17d364 | code | function | kGetEndGameState |
| 0x140A2F648 | 0x1409ADE38 | -0x81810 | code | function | - |
| 0x140A31634 | 0x1409AE1E8 | -0x8344c | code | window at the address | - |
| 0x140A3C384 | 0x140C25A50 | +0x1e96cc | code | function | - |
| 0x140A3D294 | 0x1408938E4 | -0x1a99b0 | code | call target from matched code (1/1 agree) | - |
| 0x140A3F19C | 0x1409AF6E8 | -0x8fab4 | code | function, equal for 0x2AF of 0x33B | - |
| 0x140A3F508 | 0x1409AFA1C | -0x8faec | code | function | - |
| 0x140A3F614 | 0x1409AFB28 | -0x8faec | code | function, equal for 0x7F of 0xD6 | - |
| 0x140A401BC | 0x140C258C0 | +0x1e5704 | code | function, equal for 0x94 of 0xC0 | - |
| 0x140A402F4 | 0x1409B0678 | -0x8fc7c | code | call target from matched code (2/2 agree) | - |
| 0x140A514AC | 0x140A63A00 | +0x12554 | code | call target from matched code (1/1 agree) | kIsAnyPlaying |
| 0x140A53846 | 0x1409321FA | -0x12164c | code | window at the address | kLocationArgument |
| 0x140A538C6 | 0x14093227A | -0x12164c | code | window at the address | kReadyCall |
| 0x140A538CB | 0x14093227F | -0x12164c | code | window at the address | kAfterCall |
| 0x140A59A30 | 0x140E8EA08 | +0x434fd8 | code | function, equal for 0x3A0 of 0x437 | kPlaceMarker |
| 0x140A59D44 | 0x140E8ED1C | +0x434fd8 | code | function, equal for 0x3A0 of 0x437 | kArrowEdgeCheckStart |
| 0x140A59DB8 | 0x140E8ED90 | +0x434fd8 | code | function, equal for 0x3A0 of 0x437 | kArrowEdgeSite |
| 0x140A59DCB | 0x140E8EDA3 | +0x434fd8 | code | function, equal for 0x3A0 of 0x437 | kArrowEdgeSide |
| 0x140A59DCF | 0x140E8EDA7 | +0x434fd8 | code | function, equal for 0x3A0 of 0x437 | - |
| 0x140A59E19 | 0x140E8EDF1 | +0x434fd8 | code | window at the address | kArrowEdgeTopBottom |
| 0x140A6085C | 0x140764048 | -0x2fc814 | code | function | kResolveLeader |
| 0x140A60A10 | 0x1407641A8 | -0x2fc868 | code | window at the address, by position | - |
| 0x140A61108 | 0x1407648A0 | -0x2fc868 | code | function | - |
| 0x140A61210 | 0x1407649A8 | -0x2fc868 | code | window at the address | - |
| 0x140A61654 | 0x140764B68 | -0x2fcaec | code | window at the address | - |
| 0x140A61C20 | 0x140D50180 | +0x2ee560 | code | function | kFinalizeCall |
| 0x140A61D1C | 0x140763D40 | -0x2fdfdc | code | function | kPrimary |
| 0x140A64E05 | 0x140DCF999 | +0x36ab94 | code | function | kGuideShowCall |
| 0x140A64E1B | 0x140DCF9AF | +0x36ab94 | code | function | kGuideMode8Call |
| 0x140A65F88 | 0x140DCF2F4 | +0x36936c | code | function, equal for 0x3E of 0x7E | - |
| 0x140A6699C | 0x140F6F488 | +0x508aec | code | function | kTargeted |
| 0x140A66B85 | 0x140F6F67D | +0x508af8 | code | window at the address | kCreateCall |
| 0x140A677F0 | 0x140F76A20 | +0x50f230 | code | function | - |
| 0x140A68620 | 0x140F6ECE4 | +0x5066c4 | code | function, equal for 0x10A of 0x4E5 | - |
| 0x140A68826 | 0x140F6EEFF | +0x5066d9 | code | window at the address | kLookupCall |
| 0x140A68C84 | 0x140F6DE00 | +0x50517c | code | function | - |
| 0x140A97DE0 | 0x140ECDA58 | +0x435c78 | code | function, equal for 0x35 of 0x8F | kAddViewSlaveLocation |
| 0x140A9CC5C | 0x140D4E4E4 | +0x2b1888 | code | function where its callers say (1/1 agree) | kFinalizePose |
| 0x140A9F840 | 0x140CF11A0 | +0x251960 | code | function, equal for 0x89 of 0xBF | kContainsPoint |
| 0x140ADE688 | 0x1409A3C34 | -0x13aa54 | code | function | kRequest |
| 0x140AF5BFC | 0x141419328 | +0x92372c | code | function where its callers say (2/2 agree) | - |
| 0x140B1AB40 | 0x1410AC788 | +0x591c48 | code | window at the address | - |
| 0x140B20F39 | 0x140F73B8D | +0x452c54 | code | function | kSyncCall |
| 0x140B8AF5C | 0x140974A80 | -0x2164dc | code | function | kScaleStep |
| 0x140B8B10C | 0x140975C7C | -0x215490 | code | function | kYStep |
| 0x140B8B374 | 0x140F754C0 | +0x3ea14c | code | function | - |
| 0x140B99E3C | 0x140A12C04 | -0x187238 | code | function | - |
| 0x140BEE184 | 0x1409E9CBC | -0x2044c8 | code | window at the address | kBeginRenderingViewFamily |
| 0x140BF1270 | 0x1409D4708 | -0x21cb68 | code | function, equal for 0xC8 of 0x2B5 | kGetViewPoint |
| 0x140C07E34 | 0x1415322C9 | +0x92a495 | code | window ending at the address | - |
| 0x140C08A60 | 0x140FEEF3C | +0x3e64dc | code | function | - |
| 0x140C10F94 | 0x14117D034 | +0x56c0a0 | code | call target from matched code (1/1 agree) | kMarkByKey |
| 0x140C26810 | 0x140DD2AD0 | +0x1ac2c0 | code | function where its callers say (1/1 agree) | kUpdate |
| 0x140C27B38 | 0x140DD3400 | +0x1ab8c8 | code | function | kSetVisibility |
| 0x140C29AAC | 0x140DD5994 | +0x1abee8 | code | function | kReleaseWeak |
| 0x140C86C30 | 0x14083A200 | -0x44ca30 | code | function | - |
| 0x140C86DC4 | 0x14083A600 | -0x44c7c4 | code | function | - |
| 0x140C86EA8 | 0x14083A6E4 | -0x44c7c4 | code | function | - |
| 0x140C86FA0 | 0x14083A310 | -0x44cc90 | code | function, equal for 0x103 of 0x187 | - |
| 0x140C89B84 | 0x140F13748 | +0x289bc4 | code | function | - |
| 0x140C8D360 | 0x140F75088 | +0x2e7d28 | code | function, equal for 0x3B2 of 0x436 | - |
| 0x140C8D620 | 0x140F75348 | +0x2e7d28 | code | function, equal for 0x3B2 of 0x436 | - |
| 0x140C8E828 | 0x1416D43F8 | +0xa45bd0 | code | function | - |
| 0x140C8F528 | 0x140977740 | -0x317de8 | code | function | - |
| 0x140C8F560 | 0x140977778 | -0x317de8 | code | function | - |
| 0x140C91AA2 | 0x140B3814E | -0x159954 | code | function | - |
| 0x140C91C57 | 0x140B38303 | -0x159954 | code | function | kEnabledCall |
| 0x140C91C61 | 0x140B3830D | -0x159954 | code | function | kLockedCall |
| 0x140C91CC0 | 0x140B3836C | -0x159954 | code | function | - |
| 0x140C92978 | 0x140B3564D | -0x15d32b | code | window at the address | kTickCall |
| 0x140C958B8 | 0x140B0E8CC | -0x186fec | code | function | kGetCompositeLayerRect |
| 0x140C95F57 | 0x140B0EFAD | -0x186faa | code | window at the address | kOverlayRectCall |
| 0x140C95FBE | 0x140B0F014 | -0x186faa | code | window at the address | kContext |
| 0x140C95FC6 | 0x140B0F01C | -0x186faa | code | window at the address | kSite |
| 0x140C95FCE | 0x140B0F024 | -0x186faa | code | window at the address | kResume |
| 0x140CB3815 | 0x1412E133A | +0x62db25 | code | window at the address | - |
| 0x140CB9618 | 0x14119AD20 | +0x4e1708 | code | function, equal for 0x3B of 0x196 | - |
| 0x140CBF974 | 0x140BC8AB8 | -0xf6ebc | code | function | kProject |
| 0x140CC2F86 | 0x1409EEB6A | -0x2d441c | code | window at the address | kDrawSubmitCall |
| 0x140CC2F89 | 0x1409EEB6D | -0x2d441c | code | window at the address | kDrawSubmitReturn |
| 0x140CC39E8 | 0x140E8F54C | +0x1cbb64 | code | function, equal for 0x39 of 0x96 | kSetPosition, kSetPositionInViewport |
| 0x140CC4353 | 0x1410BE6B7 | +0x3fa364 | code | function | kAdditionalFamilyStore |
| 0x140CCF8E4 | 0x140925D68 | -0x3a9b7c | code | function | kFirstGamePlayer |
| 0x140CCF8E9 | 0x140925D6D | -0x3a9b7c | code | function | kFirstGamePlayerResume |
| 0x140D009A8 | 0x140AE3410 | -0x21d598 | code | function, equal for 0x2A of 0x1E4 | - |
| 0x140D239EA | 0x14089529A | -0x48e750 | code | function | kSlotCall |
| 0x140D23A08 | 0x1408952B8 | -0x48e750 | code | function | - |
| 0x140D9C309 | 0x1410D8951 | +0x33c648 | code | window at the address | kFieldEnterCall |
| 0x140D9FFD8 | 0x1410DC6B7 | +0x33c6df | code | window at the address | kFieldExitCall |
| 0x140DBC198 | 0x140B7685C | -0x24593c | code | function where its callers say (2/2 agree) | - |
| 0x140DBCBBC | 0x1409B25D4 | -0x40a5e8 | code | function | - |
| 0x140DECAE9 | 0x141047E85 | +0x25b39c | code | function, equal for 0x21D of 0x4D5 | kParkNavFlag |
| 0x140DECAF1 | 0x141047E8D | +0x25b39c | code | function, equal for 0x21D of 0x4D5 | kParkLocation |
| 0x140DECB06 | 0x141047EA2 | +0x25b39c | code | function, equal for 0x21D of 0x4D5 | kParkCall |
| 0x140DECB0B | 0x141047EA7 | +0x25b39c | code | function, equal for 0x21D of 0x4D5 | kParkAfterCall |
| 0x140E65D18 | 0x141546794 | +0x6e0a7c | code | function | kKeyUp |
| 0x140E65D1D | 0x141546799 | +0x6e0a7c | code | function | kKeyUpAfterFirst |
| 0x140E66914 | 0x1409F81C8 | -0x46e74c | code | window at the address | kUpRouteCall |
| 0x140E66A14 | 0x1409F84C0 | -0x46e554 | code | call target from matched code (1/1 agree) | - |
| 0x140E77414 | 0x140894A90 | -0x5e2984 | code | function | - |
| 0x140E77419 | 0x140894A95 | -0x5e2984 | code | function | kCommandableResume |
| 0x140E7A700 | 0x141510600 | +0x695f00 | code | window at the address | kUpdate |
| 0x140E80480 | 0x141121208 | +0x2a0d88 | code | window at the address | - |
| 0x140EBE7FC | 0x140F67C0C | +0xa9410 | code | function | - |
| 0x140EF28C8 | 0x140F9A248 | +0xa7980 | code | function | - |
| 0x140EF3078 | 0x140F99D50 | +0xa6cd8 | code | function | kFirstController |
| 0x140EF9648 | 0x141965660 | +0xa6c018 | code | function where its callers say (5/5 agree) | - |
| 0x140F22659 | 0x140C5FA41 | -0x2c2c18 | code | function | kFallGate |
| 0x140F226BD | 0x140C5FAA5 | -0x2c2c18 | code | function | kFallLocation |
| 0x140F22706 | 0x140C5FAEE | -0x2c2c18 | code | function | kFallCall |
| 0x140F2270B | 0x140C5FAF3 | -0x2c2c18 | code | function | kFallAfterCall |
| 0x140F318C3 | 0x141400CAB | +0x4cf3e8 | code | window at the address | - |
| 0x140F31BB5 | 0x141401509 | +0x4cf954 | code | function | - |
| 0x140F31BE8 | 0x14140153C | +0x4cf954 | code | function | - |
| 0x140F37874 | 0x140F763F4 | +0x3eb80 | code | function | - |
| 0x140F37ACC | 0x140F761D0 | +0x3e704 | code | function | - |
| 0x140F37E08 | 0x140F759B0 | +0x3dba8 | code | function, equal for 0x57 of 0x244 | kMasterHudTick |
| 0x140F37F48 | 0x140F75AF0 | +0x3dba8 | code | window at the address | kGuideTickCall |
| 0x140F38078 | 0x140F75C2C | +0x3dbb4 | code | function | - |
| 0x140F3D0CC | 0x140806814 | -0x7368b8 | code | function, equal for 0x122 of 0x2C0 | - |
| 0x140F51D00 | 0x140839B80 | -0x718180 | code | function, equal for 0x87 of 0x22D | kResetAll |
| 0x140F52301 | 0x1413FBB81 | +0x4a9880 | code | window at the address | kResetAllCall |
| 0x140F5D854 | 0x140F62A48 | +0x51f4 | code | call target from matched code (1/1 agree) | kClearAll |
| 0x140F5F708 | 0x14130EF18 | +0x3af810 | code | function | kPlaceSlate |
| 0x140F5FBBC | 0x14130F3D0 | +0x3af814 | code | function | kCenteredFrame |
| 0x140F5FC54 | 0x14130F468 | +0x3af814 | code | function | kCenteredFrameSite |
| 0x140F5FC59 | 0x14130F46D | +0x3af814 | code | function | kCenteredFrameResume |
| 0x140F6E7F4 | 0x140DC82A4 | -0x1a6550 | code | function | - |
| 0x140F7F5A6 | 0x14110E26A | +0x18ecc4 | code | window at the address | kAppliedCall |
| 0x140FA0E80 | 0x140D4BEC0 | -0x254fc0 | code | window at the address | - |
| 0x140FC6F13 | 0x14130A779 | +0x343866 | code | window at the address | kSubmitCall |
| 0x140FC6F18 | 0x14130A77E | +0x343866 | code | window at the address | - |
| 0x140FC7C91 | 0x140A460ED | -0x581ba4 | code | function | kConsumeLoadSite |
| 0x140FC7C98 | 0x140A460F4 | -0x581ba4 | code | function | kConsumeLoadResume |
| 0x140FC8501 | 0x140A477D9 | -0x580d28 | code | function | kLoadVertexBufferSite |
| 0x140FC8508 | 0x140A477E0 | -0x580d28 | code | function | kLoadVertexBufferResume |
| 0x140FC8694 | 0x140A4796C | -0x580d28 | code | function | kAcquireLayerSlot |
| 0x140FC873C | 0x140A47A14 | -0x580d28 | code | call target from matched code (2/2 agree) | kSubmitLayerSlot |
| 0x140FC8E05 | 0x140A48861 | -0x5805a4 | code | function, equal for 0x107 of 0x18D | kDrawGateSite |
| 0x140FC8E0A | 0x140A48866 | -0x5805a4 | code | function, equal for 0x107 of 0x18D | kDrawGateResume |
| 0x14102EA68 | 0x140971F94 | -0x6bcad4 | code | function | - |
| 0x141033388 | 0x1410D1670 | +0x9e2e8 | code | function, equal for 0x5C of 0x1452 | - |
| 0x141035AC8 | 0x141432008 | +0x3fc540 | code | function | kListenerForward |
| 0x14103AC48 | 0x1411CA834 | +0x18fbec | code | function | - |
| 0x1410A0156 | 0x1409F205E | -0x6ae0f8 | code | window at the address | kRenderRootOverlayCall |
| 0x1410A80E0 | 0x1408399D4 | -0x86e70c | code | function | - |
| 0x1410A8F78 | 0x14143376C | +0x38a7f4 | code | function | - |
| 0x1410A9168 | 0x141371D08 | +0x2c8ba0 | code | function | - |
| 0x1410E5AD8 | 0x14113115C | +0x4b684 | code | function, equal for 0x73 of 0x135 | - |
| 0x1411BBAD8 | 0x14144729C | +0x28b7c4 | code | call target from matched code (1/1 agree) | - |
| 0x1411BDE60 | 0x140FED95C | -0x1d0504 | code | function, equal for 0x34 of 0x41B | kAddPoint |
| 0x1411BF1FC | 0x1417C9044 | +0x609e48 | code | function | - |
| 0x1411F1434 | 0x14168E1EC | +0x49cdb8 | code | function | - |
| 0x1411F14AC | 0x14168E264 | +0x49cdb8 | code | function | - |
| 0x1411F1B3C | 0x14168D414 | +0x49b8d8 | code | function | - |
| 0x1411F1B74 | 0x14168D44C | +0x49b8d8 | code | function | - |
| 0x1411F52D8 | 0x14137CDF0 | +0x187b18 | code | function, equal for 0x35 of 0x4F | - |
| 0x1411F6B30 | 0x1410314B8 | -0x1c5678 | code | function | - |
| 0x1411F6B6B | 0x1410314F3 | -0x1c5678 | code | function | - |
| 0x1411F6EE1 | 0x141030C3D | -0x1c62a4 | code | function | - |
| 0x1411F6EF4 | 0x14102F888 | -0x1c766c | code | function | - |
| 0x1411F6F41 | 0x14102F8D5 | -0x1c766c | code | function | - |
| 0x1411F7122 | 0x14102FB4E | -0x1c75d4 | code | function | - |
| 0x1411F71E8 | 0x14102FC14 | -0x1c75d4 | code | function | kPlay |
| 0x1411F7234 | 0x14102FC60 | -0x1c75d4 | code | function | kFromDispatcher |
| 0x1411F7244 | 0x14102FC70 | -0x1c75d4 | code | function, equal for 0x31 of 0x25A | kPlayClassic |
| 0x1411F7B64 | 0x141030618 | -0x1c754c | code | function, equal for 0x37 of 0x114 | - |
| 0x1411F7BB9 | 0x141030661 | -0x1c7558 | code | window at the address | - |
| 0x1412041A4 | 0x141433270 | +0x22f0cc | code | function | - |
| 0x141204E09 | 0x140F8B28D | -0x279b7c | code | function, equal for 0x90 of 0xBE | kLeaderCall |
| 0x1412051B4 | 0x1412FA9F4 | +0xf5840 | code | function, equal for 0x320 of 0x371 | - |
| 0x141206FCC | 0x1410EAE4C | -0x11c180 | code | function, equal for 0xD7 of 0x163 | - |
| 0x14122FBA0 | 0x14197DC1C | +0x74e07c | code | function, equal for 0x9E of 0x1B4A | kCompassFrameUpdate |
| 0x141258734 | 0x140E88050 | -0x3d06e4 | code | function, equal for 0x31E of 0x3E2 | - |
| 0x14125A6F0 | 0x140E89110 | -0x3d15e0 | code | function | - |
| 0x14125AECC | 0x140E89EB0 | -0x3d101c | code | function, equal for 0x51 of 0xD6 | - |
| 0x14125C16C | 0x1410E519C | -0x176fd0 | code | function | - |
| 0x14125C1A7 | 0x1410E51D7 | -0x176fd0 | code | function | kClockIncrement |
| 0x1412F376F | 0x141805B33 | +0x5123c4 | code | function | - |
| 0x14131FEA2 | 0x14139787C | +0x779da | code | window at the address | kFallbackCall |
| 0x14132407C | 0x1411AE5B0 | -0x175acc | code | function | - |
| 0x1413A0E50 | 0x140956BFF | -0xa4a251 | code | window ending at the address | - |
| 0x1413C2F70 | 0x1415868D0 | +0x1c3960 | code | function, equal for 0x7C of 0x183 | - |
| 0x141406348 | 0x14088B6B0 | -0xb7ac98 | code | function | - |
| 0x1414A8A94 | 0x14117CF38 | -0x32bb5c | code | function, equal for 0x53 of 0x73 | kEnd2Key |
| 0x1414A8B08 | 0x14117CDF8 | -0x32bd10 | code | function | kChestKey |
| 0x1414A90AD | 0x14117CFFD | -0x32c0b0 | code | function | kMarkFromListener |
| 0x1414A90E4 | 0x14117D390 | -0x32bd54 | code | function | kExecuteSlot |
| 0x1414A9B14 | 0x1409FB938 | -0xaae1dc | code | function | kSortAndLayout |
| 0x1414A9CC0 | 0x14344480C | +0x1f9ab4c | code | function, equal for 0x30 of 0x51 | - |
| 0x1414A9D7C | 0x1410D7D64 | -0x3d2018 | code | function | - |
| 0x1414AA14C | 0x14117DB24 | -0x32c628 | code | function | - |
| 0x1414FA05C | 0x140824C28 | -0xcd5434 | code | window at the address | - |
| 0x141513F24 | 0x140E7A7B8 | -0x69976c | code | function | kPieceShouldUnload |
| 0x14158B0F4 | 0x1413FBCEC | -0x18f408 | code | function, equal for 0xA4 of 0x4C5 | - |
| 0x14158BAB0 | 0x140F8A52C | -0x601584 | code | window at the address | kCycleCall |
| 0x14158BD82 | 0x140F8A83A | -0x601548 | code | function | kCanSwitchCall |
| 0x1415F4900 | 0x140E936D4 | -0x76122c | code | function | kNoteLastDevice |
| 0x141614E6C | 0x140975D60 | -0xc9f10c | code | function | - |
| 0x14161B408 | 0x1418DE9B8 | +0x2c35b0 | code | call target from matched code (1/1 agree) | - |
| 0x14161B4E0 | 0x141866A44 | +0x24b564 | code | function where its callers say (1/1 agree) | - |
| 0x141627E98 | 0x14167E258 | +0x563c0 | code | function | kRenderRootOverlay |
| 0x14166129C | 0x1416E75A4 | +0x86308 | code | function where its callers say (11/11 agree) | - |
| 0x141662054 | 0x1416CA728 | +0x686d4 | code | function | kNavAreaTick |
| 0x14168E1B8 | 0x140B34280 | -0xb59f38 | code | function | - |
| 0x14169AE88 | 0x140ED3D10 | -0x7c7178 | code | function | kGate |
| 0x1416FADE5 | 0x1407A611D | -0xf54cc8 | code | function | - |
| 0x1416FAE10 | 0x141161EC0 | -0x598f50 | code | function | kCamera |
| 0x14176201C | 0x141814774 | +0xb2758 | code | function | kByWidget |
| 0x141762021 | 0x141814779 | +0xb2758 | code | function | kByWidgetSite |
| 0x141762026 | 0x14181477E | +0xb2758 | code | function | kByWidgetResume |
| 0x1418022AB | 0x14188ACE3 | +0x88a38 | code | function | kStatusCall |
| 0x1418022B8 | 0x14188ACF0 | +0x88a38 | code | function | - |
| 0x14180A4D6 | 0x140FE6BF9 | -0x8238dd | code | window ending at the address | kConfirmCall |
| 0x141816EE0 | 0x140F6293C | -0x8b45a4 | code | function | kHideAndClear |
| 0x141816F1C | 0x140F62978 | -0x8b45a4 | code | function | kEnable |
| 0x14181ACC0 | 0x1418BB3A8 | +0xa06e8 | code | function | - |
| 0x14181AD0C | 0x1418BB3F4 | +0xa06e8 | code | function | - |
| 0x14181AD44 | 0x1418BB42C | +0xa06e8 | code | function | - |
| 0x14181AD87 | 0x1418BB46F | +0xa06e8 | code | function | - |
| 0x14182BE00 | 0x141432B38 | -0x3f92c8 | code | function | kSetLayer7 |
| 0x141836901 | 0x141909E45 | +0xd3544 | code | window at the address | kChooserHpCall |
| 0x14184DE48 | 0x1418CE2A4 | +0x8045c | code | function | - |
| 0x141884577 | 0x1415DC50B | -0x2a806c | code | window ending at the address | - |
| 0x14189B358 | 0x140F89314 | -0x912044 | code | function | - |
| 0x1418B2F00 | 0x141907924 | +0x54a24 | code | window at the address | - |
| 0x1419058B4 | 0x140834850 | -0x10d1064 | code | call target from matched code (14/14 agree) | - |
| 0x141905998 | 0x1408349FC | -0x10d0f9c | code | window at the address | kWeakGet |
| 0x142AED1BB | 0x14274A8AB | -0x3a2910 | code | function | - |
| 0x1431CF7D4 | 0x142ED7394 | -0x2f8440 | code | function | - |
| 0x1431D84F4 | 0x142EDFAC0 | -0x2f8a34 | code | function | - |
| 0x14325F71C | 0x142F6FB80 | -0x2efb9c | code | function | - |
| 0x14325F752 | 0x142F6FBB6 | -0x2efb9c | code | function | - |
| 0x14325F7A5 | 0x142F6FC09 | -0x2efb9c | code | function | - |
| 0x14331A4A8 | 0x14302A2A4 | -0x2f0204 | code | function, equal for 0x388 of 0x3A8 | - |
| 0x14331A814 | 0x14302A610 | -0x2f0204 | code | function, equal for 0x388 of 0x3A8 | kDetachLeaderSite |
| 0x1433F94EC | 0x1431112A4 | -0x2e8248 | code | function | - |
| 0x143444B64 | 0x1431647E4 | -0x2e0380 | code | function | kGrant |
| 0x143447250 | 0x14316707C | -0x2e01d4 | code | function | kLeaderTest |
| 0x1434488E7 | 0x143168703 | -0x2e01e4 | code | function, equal for 0x228 of 0x270 | kLeaderTestCall |
| 0x14344F218 | 0x14316F4EC | -0x2dfd2c | code | function | - |
| 0x1434502B0 | 0x143170530 | -0x2dfd80 | code | function | - |
| 0x143450444 | 0x1431706C4 | -0x2dfd80 | code | function, equal for 0x13F of 0x25C | - |
| 0x1434506A0 | 0x143170920 | -0x2dfd80 | code | function | - |
| 0x1434507BF | 0x143170A3F | -0x2dfd80 | code | function | kOwnGetOffSite |
| 0x143451298 | 0x143171514 | -0x2dfd84 | code | function | - |
| 0x143451320 | 0x14317159C | -0x2dfd84 | code | function, equal for 0xBC of 0x1429 | - |
| 0x1434973C6 | 0x1431BC6BE | -0x2dad08 | code | window at the address | - |
| 0x1434AFE70 | 0x1431D8E08 | -0x2d7068 | code | function | - |
| 0x1434E1CDE | 0x14320CFB6 | -0x2d4d28 | code | window at the address | - |
| 0x1434E1D6A | 0x14320D042 | -0x2d4d28 | code | function | - |
| 0x1434E1E2E | 0x14320D106 | -0x2d4d28 | code | function | - |
| 0x1434E1F4F | 0x14320D227 | -0x2d4d28 | code | function | - |
| 0x1434E2013 | 0x14320D2EB | -0x2d4d28 | code | function | - |
| 0x143544CB0 | 0x143273DD8 | -0x2d0ed8 | code | function | - |
| 0x143544CB9 | 0x143273DE1 | -0x2d0ed8 | code | function | - |
| 0x143550C90 | 0x14327FE28 | -0x2d0e68 | code | function | kCameraOff |
| 0x143550CC4 | 0x14327FE5C | -0x2d0e68 | code | function, equal for 0xB4 of 0x16C | kCamera |
| 0x1435548D4 | 0x143283B58 | -0x2d0d7c | code | function | - |
| 0x143554989 | 0x143283C0D | -0x2d0d7c | code | function | - |
| 0x14356050B | 0x143290977 | -0x2cfb94 | code | window ending at the address | - |
| 0x143561FD5 | 0x143293005 | -0x2cefd0 | code | function, equal for 0x56 of 0x67 | - |
| 0x1435680F4 | 0x143299B60 | -0x2ce594 | code | window at the address | - |
| 0x1439C4B30 | 0x143704440 | -0x2c06f0 | code | function | - |
| 0x1439D0E08 | 0x143711330 | -0x2bfad8 | code | function | - |
| 0x1439D1A74 | 0x143712154 | -0x2bf920 | code | function | - |
| 0x1439D1AA0 | 0x143712180 | -0x2bf920 | code | function | - |
| 0x1439D1CEC | 0x1437123CC | -0x2bf920 | code | function | kSetFlagD1 |
| 0x143A0D944 | 0x14374F5F4 | -0x2be350 | code | function | kCtor |
| 0x143A1DF44 | 0x1437601F0 | -0x2bdd54 | code | function | - |
| 0x143A41704 | 0x143786010 | -0x2bb6f4 | code | function, equal for 0xBD of 0xDF | kPlaceUmg |
| 0x143A49DC3 | 0x14378EE37 | -0x2baf8c | code | function | - |
| 0x143A49EDB | 0x14378EF4F | -0x2baf8c | code | function | - |
| 0x143A60235 | 0x1437A5395 | -0x2baea0 | code | window at the address | - |
| 0x143A6024B | 0x1437A53AB | -0x2baea0 | code | window at the address | - |
| 0x143A604E0 | 0x1437A5635 | -0x2baeab | code | window at the address | - |
| 0x143A604E5 | 0x1437A563A | -0x2baeab | code | window at the address | - |
| 0x143D0016C | 0x143A61654 | -0x29eb18 | code | function | - |
| 0x143D00E00 | 0x143A622E8 | -0x29eb18 | code | function | kCreateNavArea |
| 0x143D02698 | 0x143A63A80 | -0x29ec18 | code | function | - |
| 0x143D0A4CC | 0x143A6F604 | -0x29aec8 | code | function, equal for 0x52 of 0xC4 | - |
| 0x143D33994 | 0x143A99A3C | -0x299f58 | code | function, equal for 0x60 of 0x60E | - |
| 0x143D39344 | 0x143A9F73C | -0x299c08 | code | function | - |
| 0x143DA1460 | 0x1427C0BC8 | -0x15e0898 | code | function where its callers say (2/2 agree) | - |
| 0x143DA83F0 | 0x143B15120 | -0x2932d0 | code | function, equal for 0xA3 of 0x188 | - |
| 0x143DAAAB8 | 0x143B17A0C | -0x2930ac | code | function | - |
| 0x143DAAB98 | 0x143B17AEC | -0x2930ac | code | function, equal for 0x77 of 0x241 | - |
| 0x143DAB214 | 0x143B18214 | -0x293000 | code | function | - |
| 0x143DAEE14 | 0x143B1C234 | -0x292be0 | code | function | - |
| 0x144CED910 | 0x144AD2184 | -0x21b78c | code | function | kBlueprintSetPositionCall |
| 0x1451803CC | 0x144FB0A50 | -0x1cf97c | code | function | kPlayerHostGeometry |
| 0x1451B2EA0 | 0x144FE8360 | -0x1cab40 | code | function, equal for 0x5D of 0x61 | kInitCustomFormat |
| 0x1457936B8 | 0x1455E9DF8 | -0x1a98c0 | data | referenced by matched code (1/1 refs agree) (found by scan) | kControllerVTable |
| 0x1457936C8 | 0x1455E9E08 | -0x1a98c0 | data | referenced by matched code (1/1 refs agree) | kPaintVTable |
| 0x1457936D0 | 0x1455E9E10 | -0x1a98c0 | data | referenced by matched code (1/1 refs agree) | kGcObjectVTable |
| 0x1457936F0 | 0x1455EA0C8 | -0x1a9628 | data | referenced by matched code (1/1 refs agree) | kTextBlockVTable |
| 0x1457936F8 | 0x1455EA0D0 | -0x1a9628 | data | 0x8 past an address matched code references | - |
| 0x145795ED8 | 0x145611740 | -0x184798 | data | referenced by matched code (2/2 refs agree) (found by scan) | kBattlePlayerVTable, kInputComponentVt |
| 0x145795F48 | 0x1456117B0 | -0x184798 | data | slot found by its translated pointer | - |
| 0x1457A6720 | 0x1455F61A0 | -0x1b0580 | data | slot found by its translated pointer | kRenderEntry |
| 0x1457A6C90 | 0x14566EF08 | -0x137d88 | data | referenced by matched code (1/1 refs agree) (found by scan) | kItemVtable |
| 0x1457C5908 | 0x145674518 | -0x1513f0 | data | slot found by its translated pointer | - |
| 0x1457C5930 | 0x145674540 | -0x1513f0 | data | slot found by its translated pointer | - |
| 0x1457F0988 | 0x145644C98 | -0x1abcf0 | data | referenced by matched code (3/3 refs agree) (found by scan) | kOctreeVtable |
| 0x1457FA8B0 | 0x1456CF758 | -0x12b158 | data | referenced by matched code (1/1 refs agree) (found by scan) | - |
| 0x1457FAB28 | 0x1456CF9D8 | -0x12b150 | data | slot found by its translated pointer | (sum) |
| 0x1457FAB30 | 0x1456CF9E0 | -0x12b150 | data | slot found by its translated pointer | (sum) |
| 0x1457FAB38 | 0x1456CF9E8 | -0x12b150 | data | slot found by its translated pointer | (sum) |
| 0x14580C8F8 | 0x145657190 | -0x1b5768 | data | referenced by matched code (3/3 refs agree) (found by scan) | kFollowStatePlayerVTable |
| 0x1458898E8 | 0x1456E9710 | -0x1a01d8 | data | referenced by matched code (1/1 refs agree) (found by scan) | - |
| 0x145889B60 | 0x1456E9990 | -0x1a01d0 | data | slot found by its translated pointer | (sum) |
| 0x14589EE58 | 0x1456FFC00 | -0x19f258 | data | referenced by matched code (1/1 refs agree) (found by scan) | - |
| 0x14589F0D0 | 0x1456FFE80 | -0x19f250 | data | slot found by its translated pointer | (sum) |
| 0x14589F0D8 | 0x1456FFE88 | -0x19f250 | data | slot found by its translated pointer | (sum) |
| 0x14589F0E0 | 0x1456FFE90 | -0x19f250 | data | slot found by its translated pointer | (sum) |
| 0x1458DE3E8 | 0x14574D468 | -0x190f80 | data | slot found by its translated pointer and 6 neighbours | - |
| 0x145CB1650 | 0x145B1D230 | -0x194420 | data | referenced by matched code (1/1 refs agree) (found by scan) | - |
| 0x145CB2918 | 0x145B1E488 | -0x194490 | data | referenced by matched code (2/2 refs agree) (found by scan) | - |
| 0x145CB2C00 | 0x145B1E1A0 | -0x194a60 | data | referenced by matched code (1/1 refs agree) (found by scan) | - |
| 0x145CE2C50 | 0x145B4BA30 | -0x197220 | data | referenced by matched code (2/2 refs agree) (found by scan) | kCameraModuleVt |
| 0x145CE7158 | 0x145B51B60 | -0x1955f8 | data | referenced by matched code (1/1 refs agree) (found by scan) | kMoveStateVTable |
| 0x145CE71B0 | 0x145B51BB8 | -0x1955f8 | data | slot found by its translated pointer | - |
| 0x145CE71D8 | 0x145B51BE0 | -0x1955f8 | data | slot found by its translated pointer and 3 neighbours | - |
| 0x145D40588 | 0x145BA5CD0 | -0x19a8b8 | data | 0x8 past an address matched code references (found by scan) | - |
| 0x145D40598 | 0x145BA5CE0 | -0x19a8b8 | data | 0x18 past an address matched code references (found by scan) | kKeyUpEntry |
| 0x145D6C490 | 0x145BCADA8 | -0x1a16e8 | data | slot found by its translated pointer | - |
| 0x145D6FA30 | 0x145BCED58 | -0x1a0cd8 | data | referenced by matched code (1/1 refs agree) (found by scan) | kCameraManagerVTable |
| 0x145D6FD68 | 0x145BCF098 | -0x1a0cd0 | data | slot found by its translated pointer | - |
| 0x145D700C8 | 0x145BCF3F8 | -0x1a0cd0 | data | slot found by its translated pointer | - |
| 0x145D769B8 | 0x145BD6710 | -0x1a02a8 | data | referenced by matched code (1/1 refs agree) (found by scan) | kBroncoCoordVt |
| 0x145D76DF0 | 0x145BD6B50 | -0x1a02a0 | data | referenced by matched code (1/1 refs agree) (found by scan) | kCoordVt, kScooterCoordVt |
| 0x145D770E0 | 0x145BD6E48 | -0x1a0298 | data | slot found by its translated pointer | - |
| 0x145D77220 | 0x145BD6F90 | -0x1a0290 | data | referenced by matched code (1/1 refs agree) (found by scan) | kBuggyCoordVt |
| 0x145EB1AF8 | 0x145D11A58 | -0x1a00a0 | data | referenced by matched code (2/2 refs agree) (found by scan) | kBinned3Vtable |
| 0x1460A2ABC | 0x145F0A2F0 | -0x1987cc | data | referenced by matched code (1/1 refs agree) | kArrowEdgeSixty |
| 0x1460A2D88 | 0x145F09DD0 | -0x198fb8 | data | referenced by matched code (1/1 refs agree) | kClockMsScale |
| 0x1460A3814 | 0x145F0A7F4 | -0x199020 | data | referenced by matched code (1/1 refs agree) | kArrowEdgeOneTwenty |
| 0x146621CF8 | 0x1464AF7B8 | -0x172540 | data | referenced by matched code (1/1 refs agree) (found by scan) | kVertexBufferVtable |
| 0x146621D50 | 0x1464AF810 | -0x172540 | data | referenced by matched code (1/1 refs agree) (found by scan) | kIndexBufferVtable |
| 0x14668D6C0 | 0x14651B160 | -0x172560 | data | referenced by matched code (1/1 refs agree) (found by scan) | - |
| 0x14668D938 | 0x14651B3E0 | -0x172558 | data | slot found by its translated pointer | (sum) |
| 0x1466B4928 | 0x146542338 | -0x1725f0 | data | referenced by matched code (1/1 refs agree) (found by scan) | kBattleOperatorVTable |
| 0x1466B4938 | 0x146542348 | -0x1725f0 | data | 0x10 past an address matched code references (found by scan) | - |
| 0x1466B4948 | 0x146542358 | -0x1725f0 | data | 0x20 past an address matched code references (found by scan) | - |
| 0x1466B5CB8 | 0x1465437A8 | -0x172510 | data | referenced by matched code (1/1 refs agree) (found by scan) | kFieldOperatorVTable |
| 0x1466B5CC8 | 0x1465437B8 | -0x172510 | data | 0x10 past an address matched code references (found by scan) | - |
| 0x1466B5CD8 | 0x1465437C8 | -0x172510 | data | 0x20 past an address matched code references (found by scan) | - |
| 0x1466B5D10 | 0x146543800 | -0x172510 | data | slot found by its translated pointer and 7 neighbours | - |
| 0x1466B6F90 | 0x146544740 | -0x172850 | data | 0x10 past an address matched code references (found by scan) | kListenerSlot |
| 0x1466B70D0 | 0x1465446D0 | -0x172a00 | data | referenced by matched code (3/3 refs agree) (found by scan) | kModuleVTable |
| 0x1466B9D88 | 0x146547858 | -0x172530 | data | referenced by matched code (2/2 refs agree) (found by scan) | kScooterMoveVt |
| 0x1466B9E08 | 0x1465478D8 | -0x172530 | data | slot found by its translated pointer | - |
| 0x1466B9E18 | 0x1465478E8 | -0x172530 | data | referenced by matched code (2/2 refs agree) (found by scan) | kChocoboMoveVt |
| 0x1466B9E20 | 0x1465478F0 | -0x172530 | data | 0x8 past an address matched code references (found by scan) | - |
| 0x1466B9E70 | 0x146547940 | -0x172530 | data | slot found by its translated pointer | - |
| 0x1466B9E98 | 0x146547968 | -0x172530 | data | slot found by its translated pointer | - |
| 0x146725260 | 0x1465AFEE0 | -0x175380 | data | slot found by its translated pointer | - |
| 0x146748958 | 0x1465D1F28 | -0x176a30 | data | slot found by its translated pointer | - |
| 0x14676E5D8 | 0x1465F8248 | -0x176390 | data | referenced by matched code (1/1 refs agree) (found by scan) | kCirclePaintVTable |
| 0x14676E860 | 0x1465F8270 | -0x1765f0 | data | referenced by matched code (1/1 refs agree) (found by scan) | kOctagonPaintVTable |
| 0x1467C8448 | 0x146653D08 | -0x174740 | data | slot found by its translated pointer | kNavAreaTickSlot |
| 0x1467C9220 | 0x146654AE0 | -0x174740 | data | slot found by its translated pointer | - |
| 0x1467C9230 | 0x146654AF0 | -0x174740 | data | slot found by its translated pointer and 9 neighbours | kSendSlot |
| 0x1467EB420 | 0x1466766C0 | -0x174d60 | data | referenced by matched code (1/1 refs agree) (found by scan) | kChocoboCoordVt |
| 0x146959478 | 0x1467E3410 | -0x176068 | data | slot found by its translated pointer | kPadSlot |
| 0x14697BFA8 | 0x1468012C0 | -0x17ace8 | data | referenced by matched code (2/2 refs agree) (found by scan) | kContextVtable |
| 0x14697C098 | 0x1468013B0 | -0x17ace8 | data | slot found by its translated pointer | kContextVtable + kSubmitSlot |
| 0x14698B4D0 | 0x1468101B0 | -0x17b320 | data | slot found by its translated pointer | kBeginRenderingViewFamilyEntry |
| 0x146BB33D8 | 0x146A18768 | -0x19ac70 | data | referenced by matched code (1/1 refs agree) (found by scan) | kLocalPlayerVTable |
| 0x146BB3650 | 0x146A189E8 | -0x19ac68 | data | slot found by its translated pointer | kGetViewPointEntry |
| 0x146FA9A00 | 0x146DDC920 | -0x1cd0e0 | data | referenced by matched code (2/2 refs agree) | kClockMs |
| 0x146FAA330 | 0x146DDD200 | -0x1cd130 | data | referenced by matched code (1/1 refs agree) | kDeltaTime |
| 0x146FAA370 | 0x146DDD240 | -0x1cd130 | data | 0x40 past an address matched code references | kFrameNumber |
| 0x146FCE850 | 0x146E01850 | -0x1cd000 | data | referenced by matched code (3/3 refs agree) (found by scan) | kSpawnMaxNum |
| 0x146FCE85C | 0x146E0185C | -0x1cd000 | data | referenced by matched code (2/2 refs agree) (found by scan) | kDistanceMax |
| 0x146FCE860 | 0x146E01860 | -0x1cd000 | data | referenced by matched code (2/2 refs agree) (found by scan) | kHardLimit |
| 0x1470C94C0 | 0x146EFC6E0 | -0x1ccde0 | data | referenced by matched code (2/2 refs agree) (found by scan) | kTagRegistryWeak |
| 0x1488EB8F0 | 0x14871EF70 | -0x1cc980 | data | referenced by matched code (2/2 refs agree) | kImmediateList |
| 0x1490FEB30 | 0x148F32308 | -0x1cc828 | data | referenced by matched code (1/1 refs agree) (found by scan) | kCutsceneManager |
| 0x1490FEDE8 | 0x148F325B8 | -0x1cc830 | data | referenced by matched code (24/24 refs agree) | kGameMode, kGameRoot |
| 0x1490FEE00 | 0x148F325D8 | -0x1cc828 | data | referenced by matched code (2/2 refs agree) | kClockFrames |
| 0x1490FEE08 | 0x148F325D0 | -0x1cc838 | data | referenced by matched code (2/2 refs agree) | kClockSeconds |
| 0x1490FF1C0 | 0x148F32990 | -0x1cc830 | data | referenced by matched code (3/3 refs agree) | kDirectInputOnlyFlag |
| 0x1490FF2B8 | 0x148F32AA0 | -0x1cc818 | data | referenced by matched code (4/4 refs agree) | kGameMode |
| 0x149104AE8 | 0x148F38538 | -0x1cc5b0 | data | referenced by matched code (1/1 refs agree) (found by scan) | kAllocatorStatic |
| 0x149115C28 | 0x148F49448 | -0x1cc7e0 | data | referenced by matched code (1/1 refs agree) (found by scan) | kTransientPackage |
| 0x14911F230 | 0x148F53900 | -0x1cb930 | data | referenced by matched code (1/1 refs agree) (found by scan) | kLeaderBlock |
| 0x1491A4B78 | 0x148FDCBF8 | -0x1c7f80 | data | referenced by matched code (4/4 refs agree) (found by scan) | kModuleGlobal |
| 0x1491D1948 | 0x1490099F8 | -0x1c7f50 | data | referenced by matched code (1/1 refs agree) | kRootOverlaySingleton |
| 0x1491DFB48 | 0x149017C18 | -0x1c7f30 | data | referenced by matched code (1/1 refs agree) | kEndCameraActorNameGlobal |
| 0x1491E3EE0 | 0x14901BFB8 | -0x1c7f28 | data | referenced by matched code (5/5 refs agree) (found by scan) | kTriggerTagName |
| 0x1491E44C0 | 0x14901C5A0 | -0x1c7f20 | data | referenced by matched code (6/6 refs agree) (found by scan) | kPauseMenuKey |
| 0x1491E44D8 | 0x14901C5B8 | -0x1c7f20 | data | referenced by matched code (10/10 refs agree) (found by scan) | - |
| 0x1491E44F0 | 0x14901C5D0 | -0x1c7f20 | data | referenced by matched code (3/3 refs agree) (found by scan) | kCommandMenuKey |
| 0x1491E4520 | 0x14901C600 | -0x1c7f20 | data | referenced by matched code (6/6 refs agree) (found by scan) | - |
| 0x1491E4538 | 0x14901C618 | -0x1c7f20 | data | referenced by matched code (1/1 refs agree) | kVehicleKeyName |
| 0x1491E4550 | 0x14901C630 | -0x1c7f20 | data | 0x18 past an address matched code references | kAccelKeyName |
| 0x1491E4580 | 0x14901C660 | -0x1c7f20 | data | referenced by matched code (3/3 refs agree) (found by scan) | kDashKeyName |
| 0x1491E4598 | 0x14901C678 | -0x1c7f20 | data | referenced by matched code (5/5 refs agree) (found by scan) | - |
| 0x1491E45E0 | 0x14901C6C0 | -0x1c7f20 | data | referenced by matched code (6/6 refs agree) (found by scan) | - |
| 0x1491E6D10 | 0x14901EDC0 | -0x1c7f50 | data | 0x18 past an address matched code references | kScooterKey |
| 0x149247570 | 0x14907F8F0 | -0x1c7c80 | data | referenced by matched code (3/3 refs agree) (found by scan) | kPlatformApp |
| 0x1493496E0 | 0x149186150 | -0x1c3590 | data | referenced by matched code (1/1 refs agree) (found by scan) | kEndUISystem |
| 0x1493496F0 | 0x149186160 | -0x1c3590 | data | referenced by matched code (1/1 refs agree) (found by scan) | kInputOwner, kSquareInputGlobal |
| 0x14939BC58 | 0x1491D8468 | -0x1c37f0 | data | referenced by matched code (1/1 refs agree) | kBattleStateGlobal, kBattleSystem |
| 0x14939BCA0 | 0x1491D84C0 | -0x1c37e0 | data | referenced by matched code (5/5 refs agree) | kLocationManager |
| 0x14939BCD8 | 0x1491D8508 | -0x1c37d0 | data | referenced by matched code (2/2 refs agree) | kNavStreamManagerGlobal |
| 0x14939BCE0 | 0x1491D8510 | -0x1c37d0 | data | referenced by matched code (11/11 refs agree) | kPartyManager, kPartyManagerGlobal |
| 0x14939BCE8 | 0x1491D8518 | -0x1c37d0 | data | referenced by matched code (1/1 refs agree) | kMoveTypeListener |
| 0x14939BD58 | 0x1491D8580 | -0x1c37d8 | data | referenced by matched code (3/3 refs agree) | kOptionHolderGlobal |

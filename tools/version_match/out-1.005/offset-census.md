# Offset-constant census (leads for the 1.005 layout audit)

1146 integer constants below 0x14000000. `moved` = reflected properties at that 1.004 offset whose 1.005 offset differs (any class). A lead, not proof.

| file:line | name | value | section / comment | reflected at offset (moved) | moved examples |
|---|---|---|---|---|---|
| mod.cpp:101 | kHalfShapeIntervalTicks | 0x1e | Design: Rebirth/Documentation/SPLITSCREE /  | 20 (0) |  |
| control/command_channel.cpp:27 | kScanTicks | 0xf |  /  | 19 (0) |  |
| control/runner.h:96 | kMaxCommandsPerTick | 0x20 | Pure logic with no engine access, tested /  | 810 (0) |  |
| control/runner.h:110 | kPlayers | 0x2 | Pure logic with no engine access, tested /  | 90 (0) |  |
| control/script.h:175 | kDefaultWaitUntilTicks | 0x258 | log-level: 0 error .. 4 trace /  | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| hud/action_icon_base.cpp:40 | kRowList | 0x208 | RE-p2-hud-view-levers.md §1 and RE-p2-fi / -> the icon list, a TArray<FWeakObjectPtr> | 55 (21) | SynthComponent.AttenuationOverrides -> 0x200; SQEXSEADWhizSeComponent.ApproachJudgmentDistance -> 0x200; CameraComponent.AspectRatio -> 0x200 |
| hud/action_icon_base.cpp:41 | kBasePos | 0x3c8 | RE-p2-hud-view-levers.md §1 and RE-p2-fi / UEndFieldActionIcon._BasePosY, an FVector2D | 207 (3) | EndEnvironmentPhysicsStaticMeshActor.PhysicsBehavior -> 0x3e8; EndBattleSummonTargetPoint.UnavailablePhase -> 0x3e8; EndNavModifierVolume.WallGroup -> 0x3e8 |
| hud/action_icon_base.cpp:42 | kViewportPosition | 0x220 | RE-p2-hud-view-levers.md §1 and RE-p2-fi / UUserWidget viewport offsets Left, Top (hud_addres | 46 (21) | PrimitiveComponent.CachedMaxDrawDistance -> 0x218; SQEX_Bonamik_Component_v2.SkeletalMeshComponent -> 0x218; SQEX_BonamikWind_Component_v2.m_BonamikWindAreaMargin -> 0x218 |
| hud/action_icon_base.cpp:43 | kMaxRows | 0x40 | RE-p2-hud-view-levers.md §1 and RE-p2-fi /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/area_icons_two.cpp:25 | kInstanceSize | 0x160 | Facts read from the database (RE-p2-hud- /  | 68 (0) |  |
| hud/area_icons_two.cpp:89 | kSetVisibilitySlot | 0x3b | SWidget::SetVisibility(widget, TAttribut /  | 12 (0) |  |
| hud/area_icons_two.cpp:95 | kEnabled | 0xd0 | and party markers), the icon and marker  / u8 | 205 (0) |  |
| hud/area_icons_two.cpp:96 | kFlagD1 | 0xd1 | and party markers), the icon and marker  / u8 | 7 (0) |  |
| hud/area_icons_two.cpp:97 | kSharedCount | 0x8 | and party markers), the icon and marker  / the weak pointer's reference controller: its share | 1705 (0) |  |
| hud/area_icons_two.cpp:100 | kVisibility | 0x1b0 | and party markers), the icon and marker  /  | 43 (0) |  |
| hud/area_icons_two.cpp:101 | kVisibilityDelegate | 0x1b8 | and party markers), the icon and marker  /  | 30 (0) |  |
| hud/area_icons_two.cpp:102 | kHidden | 0x2 | and party markers), the icon and marker  /  | 90 (0) |  |
| hud/area_icons_two.cpp:103 | kSelfHitTestInvisible | 0x11 | and party markers), the icon and marker  / what the canvases are constructed with | 36 (0) |  |
| hud/arrow_edge_code.h:55 | kArrowEdgeCodeSize | 0x6e | atan2 call before). Only rax, xmm3, xmm4 /  | 6 (0) |  |
| hud/centered_frame_code.h:51 | kCenteredFrameCodeSize | 0x25 | flags change; nothing else. No calls, no /  | 17 (0) |  |
| hud/enemy_gauges.cpp:36 | kManager | 0xf8 |  / the enemy-status manager (a pointer) | 149 (1) | NiagaraEmitter.Platforms -> 0xe8 |
| hud/enemy_gauges.cpp:37 | kGate | 0xc8 |  / u8: the manager updates its entries only when set | 232 (0) |  |
| hud/enemy_gauges.cpp:38 | kLockOnKey | 0x140 |  / player 1's targeted key, a TWeakPtr {object, contr | 102 (1) | NiagaraEmitter.RendererBindings -> 0x128 |
| hud/enemy_gauges.cpp:43 | kEntries | 0x10 | whose first field is the weak widget. /  | 1283 (0) |  |
| hud/enemy_gauges.cpp:44 | kElement | 0x180 | whose first field is the weak widget. /  | 52 (5) | SkeletalMesh.bUseFullPrecisionUVs -> 0x186; SkeletalMesh.bUseHighPrecisionTangentBasis -> 0x186; SkeletalMesh.bHasBeenSimplified -> 0x186 |
| hud/enemy_gauges.cpp:45 | kEntryP | 0x10 | whose first field is the weak widget. /  | 1283 (0) |  |
| hud/enemy_gauges.cpp:46 | kWeakCount | 0xc | whose first field is the weak widget. /  | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| hud/enemy_gauges.cpp:49 | kScreenPoint | 0xb8 | final step run. /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| hud/enemy_gauges.cpp:50 | kYOffset | 0x134 | final step run. /  | 43 (0) |  |
| hud/enemy_gauges.cpp:51 | kFinalFlags | 0x155 | final step run. / 3 bytes | 3 (0) |  |
| hud/enemy_gauges.cpp:54 | kOwnerSlot | 0x28 | B = 3 floats at c+0x1E0, max(min(B.x, B. /  | 889 (0) |  |
| hud/enemy_gauges.cpp:55 | kHalfSource | 0x298 | B = 3 floats at c+0x1E0, max(min(B.x, B. /  | 41 (1) | LightComponent.bUseIESBrightness -> 0x290 |
| hud/enemy_gauges.cpp:56 | kHalfBounds | 0x1e0 | B = 3 floats at c+0x1E0, max(min(B.x, B. /  | 48 (6) | NiagaraEmitter.bFixedBounds -> 0x1dc; NiagaraEmitter.bUseMinDetailLevel -> 0x1dc; NiagaraEmitter.bUseMaxDetailLevel -> 0x1dc |
| hud/enemy_gauges.cpp:57 | kHalfScaleXY | 0x4ac | B = 3 floats at c+0x1E0, max(min(B.x, B. /  | 7 (5) | PaperTerrainComponent.SegmentOverlapAmount -> 0x49c; CapsuleComponent.CapsuleRadius -> 0x49c; EndCylinderComponent.CylinderRadius -> 0x49c |
| hud/enemy_gauges.cpp:58 | kHalfScaleZ | 0x4a8 | B = 3 floats at c+0x1E0, max(min(B.x, B. /  | 25 (12) | ControlRigComponent.OnPreSetupDelegate -> 0x498; PaperTerrainComponent.RandomSeed -> 0x498; BoxComponent.BoxExtent -> 0x498 |
| hud/enemy_gauges.cpp:100 | kScale | 0x38 | (+0x98/+0x9C) and applies it (bar-target / p: the gauge's size state | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| hud/enemy_gauges.cpp:101 | kRenderScale | 0x98 | (+0x98/+0x9C) and applies it (bar-target / widget: render scale x, y | 306 (0) |  |
| hud/enemy_gauges.cpp:197 | kTargetStillFrames | 0x1e | Game thread. /  | 20 (0) |  |
| hud/enemy_gauges_code.h:37 | kGaugeRecordCapacity | 0x30 | installer so the host tests can execute  / the pool holds 16 gauges; each is placed once per  | 1052 (0) |  |
| hud/enemy_gauges_code.h:49 | kGaugeCodeSize | 0x43 | installer so the host tests can execute  /  | 5 (0) |  |
| hud/entry_filter_code.h:49 | kEntryBuffersCodeSize | 0x51 | installer so the host tests can execute  /  | 21 (0) |  |
| hud/entry_filter_code.h:50 | kEntryDrawGateCodeSize | 0x44 | installer so the host tests can execute  /  | 249 (0) |  |
| hud/field_markers.cpp:24 | kMarkerList | 0x98 |  /  | 306 (0) |  |
| hud/field_markers.cpp:25 | kMarkerEntry | 0x8c |  /  | 115 (0) |  |
| hud/first_player_code.h:24 | kFirstPlayerCodeSize | 0x18 | game thread; none of the function's othe /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| hud/hud_addresses.h:33 | kEntryMatrixScaleX | 0x50 | RebuildMatrix 0x140FC8CE8). The x/y term / row 0 x | 488 (0) |  |
| hud/hud_addresses.h:34 | kEntryMatrixScaleY | 0x64 | RebuildMatrix 0x140FC8CE8). The x/y term / row 1 y | 216 (1) | NiagaraEmitter.PreAllocationCount -> 0x58 |
| hud/hud_addresses.h:35 | kEntryMatrixTranslateX | 0x80 | RebuildMatrix 0x140FC8CE8). The x/y term / row 3 x | 393 (0) |  |
| hud/hud_addresses.h:36 | kEntryMatrixTranslateY | 0x84 | RebuildMatrix 0x140FC8CE8). The x/y term / row 3 y | 145 (0) |  |
| hud/hud_addresses.h:43 | kEntryMatrixData | 0xe0 | submitted (0x140FC87B1), so it is placed /  | 160 (1) | NiagaraEmitter.GlobalSpawnCountScaleOverrides -> 0xd4 |
| hud/hud_addresses.h:44 | kEntryMatrixCount | 0xe8 | submitted (0x140FC87B1), so it is placed /  | 156 (0) |  |
| hud/hud_addresses.h:45 | kEntryMatrixMaxCount | 0x1000 | submitted (0x140FC87B1), so it is placed / a sanity bound; the HUD pushes a handful | 2 (0) |  |
| hud/hud_addresses.h:71 | kOverlayViewWidth | 0x328 | helpers read (EndUI__ProjectWorldToRootO /  | 29 (1) | EndCinemaWidgetControlComponent.AnimName -> 0x320 |
| hud/hud_addresses.h:72 | kOverlayViewHeight | 0x32c | helpers read (EndUI__ProjectWorldToRootO /  | 14 (0) |  |
| hud/hud_addresses.h:73 | kOverlayHostScale | 0x330 | helpers read (EndUI__ProjectWorldToRootO /  | 21 (2) | EndFieldClimbingFinishPointSetting.bCachedEdgeTrans -> 0x320; EndNavMultiSwitchingVolume.NavPriorInfo -> 0x350 |
| hud/hud_addresses.h:78 | kUserWidgetViewportAnchors | 0x210 | go through the reflected Set*InViewport  / FAnchors, 16 bytes | 52 (24) | SQEX_Bonamik_Component_v2.bHasSkeletalMeshSkippedTransforms -> 0x208; SQEX_BonamikWind_Component_v2.m_BonamikWindBoxAreaHalfSize -> 0x208; AudioComponent.SoundClassOverride -> 0x208 |
| hud/hud_addresses.h:79 | kUserWidgetViewportOffsets | 0x220 | go through the reflected Set*InViewport  / FMargin, 16 bytes | 46 (21) | PrimitiveComponent.CachedMaxDrawDistance -> 0x218; SQEX_Bonamik_Component_v2.SkeletalMeshComponent -> 0x218; SQEX_BonamikWind_Component_v2.m_BonamikWindAreaMargin -> 0x218 |
| hud/hud_addresses.h:80 | kUserWidgetViewportAlignment | 0x230 | go through the reflected Set*InViewport  / FVector2D, 8 bytes | 160 (17) | PrimitiveComponent.bHasCustomNavigableGeometry -> 0x228; SQEX_BonamikWind_Component_v2.m_BonamikWind -> 0x228; AudioComponent.VolumeModulationMin -> 0x228 |
| hud/hud_addresses.h:81 | kUserWidgetViewportLayoutEnd | 0x238 | go through the reflected Set*InViewport  /  | 137 (10) | PrimitiveComponent.CustomDepthStencilValue -> 0x234; SQEX_Bonamik_Component_v2.m_BonamikEnable -> 0x230; AudioComponent.VolumeMultiplier -> 0x230 |
| hud/hud_placement.cpp:56 | kScanTicks | 0x1e |  /  | 20 (0) |  |
| hud/hud_placement.cpp:74 | kMaxStreak | 0x5 | would otherwise take turns every frame. /  | 30 (0) |  |
| hud/hud_placement.cpp:91 | kFrameBytes | 0x40 | would otherwise take turns every frame. / every setter used here needs far less | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/hud_players_dump.cpp:47 | kFrame | 0x40 |  / parameter frame; every function used here needs le | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/hud_roots.cpp:33 | kWidgetWorldWeak | 0x250 | (weak local player, weak world) and walk /  | 73 (7) | PrimitiveComponent.CustomPrimitiveDataInternal -> 0x248; AudioComponent.SourceEffectChain -> 0x248; EndFieldClimbingPointSettingBase.bCatchToHandSlide -> 0x248 |
| hud/hud_roots.cpp:48 | kCollapsed | 0x1 | ESlateVisibility: Collapsed and Hidden w /  | 155 (0) |  |
| hud/hud_roots.cpp:49 | kHidden | 0x2 | ESlateVisibility: Collapsed and Hidden w /  | 90 (0) |  |
| hud/hud_trace.cpp:71 | kFieldCommandCtrl | 0xb8 | pointers as {int32 index, int32 serial}  /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| hud/hud_trace.cpp:72 | kCompassManager | 0x108 | pointers as {int32 index, int32 serial}  /  | 127 (0) |  |
| hud/hud_trace.cpp:73 | kCompassHolder | 0x348 | pointers as {int32 index, int32 serial}  / in the compass manager; its +0 is the weak widget | 23 (0) |  |
| hud/hud_trace.cpp:74 | kFieldCommandSetupPending | 0x2d1 | pointers as {int32 index, int32 serial}  / EndUISystem byte | 3 (0) |  |
| hud/hud_trace.cpp:75 | kCtrlCommandMenuWidget | 0x4f0 | pointers as {int32 index, int32 serial}  / FieldCommandCtrl weak widgets and controller | 24 (6) | EndBonamikBoxScaleGroupParamsComponent.m_ChangeMassScale -> 0x4e0; EndBonamikRaycastWindComponent.WindDirection -> 0x4e0; EndDynamicBodyCollisionPrimitive.IgnoreActorNames -> 0x4e0 |
| hud/hud_trace.cpp:76 | kCtrlFieldMenuWidget | 0x4e8 | pointers as {int32 index, int32 serial}  /  | 34 (5) | ControlRigComponent.MappedElements -> 0x4d8; EndBonamikBoxScaleGroupParamsComponent.m_ChangeWindScale -> 0x4d8; EndDynamicBodyCollisionPrimitive.PushSpeed -> 0x4d8 |
| hud/hud_trace.cpp:77 | kCtrlStoredController | 0x558 | pointers as {int32 index, int32 serial}  /  | 18 (8) | PaperTileMapComponent.TileMap -> 0x548; SimpleDynamicMeshComponent.TangentsType -> 0x548; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyLift -> 0x548 |
| hud/hud_trace.cpp:78 | kCtrlSetupTrigger | 0x775 | pointers as {int32 index, int32 serial}  / FieldCommandCtrl byte | 0 (0) |  |
| hud/hud_trace.cpp:86 | kViewportLayerManager | 0x120 | SGameLayerManager::GetPlayerWidgetHostGe /  | 153 (0) |  |
| hud/hud_trace.cpp:87 | kViewportLayerManagerController | 0x128 | SGameLayerManager::GetPlayerWidgetHostGe /  | 106 (1) | NiagaraEmitter.ScalabilityOverrides -> 0x118 |
| hud/hud_trace.cpp:88 | kGeometryBytes | 0x38 | SGameLayerManager::GetPlayerWidgetHostGe / Size +0, Scale +8, translation +0x2C | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| hud/hud_trace.h:35 | kMaxHudTraceFrames | 0x1e | setup flags, and the stock per-player la /  | 20 (0) |  |
| hud/hud_updates.cpp:40 | kMarkerListMember | 0x150 | to its stack probe call is checked befor / the off-screen enemy arrows among them (hud/marker | 90 (0) |  |
| hud/hud_updates.cpp:54 | kLeaderTargetMember | 0x140 | w = FWeakObjectPtr::Get(m+0x6C) is set,  /  | 102 (1) | NiagaraEmitter.RendererBindings -> 0x128 |
| hud/hud_updates.cpp:55 | kLeaderTargetCount | 0x20 | w = FWeakObjectPtr::Get(m+0x6C) is set,  /  | 810 (0) |  |
| hud/hud_updates.cpp:56 | kLeaderTargetWeak | 0x6c | w = FWeakObjectPtr::Get(m+0x6C) is set,  /  | 189 (0) |  |
| hud/hud_updates.cpp:66 | kCompassManager | 0x108 | DeltaTime of 0 for each player [pass 12: /  | 127 (0) |  |
| hud/hud_updates.cpp:67 | kCompassHolder | 0x348 | DeltaTime of 0 for each player [pass 12: /  | 23 (0) |  |
| hud/label_relayouts.cpp:42 | kText | 0x2c8 |  / FString {TCHAR* data, int32 Num (with the null), i | 35 (3) | PlanarReflectionComponent.ExtraFOV -> 0x2c0; SceneCaptureComponent2D.CompositeMode -> 0x2c0; SceneCaptureComponentCube.TextureTargetRight -> 0x2c0 |
| hud/label_relayouts.cpp:43 | kTextNum | 0x2d0 |  /  | 46 (3) | EndCinemaWidgetControlComponent.LogicalOperator -> 0x2c8; PlanarReflectionComponent.DistanceFromPlaneFadeEnd -> 0x2c8; SceneCaptureComponentCube.TextureTargetODS -> 0x2c8 |
| hud/label_relayouts.cpp:44 | kWidth | 0x558 |  / the allotted size Tick stores (width, height) | 18 (8) | PaperTileMapComponent.TileMap -> 0x548; SimpleDynamicMeshComponent.TangentsType -> 0x548; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyLift -> 0x548 |
| hud/label_relayouts.cpp:45 | kWrapWidth | 0x560 |  / the width the layout was made for | 24 (10) | SplineComponent.bAllowSplineEditingPerInstance -> 0x550; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyDamping -> 0x550; EndFeelerAttendantActor.FollowSocketName -> 0x580 |
| hud/label_relayouts.cpp:46 | kLock | 0x564 |  / set: a relayout only marks itself pending | 8 (6) | SplineComponent.ReparamStepsPerSegment -> 0x554; EndBonamikBoxScaleGroupParamsComponent.m_GroupBodyDampingScale -> 0x554; EndFeelerBarrierActor.PrimaryVerticalLoopTime -> 0x584 |
| hud/label_relayouts.cpp:47 | kPending | 0x565 |  /  | 1 (0) |  |
| hud/label_relayouts.cpp:49 | kTextChars | 0x28 |  / kept per text | 889 (0) |  |
| hud/label_relayouts.cpp:50 | kCompareChars | 0x100 |  / compared per text (longer texts: their first 256) | 130 (0) |  |
| hud/label_relayouts.cpp:51 | kMaxBlocks | 0x100 |  / text blocks tracked per window | 130 (0) |  |
| hud/label_relayouts.cpp:52 | kListedBlocks | 0x8 |  / in the 5-second Debug line | 1705 (0) |  |
| hud/label_states.h:26 | kTextOffset | 0x2c8 | The swapped ranges of an SEndTextBlock ( / FString text | 35 (3) | PlanarReflectionComponent.ExtraFOV -> 0x2c0; SceneCaptureComponent2D.CompositeMode -> 0x2c0; SceneCaptureComponentCube.TextureTargetRight -> 0x2c0 |
| hud/label_states.h:27 | kTextSize | 0x10 | The swapped ranges of an SEndTextBlock ( /  | 1283 (0) |  |
| hud/label_states.h:28 | kRunsOffset | 0x530 | The swapped ranges of an SEndTextBlock ( / run arrays, desired size, last allotted size and w | 33 (23) | PaperFlipbookComponent.SpriteColor -> 0x520; SplineComponent.SplineScaleInfo -> 0x520; PaperTileMapComponent.TileLayers -> 0x520 |
| hud/label_states.h:29 | kRunsSize | 0x34 | The swapped ranges of an SEndTextBlock ( /  | 329 (0) |  |
| hud/label_states.h:30 | kFlagsOffset | 0x565 | The swapped ranges of an SEndTextBlock ( / relayout pending, glyph-font text, button text | 1 (0) |  |
| hud/label_states.h:31 | kFlagsSize | 0x3 | The swapped ranges of an SEndTextBlock ( /  | 42 (0) |  |
| hud/label_states.h:32 | kQuadsOffset | 0x570 | The swapped ranges of an SEndTextBlock ( / four quad arrays and the cached shadow offset | 20 (10) | SplineComponent.bDrawDebug -> 0x560; OctreeDynamicMeshComponent.bExplicitShowWireframe -> 0x560; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyVelocityDamping -> 0x560 |
| hud/label_states.h:33 | kQuadsSize | 0x48 | The swapped ranges of an SEndTextBlock ( /  | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| hud/label_states.h:34 | kShadowInQuads | 0x40 | The swapped ranges of an SEndTextBlock ( / +0x5B0/+0x5B4: 1.0f, 1.0f in an empty block | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/label_states.h:70 | kMaxLabels | 0x40 | The swapped ranges of an SEndTextBlock ( /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/layout_scale.cpp:26 | kReplaced | 0x8 | 0x140C95FC6  vmulss    xmm0, xmm7, [1/10 /  | 1705 (0) |  |
| hud/marker_timers.cpp:20 | kMarkerList | 0x150 | element 0x30 bytes: +0 the weak widget,  /  | 90 (0) |  |
| hud/marker_timers.cpp:21 | kElements | 0x10 | element 0x30 bytes: +0 the weak widget,  /  | 1283 (0) |  |
| hud/marker_timers.cpp:22 | kElement | 0x30 | element 0x30 bytes: +0 the weak widget,  /  | 1052 (0) |  |
| hud/marker_timers.cpp:23 | kTimer | 0x14 | element 0x30 bytes: +0 the weak widget,  /  | 478 (0) |  |
| hud/menu_buffers.h:36 | kMenuVertexCapacity | 0x400 | Game thread, except that the render thre / the same for every pool | 69 (2) | EndEnvironmentPhysicsStaticMeshActor.BoundsShapeLocation -> 0x420; EndNavWheelieBlockWallVolume.TriggerInterval -> 0x420 |
| hud/menu_buffers.h:37 | kMenuIndexCapacity | 0x600 | Game thread, except that the render thre /  | 7 (2) | EndFeelerBarrierActor.SkeletalMesh -> 0x620; EndFeelerSplineActor.CenterAnimatedOffsetCurve -> 0x620 |
| hud/minimap.cpp:49 | kAreaMapManager | 0x108 |  /  | 127 (0) |  |
| hud/minimap.cpp:50 | kNaviMapHolder | 0x340 |  /  | 23 (1) | EndFieldClimbingFinishPointSetting.CachedEdgeTransform -> 0x330 |
| hud/minimap.cpp:51 | kHolderInterface | 0x20 |  /  | 810 (0) |  |
| hud/minimap.cpp:52 | kInterfaceInObject | 0x108 |  /  | 127 (0) |  |
| hud/minimap.cpp:53 | kObjectSlateWidget | 0x138 |  /  | 100 (1) | NiagaraEmitter.bInterpolatedSpawning -> 0x1dc |
| hud/minimap.cpp:54 | kSlatePaintInterface | 0x2b0 |  /  | 62 (7) | PrimitiveComponent.MoveIgnoreActors -> 0x2a0; EndFieldClimbingFinishPointSetting.bFinishIsForceStepOn -> 0x2a0; EndFieldClimbingAttachPointSetting.bStartOrFinishPoint -> 0x2a0 |
| hud/minimap.cpp:55 | kSlateContent | 0x3b0 |  /  | 21 (4) | EndBattleSummonTargetPoint.bWarp_End2Hinachocobo -> 0x3d0; EndNavSwitchingVolume.bRefreshNav -> 0x3d0; EndNavModifierVolume.Wall -> 0x3d0 |
| hud/minimap.cpp:56 | kSlateTarget | 0x490 |  /  | 42 (27) | MeshComponent.OverrideMaterials -> 0x480; ControlRigComponent.ControlRigClass -> 0x480; SQEXSEADLayoutComponent.Sound -> 0x480 |
| hud/minimap.cpp:57 | kContentCentre | 0x478 |  / float x, y, z | 18 (1) | PrimitiveComponent.LODParentPrimitive -> 0x468 |
| hud/minimap.cpp:98 | kPoolZeroVertexBuffer | 0x80 | grow past them on the render thread). /  | 393 (0) |  |
| hud/minimap.cpp:99 | kPoolZeroIndexBuffer | 0x98 | grow past them on the render thread). /  | 306 (0) |  |
| hud/minimap.cpp:118 | kTargetFlags91 | 0x91 | grow past them on the render thread). / bit 0: SRGB | 22 (0) |  |
| hud/minimap.cpp:119 | kTargetGamma | 0xe0 | grow past them on the render thread). /  | 160 (1) | NiagaraEmitter.GlobalSpawnCountScaleOverrides -> 0xd4 |
| hud/minimap.cpp:120 | kTargetSizeX | 0xe8 | grow past them on the render thread). /  | 156 (0) |  |
| hud/minimap.cpp:121 | kTargetSizeY | 0xec | grow past them on the render thread). /  | 64 (0) |  |
| hud/minimap.cpp:122 | kTargetClearColor | 0xf0 | grow past them on the render thread). / FLinearColor | 135 (0) |  |
| hud/minimap.cpp:123 | kTargetFormat | 0x108 | grow past them on the render thread). /  | 127 (0) |  |
| hud/minimap.cpp:124 | kTargetResource | 0xa8 | grow past them on the render thread). /  | 239 (0) |  |
| hud/minimap.cpp:125 | kResourceTexture | 0x10 | grow past them on the render thread). / the texture RHI the HUD element samples | 1283 (0) |  |
| hud/minimap.cpp:126 | kResourceTarget | 0x90 | grow past them on the render thread). / the render-target RHI the consume binds | 332 (1) | NiagaraEmitter.SpawnScriptProps -> 0x88 |
| hud/minimap.cpp:127 | kFormatB8G8R8A8 | 0x2 | grow past them on the render thread). /  | 90 (0) |  |
| hud/minimap.cpp:128 | kMaxTargetSize | 0x1000 | grow past them on the render thread). /  | 2 (0) |  |
| hud/minimap_icons.cpp:50 | kHolderEntries | 0xe8 | EVisibility: bit 0 = painted). /  | 156 (0) |  |
| hud/minimap_icons.cpp:51 | kEntryElement | 0xa0 | EVisibility: bit 0 = painted). /  | 264 (0) |  |
| hud/minimap_icons.cpp:52 | kEntry | 0x8 | EVisibility: bit 0 = painted). /  | 1705 (0) |  |
| hud/minimap_icons.cpp:53 | kEntryIcon | 0x8 | EVisibility: bit 0 = painted). /  | 1705 (0) |  |
| hud/minimap_icons.cpp:54 | kEntryKind | 0x1c | EVisibility: bit 0 = painted). /  | 361 (0) |  |
| hud/minimap_icons.cpp:55 | kIconVisibility | 0x1b0 | EVisibility: bit 0 = painted). /  | 43 (0) |  |
| hud/minimap_icons.cpp:56 | kPainted | 0x1 | EVisibility: bit 0 = painted). /  | 155 (0) |  |
| hud/minimap_icons.cpp:59 | kMaxKept | 0x1000 | EVisibility: bit 0 = painted). / per run; far above any icon count | 2 (0) |  |
| hud/minimap_icons.cpp:60 | kMaxReported | 0x40 | EVisibility: bit 0 = painted). / shown-anyway keys reported once each | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/minimap_labels.cpp:70 | kInvalidateLayout | 0x5 | The text block's destructor frees the ra / what the game's SetText passes after a relayout | 30 (0) |  |
| hud/minimap_labels.cpp:78 | kControllerBefore | 0x10 | The text block's destructor frees the ra /  | 1283 (0) |  |
| hud/minimap_labels.cpp:79 | kWeakThisObject | 0x8 | The text block's destructor frees the ra /  | 1705 (0) |  |
| hud/minimap_labels.cpp:80 | kWeakThisCounter | 0x10 | The text block's destructor frees the ra /  | 1283 (0) |  |
| hud/minimap_labels.cpp:81 | kSharedCount | 0x8 | The text block's destructor frees the ra /  | 1705 (0) |  |
| hud/minimap_labels.cpp:82 | kWeakCount | 0xc | The text block's destructor frees the ra /  | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| hud/minimap_labels.cpp:83 | kPaintInterface | 0x2b0 | The text block's destructor frees the ra /  | 62 (7) | PrimitiveComponent.MoveIgnoreActors -> 0x2a0; EndFieldClimbingFinishPointSetting.bFinishIsForceStepOn -> 0x2a0; EndFieldClimbingAttachPointSetting.bStartOrFinishPoint -> 0x2a0 |
| hud/minimap_labels.cpp:84 | kGcObject | 0x2b8 | The text block's destructor frees the ra /  | 38 (5) | EndCinemaWidgetControlComponent.StoryFlagName -> 0x2b0; LightComponent.BloomTint -> 0x2b0; PlanarReflectionComponent.NormalDistortionStrength -> 0x2b0 |
| hud/minimap_labels.cpp:85 | kGetterBound | 0x2e8 | The text block's destructor frees the ra / the text attribute's getter (0: unbound) | 34 (1) | EndStreamingVolume.VolumeLabel -> 0x2f8 |
| hud/minimap_labels.cpp:86 | kRelayoutLock | 0x564 | The text block's destructor frees the ra /  | 8 (6) | SplineComponent.ReparamStepsPerSegment -> 0x554; EndBonamikBoxScaleGroupParamsComponent.m_GroupBodyDampingScale -> 0x554; EndFeelerBarrierActor.PrimaryVerticalLoopTime -> 0x584 |
| hud/minimap_labels.cpp:87 | kLanguageStamp | 0x568 | The text block's destructor frees the ra / FName copied from a global (the language, §7.12) | 23 (9) | ProceduralMeshComponent.AsyncBodySetupQueue -> 0x558; SplineComponent.Duration -> 0x558; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyConstraintDamping -> 0x558 |
| hud/minimap_labels.cpp:88 | kFont | 0x2f0 | The text block's destructor frees the ra / the main and button font UObjects | 28 (3) | EndFieldClimbingFinishPointSetting.bEdgeSearchFromForceBeforePointDir -> 0x2e0; EndFieldClimbingAttachPointSetting.ForcEnableCatchNameArray -> 0x2e0; EndStreamingVolume.ActivateProgress -> 0x300 |
| hud/minimap_labels.cpp:89 | kButtonFont | 0x2f8 | The text block's destructor frees the ra /  | 25 (1) | EndStreamingVolume.DeactivateProgress -> 0x308 |
| hud/minimap_labels.cpp:90 | kFastPathProxy | 0x18 | The text block's destructor frees the ra / logged once (an invalidation root above the label) | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| hud/minimap_once.cpp:55 | kPoolZeroSlots | 0x28 | FEndMenuRenderer pool 0 (the render-to-t / FEndMenuLayerSlot* (40 bytes each) | 889 (0) |  |
| hud/minimap_once.cpp:56 | kPoolZeroSlotCount | 0x30 | FEndMenuRenderer pool 0 (the render-to-t / int32 | 1052 (0) |  |
| hud/minimap_once.cpp:57 | kPoolZeroWrite | 0x38 | FEndMenuRenderer pool 0 (the render-to-t / uint8 | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| hud/minimap_once.cpp:58 | kPoolZeroRead | 0x39 | FEndMenuRenderer pool 0 (the render-to-t / uint8 | 39 (0) |  |
| hud/minimap_once.cpp:59 | kSlotSize | 0x28 | FEndMenuRenderer pool 0 (the render-to-t /  | 889 (0) |  |
| hud/minimap_once.cpp:60 | kSlotActive | 0x0 | FEndMenuRenderer pool 0 (the render-to-t / TArray<TSharedPtr<entry>>: data, +8 Num | 2161 (0) |  |
| hud/minimap_once.cpp:61 | kSlotRecycled | 0x10 | FEndMenuRenderer pool 0 (the render-to-t /  | 1283 (0) |  |
| hud/minimap_once.cpp:62 | kSlotState | 0x20 | FEndMenuRenderer pool 0 (the render-to-t / 0 initial, 1 acquired, 2 submitted, 3 consuming, 4 | 810 (0) |  |
| hud/minimap_once.cpp:63 | kSlotFrame | 0x24 | FEndMenuRenderer pool 0 (the render-to-t / GFrameNumber at submit | 305 (0) |  |
| hud/minimap_once.cpp:64 | kEntryBatchData | 0x90 | FEndMenuRenderer pool 0 (the render-to-t /  | 332 (1) | NiagaraEmitter.SpawnScriptProps -> 0x88 |
| hud/minimap_once.cpp:65 | kBatchVertexFree | 0x8 | FEndMenuRenderer pool 0 (the render-to-t / VertexArrayFreeList Num | 1705 (0) |  |
| hud/minimap_once.cpp:66 | kBatchIndexFree | 0x18 | FEndMenuRenderer pool 0 (the render-to-t /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| hud/minimap_once.cpp:67 | kBatchVertexArrays | 0x28 | FEndMenuRenderer pool 0 (the render-to-t / BatchVertexArrays Num | 889 (0) |  |
| hud/minimap_once.cpp:68 | kBatchIndexArrays | 0x38 | FEndMenuRenderer pool 0 (the render-to-t /  | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| hud/minimap_once_code.h:41 | kCircleSkipCodeSize | 0x33 | next call (its arguments are rcx and rdx /  | 8 (0) |  |
| hud/minimap_once_code.h:42 | kOctagonSkipCodeSize | 0x3e | next call (its arguments are rcx and rdx /  | 10 (0) |  |
| hud/navimap_watch.cpp:23 | kAreaMapManager | 0x108 | RE-p2-own-minimap.md (this build). /  | 127 (0) |  |
| hud/navimap_watch.cpp:24 | kNaviMapHolder | 0x340 | RE-p2-own-minimap.md (this build). /  | 23 (1) | EndFieldClimbingFinishPointSetting.CachedEdgeTransform -> 0x330 |
| hud/navimap_watch.cpp:25 | kHolderInterface | 0x20 | RE-p2-own-minimap.md (this build). / the widget's native interface (weak pointer follow | 810 (0) |  |
| hud/navimap_watch.cpp:26 | kHolderAutoRotate | 0x178 | RE-p2-own-minimap.md (this build). / u8: rotation from the camera (else +0x198) | 44 (0) |  |
| hud/navimap_watch.cpp:27 | kHolderFlag179 | 0x179 | RE-p2-own-minimap.md (this build). / u8: gates the +0xC8/+0xD0 calls | 1 (0) |  |
| hud/navimap_watch.cpp:28 | kHolderYawOffset | 0x194 | RE-p2-own-minimap.md (this build). / float, added to the camera yaw | 13 (0) |  |
| hud/navimap_watch.cpp:29 | kHolderFixedRotation | 0x198 | RE-p2-own-minimap.md (this build). / float | 28 (0) |  |
| hud/navimap_watch.cpp:30 | kInterfaceInObject | 0x108 | RE-p2-own-minimap.md (this build). / UEndNaviMapCircle/Octagon: interface at +0x108 | 127 (0) |  |
| hud/navimap_watch.cpp:31 | kObjectRenderTexture | 0x128 | RE-p2-own-minimap.md (this build). / UEndNaviMapCircle.RenderTexture (a Blueprint asset | 106 (1) | NiagaraEmitter.ScalabilityOverrides -> 0x118 |
| hud/navimap_watch.cpp:32 | kObjectSlateWidget | 0x138 | RE-p2-own-minimap.md (this build). / TSharedPtr<SEndNaviMapCircle>: object pointer firs | 100 (1) | NiagaraEmitter.bInterpolatedSpawning -> 0x1dc |
| hud/navimap_watch.cpp:33 | kSlatePaintInterface | 0x2b0 | RE-p2-own-minimap.md (this build). / its vtable says circle or octagon | 62 (7) | PrimitiveComponent.MoveIgnoreActors -> 0x2a0; EndFieldClimbingFinishPointSetting.bFinishIsForceStepOn -> 0x2a0; EndFieldClimbingAttachPointSetting.bStartOrFinishPoint -> 0x2a0 |
| hud/navimap_watch.cpp:34 | kSlateContent | 0x3b0 | RE-p2-own-minimap.md (this build). / SEndNaviMap* | 21 (4) | EndBattleSummonTargetPoint.bWarp_End2Hinachocobo -> 0x3d0; EndNavSwitchingVolume.bRefreshNav -> 0x3d0; EndNavModifierVolume.Wall -> 0x3d0 |
| hud/navimap_watch.cpp:35 | kSlateTarget | 0x490 | RE-p2-own-minimap.md (this build). / UTextureRenderTarget2D* | 42 (27) | MeshComponent.OverrideMaterials -> 0x480; ControlRigComponent.ControlRigClass -> 0x480; SQEXSEADLayoutComponent.Sound -> 0x480 |
| hud/navimap_watch.cpp:39 | kContentCentre | 0x478 | SEndNaviMap: / float x, y, z | 18 (1) | PrimitiveComponent.LODParentPrimitive -> 0x468 |
| hud/navimap_watch.cpp:40 | kContentPixelPerCm | 0x484 | SEndNaviMap: /  | 8 (0) |  |
| hud/navimap_watch.cpp:41 | kContentMapRotation | 0x488 | SEndNaviMap: / radians | 17 (1) | EndCrashBoxMiniGameElectricCableActor.m_CableType -> 0x4a8 |
| hud/navimap_watch.cpp:42 | kContentViewRotation | 0x48c | SEndNaviMap: / radians | 6 (1) | EndCrashBoxMiniGameElectricCableActor.m_GroupIndex -> 0x4ac |
| hud/navimap_watch.cpp:44 | kTargetFlags91 | 0x91 | UTextureRenderTarget2D (stock 4.26) and  /  | 22 (0) |  |
| hud/navimap_watch.cpp:45 | kTargetGamma | 0xe0 | UTextureRenderTarget2D (stock 4.26) and  /  | 160 (1) | NiagaraEmitter.GlobalSpawnCountScaleOverrides -> 0xd4 |
| hud/navimap_watch.cpp:46 | kTargetSizeX | 0xe8 | UTextureRenderTarget2D (stock 4.26) and  /  | 156 (0) |  |
| hud/navimap_watch.cpp:47 | kTargetSizeY | 0xec | UTextureRenderTarget2D (stock 4.26) and  /  | 64 (0) |  |
| hud/navimap_watch.cpp:48 | kTargetFormat | 0x108 | UTextureRenderTarget2D (stock 4.26) and  /  | 127 (0) |  |
| hud/navimap_watch.cpp:49 | kTargetResource | 0xa8 | UTextureRenderTarget2D (stock 4.26) and  /  | 239 (0) |  |
| hud/navimap_watch.cpp:50 | kResourceTexture | 0x10 | UTextureRenderTarget2D (stock 4.26) and  / the texture RHI the outer element samples | 1283 (0) |  |
| hud/navimap_watch.cpp:51 | kResourceTarget | 0x90 | UTextureRenderTarget2D (stock 4.26) and  / the render-target RHI pool 0's consume binds | 332 (1) | NiagaraEmitter.SpawnScriptProps -> 0x88 |
| hud/navimap_watch.cpp:53 | kObjectName | 0x18 | UObjectBase: /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| hud/navimap_watch.cpp:54 | kObjectOuter | 0x20 | UObjectBase: /  | 810 (0) |  |
| hud/navimap_watch.cpp:56 | kHeartbeatMs | 0xea60 | UObjectBase: /  | 0 (0) |  |
| hud/player_two_pass.cpp:41 | kGameModeHudTimeStep | 0x5d8 | Facts read from the database (RE-p2-hud. / float, written once per engine tick | 21 (10) | EndFeelerAttendantActor.WalkMoveVelocityOutputRange -> 0x5f8; EndFeelerBarrierActor.DeactiveUpSpeedRange -> 0x5f8; EndFeelerConflictActor.CenterAnimatedOffsetCurve -> 0x5f8 |
| hud/player_two_pass.cpp:50 | kAbilityNames | 0x180 | 72-byte elements, the label's weak widge /  | 52 (5) | SkeletalMesh.bUseFullPrecisionUVs -> 0x186; SkeletalMesh.bUseHighPrecisionTangentBasis -> 0x186; SkeletalMesh.bHasBeenSimplified -> 0x186 |
| hud/player_two_pass.cpp:51 | kAbilityNameEntry | 0x48 | 72-byte elements, the label's weak widge /  | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| hud/player_two_pass.cpp:52 | kAbilityNameWidget | 0x8 | 72-byte elements, the label's weak widge /  | 1705 (0) |  |
| hud/player_two_pass.cpp:55 | kFieldTargets | 0x98 | 72-byte elements, the label's weak widge /  | 306 (0) |  |
| hud/player_two_pass.cpp:56 | kFieldTargetEntry | 0x8c | 72-byte elements, the label's weak widge /  | 115 (0) |  |
| hud/player_two_pass.cpp:63 | kRootScanFrames | 0x1e | New roots (markers the game adds) get pl /  | 20 (0) |  |
| hud/player_two_pass.cpp:64 | kTimingFrames | 0xf0 | New roots (markers the game adds) get pl /  | 135 (0) |  |
| hud/popups.cpp:27 | kManager | 0xa0 | Facts read from the database (RE-p2-batt / the popup manager (a pointer) | 264 (0) |  |
| hud/popups.cpp:28 | kCanvas | 0x10 | Facts read from the database (RE-p2-batt / TSharedPtr<SEndPopupMessageCanvas> {object, contro | 1283 (0) |  |
| hud/popups.cpp:29 | kSharedCount | 0x8 | Facts read from the database (RE-p2-batt / a reference controller's shared count (int32) | 1705 (0) |  |
| hud/popups_code.h:39 | kPopupRecordCapacity | 0x60 | [rsp+30h] FVector2D offset, [rsp+38h] u8 / a busy fight shows a few dozen popups | 484 (1) | NiagaraEmitter.AllocationMode -> 0x53 |
| hud/popups_code.h:53 | kPopupCodeSize | 0x5d | [rsp+30h] FVector2D offset, [rsp+38h] u8 /  | 7 (0) |  |
| hud/position_probe.cpp:31 | kViewportAnchors | 0x210 | alignment +0x230; read, never written). / FAnchors: Min, Max | 52 (24) | SQEX_Bonamik_Component_v2.bHasSkeletalMeshSkippedTransforms -> 0x208; SQEX_BonamikWind_Component_v2.m_BonamikWindBoxAreaHalfSize -> 0x208; AudioComponent.SoundClassOverride -> 0x208 |
| hud/position_probe.cpp:32 | kViewportOffsets | 0x220 | alignment +0x230; read, never written). / FMargin: Left, Top, Right, Bottom | 46 (21) | PrimitiveComponent.CachedMaxDrawDistance -> 0x218; SQEX_Bonamik_Component_v2.SkeletalMeshComponent -> 0x218; SQEX_BonamikWind_Component_v2.m_BonamikWindAreaMargin -> 0x218 |
| hud/position_probe.cpp:33 | kViewportAlignment | 0x230 | alignment +0x230; read, never written). / FVector2D | 160 (17) | PrimitiveComponent.bHasCustomNavigableGeometry -> 0x228; SQEX_BonamikWind_Component_v2.m_BonamikWind -> 0x228; AudioComponent.VolumeModulationMin -> 0x228 |
| hud/prompt_rows.cpp:41 | kRowList | 0x208 | RE-p2-field-prompts.md §1.2 and §4 (this / -> {FWeakObjectPtr* data, int32 num} | 55 (21) | SynthComponent.AttenuationOverrides -> 0x200; SQEXSEADWhizSeComponent.ApproachJudgmentDistance -> 0x200; CameraComponent.AspectRatio -> 0x200 |
| hud/prompt_rows.cpp:42 | kListNum | 0x8 | RE-p2-field-prompts.md §1.2 and §4 (this /  | 1705 (0) |  |
| hud/prompt_rows.cpp:43 | kLeaderActions | 0x598 | RE-p2-field-prompts.md §1.2 and §4 (this / PC -> the leader-action component | 25 (13) | EndDolphinJumpSplineComponent.Phases -> 0x588; EndFeelerAttendantActor.WalkTransitionDeltaSizeThreshold -> 0x5b8; EndFeelerConflictActor.PrimaryRollSize -> 0x5b8 |
| hud/prompt_rows.cpp:44 | kComponentRowBytes | 0x9a8 | RE-p2-field-prompts.md §1.2 and §4 (this / + 12*i: request byte | 5 (0) |  |
| hud/prompt_rows.cpp:45 | kComponentRowWeak | 0x9ac | RE-p2-field-prompts.md §1.2 and §4 (this / + 12*i: the row's weak widget | 2 (0) |  |
| hud/prompt_rows.cpp:46 | kRowStride | 0xc | RE-p2-field-prompts.md §1.2 and §4 (this /  | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| hud/prompt_rows.cpp:47 | kComponentRows | 0xe | RE-p2-field-prompts.md §1.2 and §4 (this /  | 29 (0) |  |
| hud/prompt_rows.cpp:48 | kWidgetPriority | 0x400 | RE-p2-field-prompts.md §1.2 and §4 (this /  | 69 (2) | EndEnvironmentPhysicsStaticMeshActor.BoundsShapeLocation -> 0x420; EndNavWheelieBlockWallVolume.TriggerInterval -> 0x420 |
| hud/prompt_rows.cpp:49 | kWidgetRemoveState | 0x418 | RE-p2-field-prompts.md §1.2 and §4 (this /  | 57 (2) | EndEnvironmentPhysicsStaticMeshActor.bSteeringObstacle -> 0x438; EndDialogueGenerateSourceShot.Light -> 0x428 |
| hud/prompt_rows.cpp:50 | kWidgetViewportPosition | 0x220 | RE-p2-field-prompts.md §1.2 and §4 (this / UUserWidget viewport offsets (hud/hud_addresses.h) | 46 (21) | PrimitiveComponent.CachedMaxDrawDistance -> 0x218; SQEX_Bonamik_Component_v2.SkeletalMeshComponent -> 0x218; SQEX_BonamikWind_Component_v2.m_BonamikWindAreaMargin -> 0x218 |
| hud/prompt_rows.cpp:51 | kMaxRows | 0x40 | RE-p2-field-prompts.md §1.2 and §4 (this /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/prompt_rows.cpp:63 | kCollapsed | 0x1 | ESlateVisibility. /  | 155 (0) |  |
| hud/prompt_rows.cpp:64 | kHidden | 0x2 | ESlateVisibility. /  | 90 (0) |  |
| hud/prompt_rows.cpp:109 | kRowStillFrames | 0x1e | Game thread. /  | 20 (0) |  |
| hud/second_image.cpp:48 | kOverlayWindow | 0x2f0 | window (+0x2F0), before writing anything /  | 28 (3) | EndFieldClimbingFinishPointSetting.bEdgeSearchFromForceBeforePointDir -> 0x2e0; EndFieldClimbingAttachPointSetting.ForcEnableCatchNameArray -> 0x2e0; EndStreamingVolume.ActivateProgress -> 0x300 |
| hud/second_image.cpp:65 | kViewUnscaledViewRect | 0x238 | &Views, texture, context), once per view / FIntRect in FSceneView (FViewInfo is a copy) | 137 (10) | PrimitiveComponent.CustomDepthStencilValue -> 0x234; SQEX_Bonamik_Component_v2.m_BonamikEnable -> 0x230; AudioComponent.VolumeMultiplier -> 0x230 |
| hud/second_image.cpp:76 | kPoolTwoVertexBuffer | 0x90 | BeginInitResource_OrDirectIfRenderThread /  | 332 (1) | NiagaraEmitter.SpawnScriptProps -> 0x88 |
| hud/second_image.cpp:77 | kPoolTwoIndexBuffer | 0xa8 | BeginInitResource_OrDirectIfRenderThread /  | 239 (0) |  |
| hud/second_image.cpp:93 | kPoolTwo | 0x2 | BeginInitResource_OrDirectIfRenderThread / the root overlay's pool (the dl argument at 0x140C | 90 (0) |  |
| hud/second_image.cpp:134 | kRenderTimingFrames | 0xf0 | Render cost, logged every kRenderTimingF /  | 135 (0) |  |
| hud/target_views.cpp:49 | kTargetedKey | 0x140 | its one call, in the ability-name tick;  / the bar manager's targeted key, a TWeakPtr {object | 102 (1) | NiagaraEmitter.RendererBindings -> 0x128 |
| hud/target_views.cpp:57 | kCursor | 0x68 | The battle system's displayed target and /  | 614 (1) | NiagaraEmitter.UpdateScriptProps -> 0x60 |
| hud/target_views.cpp:58 | kDisplayed | 0x70 | The battle system's displayed target and /  | 515 (0) |  |
| hud/target_views.cpp:63 | kManager | 0xf8 | the weak widget) and the sticky list (+0 /  | 149 (1) | NiagaraEmitter.Platforms -> 0xe8 |
| hud/target_views.cpp:64 | kEntries | 0x10 | the weak widget) and the sticky list (+0 /  | 1283 (0) |  |
| hud/target_views.cpp:65 | kElement | 0x180 | the weak widget) and the sticky list (+0 /  | 52 (5) | SkeletalMesh.bUseFullPrecisionUVs -> 0x186; SkeletalMesh.bUseHighPrecisionTangentBasis -> 0x186; SkeletalMesh.bHasBeenSimplified -> 0x186 |
| hud/target_views.cpp:66 | kEntryWidget | 0x10 | the weak widget) and the sticky list (+0 /  | 1283 (0) |  |
| hud/target_views.cpp:67 | kSticky | 0xb0 | the weak widget) and the sticky list (+0 /  | 301 (0) |  |
| hud/target_views.cpp:68 | kStickyCount | 0xb8 | the weak widget) and the sticky list (+0 /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| hud/target_views.cpp:69 | kMaxSticky | 0x40 | the weak widget) and the sticky list (+0 /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| hud/target_views.cpp:71 | kOwnerSlot | 0x28 | the weak widget) and the sticky list (+0 /  | 889 (0) |  |
| hud/target_views.cpp:72 | kShowStatus | 0x773 | the weak widget) and the sticky list (+0 /  | 0 (0) |  |
| hud/target_views.cpp:73 | kSharedCount | 0x8 | the weak widget) and the sticky list (+0 /  | 1705 (0) |  |
| hud/widget_dump.cpp:39 | kFrame | 0x100 |  / parameter frame; every function used here needs fa | 130 (0) |  |
| hud/widget_dump.cpp:87 | kAnchorDataSize | 0x28 |  / FAnchorData: FMargin Offsets, FAnchors Anchors, FV | 889 (0) |  |
| hud/widget_opacity.cpp:17 | kRenderOpacity | 0xc4 | Facts read from the database (RE-p2-hud- / UWidget: float | 83 (0) |  |
| platform/binned_pools.cpp:16 | kPoolTables | 0x28 | Facts read from the database (RE-oom-pat / FPoolTable[74] | 889 (0) |  |
| platform/binned_pools.cpp:17 | kPoolStride | 0x60 | Facts read from the database (RE-oom-pat /  | 484 (1) | NiagaraEmitter.AllocationMode -> 0x53 |
| platform/binned_pools.cpp:19 | kBlockSize | 0x0 | FPoolTable / u32 | 2161 (0) |  |
| platform/binned_pools.cpp:20 | kPerBlock | 0x4 | FPoolTable / u16 blocks per block-of-blocks | 542 (0) |  |
| platform/binned_pools.cpp:21 | kAllocBits | 0x8 | FPoolTable / FBitTree: bit 1 = block-of-blocks committed | 1705 (0) |  |
| platform/binned_pools.cpp:22 | kEverUsed | 0x48 | FPoolTable / u32, highest block-of-blocks ever used + 1 | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| platform/binned_pools.cpp:23 | kPoolInfos | 0x50 | FPoolTable / u32**: page table; only page 0 is used (<= 8192 en | 488 (0) |  |
| platform/binned_pools.cpp:25 | kBits | 0x0 | FBitTree / u64* | 2161 (0) |  |
| platform/binned_pools.cpp:26 | kDesired | 0xc | FBitTree / u32, the pool's capacity in blocks-of-blocks | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| platform/binned_pools.cpp:27 | kLastRow | 0x14 | FBitTree / u32, in qwords | 478 (0) |  |
| platform/binned_pools.cpp:28 | kAllocSize | 0x18 | FBitTree / u32, bytes | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| platform/binned_pools.h:25 | kPoolCount | 0x4a | moved; pool memory itself is never read) /  | 7 (0) |  |
| platform/binned_pools.h:26 | kPool32K | 0x31 | moved; pool memory itself is never read) / serves 28673-32768 bytes | 55 (0) |  |
| platform/binned_pools.h:27 | kBlock32K | 0x8000 | moved; pool memory itself is never read) /  | 0 (0) |  |
| platform/binned_pools.h:28 | kMaxRowWords | 0x80 | moved; pool memory itself is never read) / 8192 blocks-of-blocks at most, one bit each | 393 (0) |  |
| platform/call_redirect.cpp:15 | kCallOpcode | 0xe8 |  /  | 156 (0) |  |
| platform/call_redirect.cpp:16 | kJumpOpcode | 0xe9 |  / jmp rel32: same length and displacement rule as th | 10 (0) |  |
| platform/call_redirect.cpp:17 | kCallLength | 0x5 |  /  | 30 (0) |  |
| platform/engine_addresses.h:106 | kRemoveLocalPlayerVTableOffset | 0x2e8 | Removing a local player (RE-remove-local /  | 34 (1) | EndStreamingVolume.VolumeLabel -> 0x2f8 |
| platform/engine_addresses.h:123 | kLocalPlayerControllerOffset | 0x30 | Removing a local player (RE-remove-local /  | 1052 (0) |  |
| platform/engine_addresses.h:135 | kTwoPlayerSplitscreenLayoutOffset | 0x51 | The split layout and each half's field o /  | 21 (0) |  |
| platform/engine_addresses.h:139 | kAspectRatioAxisConstraintOffset | 0x94 | The split layout and each half's field o /  | 91 (0) |  |
| platform/entry_redirect.cpp:14 | kTrampolineStart | 0x10 |  / after the thunk's 14 bytes | 1283 (0) |  |
| platform/frame_watch.cpp:28 | kMaxNotes | 0x6 |  /  | 23 (0) |  |
| platform/frame_watch.cpp:71 | kMaxJobs | 0xc | Game thread, except the log-line counter /  | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| platform/memory_watch.cpp:37 | kPoolStepPercent | 0x2 |  / a 32 KB pool change this large (of its 512 MB) is  | 90 (0) |  |
| platform/near_code.cpp:14 | kAllocationGrain | 0x10000 |  / VirtualAlloc reserves on 64 KiB boundaries | 0 (0) |  |
| platform/near_code.h:14 | kCodeBlockSize | 0x10 | code sees either the old bytes or the ne /  | 1283 (0) |  |
| platform/near_code.h:15 | kCodePageSize | 0x1000 | code sees either the old bytes or the ne /  | 2 (0) |  |
| platform/sparse_array.h:20 | kSparseData | 0x0 | header visits nothing. Game thread only  /  | 2161 (0) |  |
| platform/sparse_array.h:21 | kSparseInlineBits | 0x10 | header visits nothing. Game thread only  /  | 1283 (0) |  |
| platform/sparse_array.h:22 | kSparseHeapBits | 0x20 | header visits nothing. Game thread only  /  | 810 (0) |  |
| platform/sparse_array.h:23 | kSparseNumBits | 0x28 | header visits nothing. Game thread only  /  | 889 (0) |  |
| platform/sparse_array.h:24 | kSparseMaxBits | 0x1000 | header visits nothing. Game thread only  / far above any HUD set; guards a bad read | 2 (0) |  |
| platform/x64_jump.h:14 | kJumpLength | 0x5 | 32-bit displacement measured from the en /  | 30 (0) |  |
| players/local_players.cpp:9 | kPlayerTwoControllerId | 0x1 |  /  | 155 (0) |  |
| players/split_screen_settings.h:48 | kGrowToLargestRequest | 0x2 | tested on the host. /  | 90 (0) |  |
| players/unreal_access.cpp:137 | kComponentWorldLocation | 0x1d0 | `kept` is read (its class) only after it / USceneComponent's world transform translation | 33 (0) |  |
| players/unreal_access.cpp:316 | kMaxFrame | 0x40 | `kept` is read (its class) only after it /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| players/unreal_access.cpp:408 | kMaxFrame | 0x10 | `kept` is read (its class) only after it /  | 1283 (0) |  |
| players/unreal_access.cpp:428 | kMaxFrame | 0x10 | `kept` is read (its class) only after it /  | 1283 (0) |  |
| player_two/battle_camera.cpp:30 | kBattleOperator | 0x2ca8 | Facts read from the database (RE-p2-batt / operator 11 | 0 (0) |  |
| player_two/battle_camera.cpp:31 | kOperatorState | 0xd0 | Facts read from the database (RE-p2-batt / u8 (the Field operator's active byte is at the sam | 205 (0) |  |
| player_two/battle_camera.cpp:32 | kOperatorActive | 0xd1 | Facts read from the database (RE-p2-batt / u8: the update picks the lowest-numbered active op | 7 (0) |  |
| player_two/battle_camera.cpp:33 | kOperatorBytes | 0x2cd0 | Facts read from the database (RE-p2-batt / current / previous operator bytes (+0x2CD0..+0x2CD | 0 (0) |  |
| player_two/battle_camera.cpp:47 | kModules | 0x6e0 | look-at, skill, command, leader change,  /  | 9 (3) | EndFeelerSplineActor.MasterSkeletalMeshComponents -> 0x700; EndFeelerTornadeActor.MasterSkeletalMeshComponents -> 0x700; ParticleSystemComponent.WarmupTickRate -> 0x6d0 |
| player_two/battle_camera.cpp:48 | kModuleEntry | 0x10 | look-at, skill, command, leader change,  /  | 1283 (0) |  |
| player_two/battle_camera.cpp:49 | kModuleActive | 0x44 | look-at, skill, command, leader change,  /  | 249 (0) |  |
| player_two/battle_input.cpp:35 | kLockMode | 0x8c | BattlePlayer component (+0x258 object, + /  | 115 (0) |  |
| player_two/battle_input.cpp:36 | kSwitchLatch | 0x8d | BattlePlayer component (+0x258 object, + /  | 9 (0) |  |
| player_two/battle_input.cpp:37 | kSkipTargeting | 0x96 | BattlePlayer component (+0x258 object, + /  | 2 (0) |  |
| player_two/battle_input.cpp:38 | kDisplayed | 0x70 | BattlePlayer component (+0x258 object, + /  | 515 (0) |  |
| player_two/battle_input.cpp:39 | kTargetSlot | 0x258 | BattlePlayer component (+0x258 object, + /  | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| player_two/battle_input.cpp:41 | kPartyManagerLeader | 0x148 | BattlePlayer component (+0x258 object, + /  | 92 (0) |  |
| player_two/battle_input.cpp:45 | kController | 0xb8 | BattlePlayer component (+0x258 object, + /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/battle_input.cpp:46 | kBattleHud | 0x540 | BattlePlayer component (+0x258 object, + / u8 | 25 (13) | PaperFlipbookComponent.CachedBodySetup -> 0x530; PaperTileMapComponent.TileMapColor -> 0x530; EndBonamikBoxScaleGroupParamsComponent.m_ChangeChainRootRotDrag -> 0x530 |
| player_two/battle_input.cpp:47 | kBattleBlocked | 0x541 | BattlePlayer component (+0x258 object, + / u8 | 0 (0) |  |
| player_two/battle_input.cpp:48 | kBattleReadyA | 0x5e1 | BattlePlayer component (+0x258 object, + / u8 | 2 (0) |  |
| player_two/battle_input.cpp:49 | kBattleReadyB | 0x782 | BattlePlayer component (+0x258 object, + / u8 | 1 (0) |  |
| player_two/battle_input.cpp:50 | kGuardMode | 0x760 | BattlePlayer component (+0x258 object, + / int32 | 21 (2) | SkinnedMeshComponent.LODInfo -> 0x748; EndCardGame3DToken.ManagerTransform -> 0x780 |
| player_two/battle_input.cpp:51 | kRequests | 0x570 | BattlePlayer component (+0x258 object, + / TArray<int32> {data, num} | 20 (10) | SplineComponent.bDrawDebug -> 0x560; OctreeDynamicMeshComponent.bExplicitShowWireframe -> 0x560; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyVelocityDamping -> 0x560 |
| player_two/battle_input.cpp:55 | kCharacterHolder | 0x1758 | A party character's battle parts (§1.4,  / holder whose +0 is the character | 2 (1) | EndFA0122RobotArmCharacter.ElevatingSoundAsset -> 0x1778 |
| player_two/battle_input.cpp:56 | kCharacterAiHolder | 0x638 | A party character's battle parts (§1.4,  / its vt+0x48 gives the BattlePlayer component | 13 (10) | NiagaraComponent.Asset -> 0x628; EndFeelerConflictActor.LoseFadeOutTime -> 0x658; EndFeelerLaserActor.SkeletalMesh -> 0x658 |
| player_two/battle_input.cpp:57 | kOwnerSlot | 0x48 | A party character's battle parts (§1.4,  /  | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| player_two/battle_input.cpp:59 | kCommandHandle | 0x418 | A party character's battle parts (§1.4,  / the component's handle; null: the actions do nothi | 57 (2) | EndEnvironmentPhysicsStaticMeshActor.bSteeringObstacle -> 0x438; EndDialogueGenerateSourceShot.Light -> 0x428 |
| player_two/battle_input.cpp:60 | kHandleMode | 0x112 | A party character's battle parts (§1.4,  / u8 in the handle's object | 2 (0) |  |
| player_two/battle_input.cpp:62 | kPendingAction | 0x84 | A party character's battle parts (§1.4,  / int32 | 145 (0) |  |
| player_two/battle_input.cpp:206 | kStillFrames | 0x1e | Game thread. /  | 20 (0) |  |
| player_two/camera_distance.cpp:45 | kOptionFieldLevel | 0x36 | GetSystemOptionData(): the global option / Camera Distance: Out of Battle | 10 (0) |  |
| player_two/camera_distance.cpp:46 | kOptionBattleLevel | 0x37 | GetSystemOptionData(): the global option / Camera Distance: In Battle | 6 (0) |  |
| player_two/camera_distance.cpp:47 | kOpInitialized | 0xd2 | GetSystemOptionData(): the global option / the operator's lazy init ran (it seeds the level f | 5 (0) |  |
| player_two/camera_distance.cpp:48 | kOpLevel | 0x738 | GetSystemOptionData(): the global option /  | 28 (3) | SkinnedMeshComponent.PhysicsAssetOverride -> 0x700; NiagaraComponent.bForceSolo -> 0x728; NiagaraComponent.bEnableGpuComputeDebug -> 0x728 |
| player_two/camera_override.cpp:30 | kModules | 0x6e0 | Facts read from the database (RE-p2-batt / an operator's module table (TSharedPtr, 16 bytes e | 9 (3) | EndFeelerSplineActor.MasterSkeletalMeshComponents -> 0x700; EndFeelerTornadeActor.MasterSkeletalMeshComponents -> 0x700; ParticleSystemComponent.WarmupTickRate -> 0x6d0 |
| player_two/camera_override.cpp:32 | kModuleOwner | 0x50 | Facts read from the database (RE-p2-batt / the owner operator | 488 (0) |  |
| player_two/camera_override.cpp:33 | kModuleActive | 0x44 | Facts read from the database (RE-p2-batt / entering / active bytes | 249 (0) |  |
| player_two/camera_override.cpp:34 | kModuleRequests | 0x3f8 | Facts read from the database (RE-p2-batt / int32: request count | 73 (4) | EndEnvironmentPhysicsStaticMeshActor.BoundsShapeSize -> 0x418; EndBattleLevelSequenceActor.CameraActor -> 0x418; EndNavWheelieBlockWallVolume.TriggerName -> 0x418 |
| player_two/camera_override.cpp:35 | kBattleOperator | 0x2ca8 | Facts read from the database (RE-p2-batt / operator 11 | 0 (0) |  |
| player_two/camera_override.cpp:36 | kFieldOperator | 0x2cb0 | Facts read from the database (RE-p2-batt / operator 12 | 0 (0) |  |
| player_two/camera_override.cpp:39 | kAddedFn | 0x17a0 | Facts read from the database (RE-p2-batt /  | 1 (0) |  |
| player_two/camera_override.cpp:40 | kAddedPayload | 0x17c8 | Facts read from the database (RE-p2-batt /  | 2 (0) |  |
| player_two/camera_override.cpp:41 | kRemovedFn | 0x17e0 | Facts read from the database (RE-p2-batt /  | 2 (0) |  |
| player_two/camera_override.cpp:42 | kRemovedPayload | 0x1808 | Facts read from the database (RE-p2-batt /  | 1 (0) |  |
| player_two/camera_override.cpp:43 | kHeldRequests | 0x1770 | Facts read from the database (RE-p2-batt /  | 3 (1) | EndFA0122RobotArmCharacter.ContainerHitSoundAsset -> 0x1790 |
| player_two/charapop_centre.cpp:32 | kOverrideOn | 0x119c | once by CharaPop_LoadParamsFromEndParam. / u8 | 0 (0) |  |
| player_two/charapop_centre.cpp:33 | kOverrideCentre | 0x11a0 | once by CharaPop_LoadParamsFromEndParam. / FVector | 0 (0) |  |
| player_two/charapop_centre.cpp:34 | kLeaderOn | 0x11be | once by CharaPop_LoadParamsFromEndParam. / u8 | 0 (0) |  |
| player_two/charapop_centre.cpp:35 | kLeaderCentre | 0x11b0 | once by CharaPop_LoadParamsFromEndParam. / FVector | 1 (0) |  |
| player_two/charapop_centre.cpp:36 | kManagerSelect | 0x18 | once by CharaPop_LoadParamsFromEndParam. / pointer; the threshold at +0xE8 (i32) | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/charapop_centre.cpp:37 | kThreshold | 0xe8 | once by CharaPop_LoadParamsFromEndParam. /  | 156 (0) |  |
| player_two/charapop_centre.cpp:42 | kEvery | 0xa | once by CharaPop_LoadParamsFromEndParam. / ticks between passes | 53 (0) |  |
| player_two/charapop_facts.cpp:18 | kComponentWorldLocation | 0x1d0 | USceneComponent's world location (as han /  | 33 (0) |  |
| player_two/charapop_facts.h:22 | kRootCharaPop | 0x180 | The manager M = [[game root]+0x180] and  /  | 52 (5) | SkeletalMesh.bUseFullPrecisionUVs -> 0x186; SkeletalMesh.bUseHighPrecisionTangentBasis -> 0x186; SkeletalMesh.bHasBeenSimplified -> 0x186 |
| player_two/charapop_facts.h:23 | kManagerPop | 0x10 | The manager M = [[game root]+0x180] and  /  | 1283 (0) |  |
| player_two/charapop_facts.h:24 | kPopListInline | 0xc8 | The manager M = [[game root]+0x180] and  /  | 232 (0) |  |
| player_two/charapop_facts.h:25 | kPopListHeap | 0x20c8 | The manager M = [[game root]+0x180] and  /  | 0 (0) |  |
| player_two/charapop_facts.h:26 | kPopCount | 0x20d0 | The manager M = [[game root]+0x180] and  /  | 0 (0) |  |
| player_two/charapop_facts.h:27 | kMaxEntries | 0x1000 | The manager M = [[game root]+0x180] and  /  | 2 (0) |  |
| player_two/charapop_facts.h:30 | kShouldTickFov | 0x1148 | The LOD parameters in M (RE-p2-lod-draw- / degrees off the camera's forward | 0 (0) |  |
| player_two/charapop_facts.h:31 | kLightEnableDraw | 0x114c | The LOD parameters in M (RE-p2-lod-draw- /  | 0 (0) |  |
| player_two/charapop_facts.h:32 | kLightInCamera | 0x1150 | The LOD parameters in M (RE-p2-lod-draw- /  | 1 (0) |  |
| player_two/charapop_facts.h:33 | kLightOutCamera | 0x1154 | The LOD parameters in M (RE-p2-lod-draw- /  | 0 (0) |  |
| player_two/charapop_facts.h:34 | kLightShouldTick | 0x1158 | The LOD parameters in M (RE-p2-lod-draw- /  | 0 (0) |  |
| player_two/charapop_facts.h:35 | kMiddleEnableDraw | 0x115c | The LOD parameters in M (RE-p2-lod-draw- /  | 0 (0) |  |
| player_two/charapop_facts.h:36 | kMiddleInCamera | 0x1160 | The LOD parameters in M (RE-p2-lod-draw- /  | 0 (0) |  |
| player_two/charapop_facts.h:37 | kMiddleOutCamera | 0x1164 | The LOD parameters in M (RE-p2-lod-draw- /  | 0 (0) |  |
| player_two/charapop_facts.h:38 | kMiddleShouldTick | 0x1168 | The LOD parameters in M (RE-p2-lod-draw- /  | 1 (0) |  |
| player_two/charapop_facts.h:39 | kEnemyInCameraAdded | 0x1170 | The LOD parameters in M (RE-p2-lod-draw- /  | 0 (0) |  |
| player_two/charapop_facts.h:40 | kOverrideEnableDraw | 0x1178 | The LOD parameters in M (RE-p2-lod-draw- / with its enable byte at +4 | 0 (0) |  |
| player_two/charapop_facts.h:41 | kOverrideInCamera | 0x1180 | The LOD parameters in M (RE-p2-lod-draw- / with its enable byte at +4 | 1 (0) |  |
| player_two/charapop_facts.h:42 | kOverrideOutCamera | 0x1188 | The LOD parameters in M (RE-p2-lod-draw- / with its enable byte at +4 | 0 (0) |  |
| player_two/charapop_facts.h:45 | kEntryKey | 0x8 | One entry (§1.3). / 8 bytes, FName layout: the setter's key | 1705 (0) |  |
| player_two/charapop_facts.h:46 | kEntryId | 0x10 | One entry (§1.3). / i32 | 1283 (0) |  |
| player_two/charapop_facts.h:47 | kEntryCategory | 0x24 | One entry (§1.3). / u8: 1 has the draw-distance hide, 4 enemy | 305 (0) |  |
| player_two/charapop_facts.h:48 | kEntryLevel | 0x1a0 | One entry (§1.3). / u8: 1 Light, 2 Middle, 3 Heavy | 49 (0) |  |
| player_two/charapop_facts.h:49 | kEntryMinLevel | 0x1a1 | One entry (§1.3). / u8: the per-entry minimum (the setter's byte) | 0 (0) |  |
| player_two/charapop_facts.h:50 | kEntryActor | 0x1a4 | One entry (§1.3). / FWeakObjectPtr | 11 (0) |  |
| player_two/charapop_facts.h:51 | kEntryState | 0x1ac | One entry (§1.3). / i32: 5 visible, 6 fading out, 7 hidden, 8 fading i | 10 (0) |  |
| player_two/charapop_facts.h:53 | kLevelHeavy | 0x3 | One entry (§1.3). /  | 42 (0) |  |
| player_two/charapop_facts.h:57 | kCameraPose | 0x2cb8 | the pose's location at +0, its rotator a /  | 0 (0) |  |
| player_two/charapop_near.cpp:37 | kFirstEvaluatedState | 0x4 | The evaluator runs for these states only /  | 542 (0) |  |
| player_two/charapop_near.cpp:38 | kLastEvaluatedState | 0x9 | The evaluator runs for these states only /  | 82 (0) |  |
| player_two/charapop_near.cpp:40 | kEvery | 0xa | The evaluator runs for these states only / ticks between passes | 53 (0) |  |
| player_two/charapop_near.cpp:41 | kReleasePasses | 0x6 | The evaluator runs for these states only / a raised entry goes back after this many passes in | 23 (0) |  |
| player_two/charapop_watch.cpp:40 | kManagerParams | 0x1148 | watch's own extra fields (RE-p2-charapop / through +0x118C (RE-p2-lod-draw-distance.md §1) | 0 (0) |  |
| player_two/charapop_watch.cpp:42 | kPopForceAll | 0x619c | watch's own extra fields (RE-p2-charapop /  | 0 (0) |  |
| player_two/charapop_watch.cpp:43 | kPopQueueCount | 0x6228 | watch's own extra fields (RE-p2-charapop /  | 0 (0) |  |
| player_two/charapop_watch.cpp:44 | kPopCursor | 0x6230 | watch's own extra fields (RE-p2-charapop /  | 0 (0) |  |
| player_two/charapop_watch.cpp:45 | kEntryPauseFlag | 0x110 | watch's own extra fields (RE-p2-charapop / u32, cleared on unpause | 120 (0) |  |
| player_two/charapop_watch.cpp:46 | kEntryPauseWant | 0x114 | watch's own extra fields (RE-p2-charapop / u8, set on a pause request | 34 (0) |  |
| player_two/charapop_watch.cpp:47 | kEntryRequested | 0x1b0 | watch's own extra fields (RE-p2-charapop / i32 | 43 (0) |  |
| player_two/charapop_watch.cpp:48 | kEntryLock | 0x1b4 | watch's own extra fields (RE-p2-charapop / u8 | 10 (0) |  |
| player_two/charapop_watch.cpp:50 | kEntryUseCount | 0x40 | The population pass's fields (RE-p2-char / i32: 0 -> despawn | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/charapop_watch.cpp:51 | kEntryScore | 0x44 | The population pass's fields (RE-p2-char / i32: below the threshold -> despawn | 249 (0) |  |
| player_two/charapop_watch.cpp:52 | kEntryFlags | 0x118 | The population pass's fields (RE-p2-char / u32: 1 managed, 2 placed, 0x20 no despawn fade, 0x | 111 (1) | MovieScene.EvaluationType -> 0x119 |
| player_two/charapop_watch.cpp:53 | kEntryKeep | 0x11c | The population pass's fields (RE-p2-char / u32: keep reasons (no despawn) | 35 (0) |  |
| player_two/charapop_watch.cpp:54 | kEntrySpawnHidden | 0x1a2 | The population pass's fields (RE-p2-char / u8: over the visible budget | 0 (0) |  |
| player_two/charapop_watch.cpp:55 | kOverrideOn | 0x119c | The population pass's fields (RE-p2-char / u8, M | 0 (0) |  |
| player_two/charapop_watch.cpp:56 | kOverrideCentre | 0x11a0 | The population pass's fields (RE-p2-char / FVector, M | 0 (0) |  |
| player_two/charapop_watch.cpp:57 | kLeaderOn | 0x11be | The population pass's fields (RE-p2-char / u8, M | 0 (0) |  |
| player_two/charapop_watch.cpp:58 | kLeaderCentre | 0x11b0 | The population pass's fields (RE-p2-char / FVector, M | 1 (0) |  |
| player_two/charapop_watch.cpp:59 | kManagerSelect | 0x18 | The population pass's fields (RE-p2-char / M: pointer; the threshold at +0xE8 (i32) | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/charapop_watch.cpp:60 | kThreshold | 0xe8 | The population pass's fields (RE-p2-char /  | 156 (0) |  |
| player_two/charapop_watch.cpp:69 | kFadeSlot | 0x1d0 | The population pass's fields (RE-p2-char /  | 33 (0) |  |
| player_two/charapop_watch.cpp:70 | kFadeHidden | 0x88 | The population pass's fields (RE-p2-char / u8 | 292 (0) |  |
| player_two/charapop_watch.cpp:71 | kFadeOpacity | 0x8c | The population pass's fields (RE-p2-char / float, now | 115 (0) |  |
| player_two/charapop_watch.cpp:72 | kFadeTarget | 0x90 | The population pass's fields (RE-p2-char / float | 332 (1) | NiagaraEmitter.SpawnScriptProps -> 0x88 |
| player_two/charapop_watch.cpp:73 | kFadeReasons | 0xa4 | The population pass's fields (RE-p2-char / u32: bit 0 CharaPop or the wild-animal AI | 86 (0) |  |
| player_two/charapop_watch.cpp:76 | kActorHiddenByte | 0x58 | Stock visibility (spawn-gate doc §4; off / AActor::bHidden, bit 0x20 | 416 (1) | NiagaraEmitter.bLocalSpace -> 0x51 |
| player_two/charapop_watch.cpp:77 | kActorHiddenBit | 0x20 | Stock visibility (spawn-gate doc §4; off /  | 810 (0) |  |
| player_two/charapop_watch.cpp:78 | kCompVisibleByte | 0x14c | Stock visibility (spawn-gate doc §4; off / bVisible, bit 0x20 | 29 (0) |  |
| player_two/charapop_watch.cpp:79 | kCompVisibleBit | 0x20 | Stock visibility (spawn-gate doc §4; off /  | 810 (0) |  |
| player_two/charapop_watch.cpp:80 | kCompHiddenByte | 0x14d | Stock visibility (spawn-gate doc §4; off / bHiddenInGame, bit 0x04 | 5 (0) |  |
| player_two/charapop_watch.cpp:81 | kCompHiddenBit | 0x4 | Stock visibility (spawn-gate doc §4; off /  | 542 (0) |  |
| player_two/charapop_watch.cpp:82 | kCompLastRender | 0x2a4 | Stock visibility (spawn-gate doc §4; off / float: LastRenderTime, written by any view that dr | 16 (1) | LightComponent.DisabledBrightness -> 0x29c |
| player_two/charapop_watch.cpp:93 | kPausedSlot | 0x30 | push rdi; mov eax,80h. /  | 1052 (0) |  |
| player_two/charapop_watch.cpp:94 | kPauseReason | 0x6 | push rdi; mov eax,80h. /  | 23 (0) |  |
| player_two/charapop_watch.cpp:95 | kUnread | 0xff | push rdi; mov eax,80h. /  | 2 (0) |  |
| player_two/charapop_watch.cpp:99 | kSnapshotLocation | 0x8c0 | The animation world manager W = [0x14939 /  | 2 (1) | EndCardGame3DCardBoard.DeckMoveAfterTransform -> 0x8e0 |
| player_two/charapop_watch.cpp:100 | kSnapshotSpeed | 0x908 | The animation world manager W = [0x14939 / float | 6 (2) | SkeletalMeshComponent.CachedBoneSpaceTransforms -> 0x8f8; EndCardGame3DCardBoard.PredictBuffEffect -> 0x928 |
| player_two/charapop_watch.cpp:101 | kSnapshotTurn | 0x90c | The animation world manager W = [0x14939 / float | 3 (1) | EndCardGame3DCardBoard.bPredictDebuffVisible -> 0x92c |
| player_two/charapop_watch.cpp:102 | kSnapshotFaster | 0x910 | The animation world manager W = [0x14939 / two u8 | 8 (1) | EndCardGame3DCardBoard.PredictDebuffEffect -> 0x930 |
| player_two/charapop_watch.cpp:103 | kSnapshotValid | 0x913 | The animation world manager W = [0x14939 / u8 | 0 (0) |  |
| player_two/charapop_watch.cpp:106 | kCompLevelOn | 0x121c | UEndSkeletalMeshComponent's optimisation / u8: reads the owner's CharaPop level | 0 (0) |  |
| player_two/charapop_watch.cpp:107 | kCompForced | 0x1220 | UEndSkeletalMeshComponent's optimisation / u8 | 1 (1) | EndCharacterBase.LightAnimationComponent -> 0x1240 |
| player_two/charapop_watch.cpp:108 | kCompShadow | 0x1224 | UEndSkeletalMeshComponent's optimisation / u8: cast shadow | 0 (0) |  |
| player_two/charapop_watch.cpp:109 | kCompPauseLevel | 0x1226 | UEndSkeletalMeshComponent's optimisation / u8 | 0 (0) |  |
| player_two/charapop_watch.cpp:110 | kCompLevel | 0x1228 | UEndSkeletalMeshComponent's optimisation / u8: the owner's CharaPop level, copied | 2 (1) | EndCharacterBase.CoordinateAnimationComponent -> 0x1248 |
| player_two/charapop_watch.cpp:111 | kCompDistance | 0x1234 | UEndSkeletalMeshComponent's optimisation / float, to the snapshot camera | 0 (0) |  |
| player_two/charapop_watch.cpp:112 | kCompAngle | 0x1238 | UEndSkeletalMeshComponent's optimisation / float, radians | 1 (1) | EndCharacterBase.bForceUpdateAnimation -> 0x1258 |
| player_two/charapop_watch.cpp:114 | kSampleEvery | 0xa | UEndSkeletalMeshComponent's optimisation / ticks | 53 (0) |  |
| player_two/charapop_watch.cpp:119 | kUndrawnSamples | 0x3 | UEndSkeletalMeshComponent's optimisation / a mesh counts as not drawn after this many still s | 42 (0) |  |
| player_two/charapop_watch.cpp:120 | kSettleSamples | 0x3 | UEndSkeletalMeshComponent's optimisation / an entry's change is logged once it has held this  | 42 (0) |  |
| player_two/chest_watch.cpp:32 | kFirst | 0x5 | RE-p2-field-interact.md §13.5 (this buil / each entry's first instruction: mov [rsp+x], rbx | 30 (0) |  |
| player_two/chest_watch.cpp:33 | kPartyLeader | 0x148 | RE-p2-field-interact.md §13.5 (this buil / the party manager's leader, an FWeakObjectPtr | 92 (0) |  |
| player_two/chest_watch.cpp:34 | kStateBits | 0x131 | RE-p2-field-interact.md §13.5 (this buil / the treasure state's u8 bits; 0x20 granted (§13 li | 7 (0) |  |
| player_two/chest_watch.cpp:35 | kGrantedBit | 0x20 | RE-p2-field-interact.md §13.5 (this buil /  | 810 (0) |  |
| player_two/chocobo_ride.cpp:77 | kRootRideManager | 0x270 | Objects and fields (§1.1, §9.4; this bui /  | 67 (6) | PrimitiveComponent.TranslucencySortOffset -> 0x230; AudioComponent.AttenuationOverrides -> 0x268; NiagaraEmitter.m_VfxRandomSeed_EveryFrame_Params -> 0x258 |
| player_two/chocobo_ride.cpp:78 | kManagerGetOff | 0xeb0 | Objects and fields (§1.1, §9.4; this bui / u8 | 1 (0) |  |
| player_two/chocobo_ride.cpp:79 | kMemberEntries | 0x40 | Objects and fields (§1.1, §9.4; this bui /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/chocobo_ride.cpp:80 | kMemberEntrySize | 0x2c | Objects and fields (§1.1, §9.4; this bui /  | 299 (0) |  |
| player_two/chocobo_ride.cpp:81 | kPlayerTypes | 0xf | Objects and fields (§1.1, §9.4; this bui /  | 19 (0) |  |
| player_two/chocobo_ride.cpp:82 | kEntryRiding | 0x4 | Objects and fields (§1.1, §9.4; this bui / the member entry's state byte while she rides | 542 (0) |  |
| player_two/chocobo_ride.cpp:83 | kChocoboType | 0x1 | Objects and fields (§1.1, §9.4; this bui / the vehicle type of a chocobo coordinator | 155 (0) |  |
| player_two/chocobo_ride.cpp:84 | kActiveVehicle | 0x5b8 | Objects and fields (§1.1, §9.4; this bui / AEndPlayerController::ActiveVehicleCoordinator | 19 (8) | SynthComponent.ConcurrencySet -> 0x5b0; EndBikeSplineComponent.BuggyEnableInputSlideLength -> 0x5a8; EndFeelerAttendantActor.WalkToIdleMoveDelayTimeRange -> 0x5d8 |
| player_two/chocobo_ride.cpp:85 | kCoordState | 0x58 | Objects and fields (§1.1, §9.4; this bui / the coordinator's state machine: current row -> u3 | 416 (1) | NiagaraEmitter.bLocalSpace -> 0x51 |
| player_two/chocobo_ride.cpp:86 | kCoordPending | 0x60 | Objects and fields (§1.1, §9.4; this bui / ... pending row (null when none) | 484 (1) | NiagaraEmitter.AllocationMode -> 0x53 |
| player_two/chocobo_ride.cpp:87 | kCoordIdle | 0x0 | Objects and fields (§1.1, §9.4; this bui /  | 2161 (0) |  |
| player_two/chocobo_ride.cpp:88 | kCoordRiding | 0x2 | Objects and fields (§1.1, §9.4; this bui /  | 90 (0) |  |
| player_two/chocobo_ride.cpp:89 | kChocoboRidden | 0x1924 | Objects and fields (§1.1, §9.4; this bui / u8 | 1 (0) |  |
| player_two/chocobo_ride.cpp:90 | kChocoboRideObject | 0x1918 | Objects and fields (§1.1, §9.4; this bui /  | 1 (0) |  |
| player_two/chocobo_ride.cpp:91 | kControlHolder | 0x638 | Objects and fields (§1.1, §9.4; this bui / its vt+0x40 gives the input component | 13 (10) | NiagaraComponent.Asset -> 0x628; EndFeelerConflictActor.LoseFadeOutTime -> 0x658; EndFeelerLaserActor.SkeletalMesh -> 0x658 |
| player_two/chocobo_ride.cpp:92 | kHolderComponent | 0x40 | Objects and fields (§1.1, §9.4; this bui /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/chocobo_ride.cpp:93 | kAttachParent | 0xc0 | Objects and fields (§1.1, §9.4; this bui / USceneComponent::AttachParent | 255 (0) |  |
| player_two/chocobo_ride.cpp:94 | kViewTarget | 0x1000 | Objects and fields (§1.1, §9.4; this bui / PCM ViewTarget.Target | 2 (0) |  |
| player_two/chocobo_ride.cpp:95 | kRideSubjectField | 0x2cc8 | Objects and fields (§1.1, §9.4; this bui / PCM, weak | 0 (0) |  |
| player_two/chocobo_ride.cpp:96 | kFieldOperator | 0x2cb0 | Objects and fields (§1.1, §9.4; this bui / PCM: the Field operator | 0 (0) |  |
| player_two/chocobo_ride.cpp:97 | kOperatorModules | 0x6e0 | Objects and fields (§1.1, §9.4; this bui / op: the module slots (16 bytes each) | 9 (3) | EndFeelerSplineActor.MasterSkeletalMeshComponents -> 0x700; EndFeelerTornadeActor.MasterSkeletalMeshComponents -> 0x700; ParticleSystemComponent.WarmupTickRate -> 0x6d0 |
| player_two/chocobo_ride.cpp:110 | kEndRetryTicks | 0x3c | (RE-p2-chocobo-rows.md §2); 1 = lit, 2 = / ticks to wait for the coordinator to reach riding  | 318 (0) |  |
| player_two/combat_rumble.cpp:78 | kComponentOwner | 0xa0 | slot, the function it holds, its GetFirs / UActorComponent::OwnerPrivate | 264 (0) |  |
| player_two/combat_rumble.cpp:86 | kComponentController | 0x10 | slot, the function it holds, its GetFirs / the End force-feedback component's controller | 1283 (0) |  |
| player_two/command_locks.cpp:45 | kFirstInstruction | 0x5 | conditions), is replaced. /  | 30 (0) |  |
| player_two/command_locks.cpp:48 | kController | 0xb8 | conditions), is replaced. / EndUISystem[23] | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/command_locks.cpp:50 | kCommandsEnabled | 0x5e2 | The controller's values (RE-p2-command-m / u8 | 1 (0) |  |
| player_two/command_locks.cpp:51 | kCommandsLocked | 0x5e3 | The controller's values (RE-p2-command-m / u8 | 0 (0) |  |
| player_two/command_locks.cpp:52 | kCategoryLocks | 0x2d8 | The controller's values (RE-p2-command-m / u8 array {data, count +0x8} | 50 (4) | EndCinemaWidgetControlComponent.TextFieldsByStoryFlag -> 0x2d0; EndStreamingVolume.DataAssets -> 0x2e8; PlanarReflectionComponent.DistanceFromPlaneFadeoutEnd -> 0x2d0 |
| player_two/command_locks.cpp:53 | kCategoryCount | 0x2e0 | The controller's values (RE-p2-command-m / i32 | 36 (2) | PrimitiveComponent.BodyInstance -> 0x2d0; PlanarReflectionComponent.AngleFromPlaneFadeEnd -> 0x2d8 |
| player_two/command_locks.cpp:54 | kMaxCategories | 0x20 | The controller's values (RE-p2-command-m /  | 810 (0) |  |
| player_two/command_menu_tick.cpp:73 | kPartyList | 0x360 | EndUI23_SuspendHudInput(u8 kind, u8 rese / the controller's party list object | 16 (2) | EndNavMultiSwitchingVolume.ModifierVolume -> 0x380; WidgetInteractionComponent.HoveredWidgetComponent -> 0x358 |
| player_two/command_menu_tick.cpp:74 | kPartyListInput | 0x4a0 | EndUI23_SuspendHudInput(u8 kind, u8 rese / u8 | 29 (15) | MeshComponent.DynamicMaterialsForParameters -> 0x490; SQEXSEADLayoutComponent.LayoutInfo -> 0x490; PaperTerrainComponent.AssociatedSpline -> 0x490 |
| player_two/command_menu_tick.cpp:75 | kSetupPending | 0x775 | EndUI23_SuspendHudInput(u8 kind, u8 rese / u8: the tick re-runs Setup | 0 (0) |  |
| player_two/command_menu_tick.cpp:87 | kEventKey | 0x18 | (RE-p2-targeting-scoped-tick.md §7). /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/command_menu_tick.cpp:99 | kKeyEventUser | 0xc | are checked after that instruction. /  | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| player_two/command_menu_tick.cpp:107 | kVisibility | 0xc3 | done"), SuspendHudInput(0, _) Collapsed  /  | 6 (0) |  |
| player_two/command_menu_tick.cpp:108 | kMenuWidget | 0x4f0 | done"), SuspendHudInput(0, _) Collapsed  / the controller's menu (FWeakObjectPtr) | 24 (6) | EndBonamikBoxScaleGroupParamsComponent.m_ChangeMassScale -> 0x4e0; EndBonamikRaycastWindComponent.WindDirection -> 0x4e0; EndDynamicBodyCollisionPrimitive.IgnoreActorNames -> 0x4e0 |
| player_two/command_menu_tick.cpp:109 | kFieldMenu | 0x4e8 | done"), SuspendHudInput(0, _) Collapsed  / its field menu, which holds the party list (FWeakO | 34 (5) | ControlRigComponent.MappedElements -> 0x4d8; EndBonamikBoxScaleGroupParamsComponent.m_ChangeWindScale -> 0x4d8; EndDynamicBodyCollisionPrimitive.PushSpeed -> 0x4d8 |
| player_two/command_menu_tick.cpp:131 | kFilterManager | 0x148 | other menu is still in wait mode. / EndUISystem[41] | 92 (0) |  |
| player_two/command_menu_tick.cpp:132 | kFilterLatch | 0x1c | other menu is still in wait mode. / u8 per type | 361 (0) |  |
| player_two/command_menu_tick.cpp:133 | kFilterCount | 0x2c | other menu is still in wait mode. / i32 per type | 299 (0) |  |
| player_two/command_menu_tick.cpp:134 | kTintType | 0x0 | other menu is still in wait mode. /  | 2161 (0) |  |
| player_two/command_menu_tick.cpp:151 | kInput | 0x10 | The action keys (FKeys) the route and it /  | 1283 (0) |  |
| player_two/command_menu_tick.cpp:152 | kController | 0xb8 | The action keys (FKeys) the route and it /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/command_menu_tick.cpp:154 | kLeaderType | 0x150 | The action keys (FKeys) the route and it /  | 90 (0) |  |
| player_two/command_menu_tick.cpp:155 | kPrimary | 0x140 | The action keys (FKeys) the route and it /  | 102 (1) | NiagaraEmitter.RendererBindings -> 0x128 |
| player_two/command_menu_tick.cpp:156 | kLeaderWeak | 0x148 | The action keys (FKeys) the route and it /  | 92 (0) |  |
| player_two/command_menu_tick.cpp:159 | kStoredLeaderType | 0x30 | The controller (RE-p2-command-menu-tick. / u8 | 1052 (0) |  |
| player_two/command_menu_tick.cpp:160 | kMembers | 0x34 | The controller (RE-p2-command-menu-tick. / i32 x3: leader ordinal, ally, shortcut target (RE- | 329 (0) |  |
| player_two/command_menu_tick.cpp:161 | kBattleInput | 0x25c | The controller (RE-p2-command-menu-tick. / u8 | 30 (5) | AudioComponent.LowPassFilterFrequency -> 0x254; EndFieldClimbingPointSettingBase.CatchToHandSlideSaveStoryFlag -> 0x254; LightComponent.ShadowBias -> 0x254 |
| player_two/command_menu_tick.cpp:162 | kSwitchingLayer | 0x258 | The controller (RE-p2-command-menu-tick. / u8 | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| player_two/command_menu_tick.cpp:163 | kRequests | 0x570 | The controller (RE-p2-command-menu-tick. / TArray<int32> {data, num} | 20 (10) | SplineComponent.bDrawDebug -> 0x560; OctreeDynamicMeshComponent.bExplicitShowWireframe -> 0x560; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyVelocityDamping -> 0x560 |
| player_two/command_menu_tick.cpp:164 | kHudMode | 0x540 | The controller (RE-p2-command-menu-tick. / u8 | 25 (13) | PaperFlipbookComponent.CachedBodySetup -> 0x530; PaperTileMapComponent.TileMapColor -> 0x530; EndBonamikBoxScaleGroupParamsComponent.m_ChangeChainRootRotDrag -> 0x530 |
| player_two/command_menu_tick.cpp:165 | kBlocked | 0x541 | The controller (RE-p2-command-menu-tick. / u8 | 0 (0) |  |
| player_two/command_menu_tick.cpp:166 | kReady | 0x5e1 | The controller (RE-p2-command-menu-tick. / u8 | 2 (0) |  |
| player_two/command_menu_tick.cpp:167 | kWaitFilter | 0x644 | The controller (RE-p2-command-menu-tick. / i32 | 6 (4) | NiagaraComponent.RandomSeedOffset -> 0x634; EndFeelerBarrierActor.CullingDistance -> 0x664; EndInstancedLightweightPhysicsComponent.InitialRotationNoise -> 0x634 |
| player_two/command_menu_tick.cpp:168 | kPinch | 0x650 | The controller (RE-p2-command-menu-tick. / u8 | 20 (10) | SynthComponent.bIsUISound -> 0x648; SynthComponent.bIsPreviewSound -> 0x648; EndFeelerBarrierActor.bRefreshSpline -> 0x670 |
| player_two/command_menu_tick.cpp:169 | kGuardLatch | 0x760 | The controller (RE-p2-command-menu-tick. / i32 | 21 (2) | SkinnedMeshComponent.LODInfo -> 0x748; EndCardGame3DToken.ManagerTransform -> 0x780 |
| player_two/command_menu_tick.cpp:170 | kHudSoundMode | 0x774 | The controller (RE-p2-command-menu-tick. / u8 | 5 (1) | NiagaraComponent.MaxTimeBeforeForceUpdateTransform -> 0x764 |
| player_two/command_menu_tick.cpp:171 | kReadyB | 0x782 | The controller (RE-p2-command-menu-tick. / u8 | 1 (0) |  |
| player_two/command_menu_tick.cpp:172 | kMenuLayer | 0x783 | The controller (RE-p2-command-menu-tick. / u8 | 0 (0) |  |
| player_two/command_menu_tick.cpp:188 | kDodgePress | 0x3 | The controller (RE-p2-command-menu-tick. / req[32]: a dodge press | 42 (0) |  |
| player_two/command_menu_tick.cpp:189 | kDodgeGuarding | 0x4 | The controller (RE-p2-command-menu-tick. / req[32]: a dodge press while guarding | 542 (0) |  |
| player_two/command_menu_tick.cpp:190 | kGuardHeld | 0x1 | The controller (RE-p2-command-menu-tick. / req[32]: guard held | 155 (0) |  |
| player_two/command_menu_tick.cpp:192 | kSnapshotTypes | 0xf | The controller (RE-p2-command-menu-tick. /  | 19 (0) |  |
| player_two/command_menu_tick.cpp:195 | kOpenPending | 0x5d0 | The controller (RE-p2-command-menu-tick. / i32: a menu open requested (>= 0), pushed 0.125 s  | 20 (11) | EndCrashBoxMiniGameElectricCableActor.m_StaticMeshComponent -> 0x5f0; EndFeelerAttendantActor.WalkMoveVelocityInputRange -> 0x5f0; EndFeelerBarrierActor.ExtrusionWaitTimeRange -> 0x5f0 |
| player_two/command_menu_tick.cpp:196 | kGate777 | 0x777 | The controller (RE-p2-command-menu-tick. / u8: [UNKNOWN] 1 on the game's controller; 0 on pla | 0 (0) |  |
| player_two/command_menu_tick.cpp:447 | kWatchStillFrames | 0x1e | full reset would also clear "setup done" /  | 20 (0) |  |
| player_two/command_menu_two.cpp:66 | kRefreshFirst | 0x5 | player 2's focus scope. /  | 30 (0) |  |
| player_two/command_menu_two.cpp:75 | kControllerSize | 0x798 | player 2's focus scope. /  | 17 (10) | SkinnedMeshComponent.bCastCapsuleDirectShadow -> 0x780; SkinnedMeshComponent.bCastCapsuleIndirectShadow -> 0x780; SkinnedMeshComponent.bCPUSkinning -> 0x780 |
| player_two/command_menu_two.cpp:82 | kKeyGuideSize | 0x38 | player 2's focus scope. /  | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| player_two/command_menu_two.cpp:83 | kKeyGuideSlot | 0x1a8 | player 2's focus scope. / EndUISystem[53] | 34 (0) |  |
| player_two/command_menu_two.cpp:84 | kKeyGuideWidget | 0x4 | player 2's focus scope. /  | 542 (0) |  |
| player_two/command_menu_two.cpp:87 | kGameController | 0xb8 | The UI system's command controller slot  /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/command_menu_two.cpp:88 | kHudMode | 0x540 | The UI system's command controller slot  / u8: the battle HUD mode | 25 (13) | PaperFlipbookComponent.CachedBodySetup -> 0x530; PaperTileMapComponent.TileMapColor -> 0x530; EndBonamikBoxScaleGroupParamsComponent.m_ChangeChainRootRotDrag -> 0x530 |
| player_two/command_menu_two.cpp:92 | kSetupDone | 0x777 | UI system's +0x2D1 marks a Setup deferre /  | 0 (0) |  |
| player_two/command_menu_two.cpp:93 | kSetupPending | 0x775 | UI system's +0x2D1 marks a Setup deferre /  | 0 (0) |  |
| player_two/command_menu_two.cpp:94 | kSetupDeferred | 0x2d1 | UI system's +0x2D1 marks a Setup deferre / in the UI system | 3 (0) |  |
| player_two/command_menu_two.cpp:95 | kSelecterSet | 0x776 | UI system's +0x2D1 marks a Setup deferre / u8, set by Setup (logged) | 0 (0) |  |
| player_two/command_menu_two.cpp:96 | kSetupOther | 0x778 | UI system's +0x2D1 marks a Setup deferre / u8, set by Setup (logged) | 13 (0) |  |
| player_two/command_menu_two.cpp:97 | kMenuLayer | 0x783 | UI system's +0x2D1 marks a Setup deferre / u8 (logged) | 0 (0) |  |
| player_two/command_menu_two.cpp:102 | kMembers | 0x34 | UI system's +0x2D1 marks a Setup deferre /  | 329 (0) |  |
| player_two/command_menu_two.cpp:103 | kMenuWidget | 0x4f0 | UI system's +0x2D1 marks a Setup deferre /  | 24 (6) | EndBonamikBoxScaleGroupParamsComponent.m_ChangeMassScale -> 0x4e0; EndBonamikRaycastWindComponent.WindDirection -> 0x4e0; EndDynamicBodyCollisionPrimitive.IgnoreActorNames -> 0x4e0 |
| player_two/command_menu_two.cpp:104 | kWidgetCtrl | 0x4e0 | UI system's +0x2D1 marks a Setup deferre /  | 31 (13) | FXSystemComponent.m_VfxCategory -> 0x4d0; EndFieldActionTriggerComponent.CustomParam -> 0x4d0; EndBodyCollisionPrimitive.bIgnoreDodgeRoll -> 0x4d0 |
| player_two/command_menu_two.cpp:107 | kObjectClass | 0x10 | UObjectBase: the class and the outer. /  | 1283 (0) |  |
| player_two/command_menu_two.cpp:108 | kObjectOuter | 0x20 | UObjectBase: the class and the outer. /  | 810 (0) |  |
| player_two/command_rules.cpp:38 | kMenuOrdinals | 0x8 | The chooser's HP call: Party_GetHpByType / command-menu ordinals searched for the member | 1705 (0) |  |
| player_two/command_rules.cpp:48 | kController | 0xb8 | The command controller (EndUISystem+0xB8 /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/command_rules.cpp:49 | kMembers | 0x34 | The command controller (EndUISystem+0xB8 / int32 x4: leader, ally, shortcut target, ally pick | 329 (0) |  |
| player_two/command_rules.cpp:50 | kRequests | 0x570 | The command controller (EndUISystem+0xB8 / TArray<int32> {data, num} | 20 (10) | SplineComponent.bDrawDebug -> 0x560; OctreeDynamicMeshComponent.bExplicitShowWireframe -> 0x560; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyVelocityDamping -> 0x560 |
| player_two/command_rules.cpp:51 | kHudMode | 0x540 | The command controller (EndUISystem+0xB8 /  | 25 (13) | PaperFlipbookComponent.CachedBodySetup -> 0x530; PaperTileMapComponent.TileMapColor -> 0x530; EndBonamikBoxScaleGroupParamsComponent.m_ChangeChainRootRotDrag -> 0x530 |
| player_two/command_rules.cpp:52 | kAllyMode | 0x44c | The command controller (EndUISystem+0xB8 /  | 2 (0) |  |
| player_two/command_rules.cpp:53 | kReady | 0x5e1 | The command controller (EndUISystem+0xB8 /  | 2 (0) |  |
| player_two/command_rules.cpp:58 | kGameState | 0x288 | its class; 12 floats at +0x3C0; slot 0 i /  | 71 (6) | PrimitiveComponent.VirtualTextureLodBias -> 0x278; BoxReflectionCaptureComponent.PreviewInfluenceBox -> 0x280; PlaneReflectionCaptureComponent.PreviewInfluenceRadius -> 0x280 |
| player_two/command_rules.cpp:59 | kSpeedSlots | 0x3c0 | its class; 12 floats at +0x3C0; slot 0 i /  | 32 (1) | EndShootingCoasterSpawnerTriggerBox.BoxTag -> 0x3e0 |
| player_two/command_rules.cpp:97 | kStillFrames | 0x1e | Game thread. /  | 20 (0) |  |
| player_two/command_rules_code.h:24 | kCommandGateCodeSize | 0x16 | execute exactly these bytes. /  | 11 (0) |  |
| player_two/engine_facts.h:38 | kActorOwner | 0xe8 | AActor / AActor::SetOwner stores it | 156 (0) |  |
| player_two/engine_facts.h:39 | kActorTagsData | 0x178 | AActor / Tags.GetData() | 44 (0) |  |
| player_two/engine_facts.h:40 | kActorTagsCount | 0x180 | AActor / Tags.Num() | 52 (5) | SkeletalMesh.bUseFullPrecisionUVs -> 0x186; SkeletalMesh.bUseHighPrecisionTangentBasis -> 0x186; SkeletalMesh.bHasBeenSimplified -> 0x186 |
| player_two/engine_facts.h:41 | kPawnController | 0x260 | AActor / APawn::Controller | 99 (6) | NiagaraEmitter.m_VfxRandomSeed_SpawnOnly_Params -> 0x248; EndCinemaWidgetControlComponent.WidgetAsset -> 0x258; AtmosphericFogComponent.IrradianceTexture -> 0x258 |
| player_two/engine_facts.h:44 | kLocalPlayerViewportClient | 0x70 | ULocalPlayer /  | 515 (0) |  |
| player_two/engine_facts.h:45 | kLocalPlayerControllerId | 0xb8 | ULocalPlayer /  | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/engine_facts.h:49 | kViewportInputModeBytes | 0x361 | through SetInputMode on the one viewport /  | 0 (0) |  |
| player_two/engine_facts.h:52 | kControllerPawn | 0x258 | Player controllers (AEndPlayerController /  | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| player_two/engine_facts.h:53 | kControllerPlayer | 0x2a0 | Player controllers (AEndPlayerController / the ULocalPlayer | 43 (2) | EndFieldClimbingCatchPointSetting.JumpPoints -> 0x290; LightComponent.LightFunctionFadeDistance -> 0x298 |
| player_two/engine_facts.h:54 | kControllerCameraManager | 0x2c0 | Player controllers (AEndPlayerController /  | 57 (10) | PrimitiveComponent.MoveIgnoreComponents -> 0x2b0; EndCinemaWidgetControlComponent.StoryFlags -> 0x2b8; EndFieldClimbingFinishPointSetting.ManualEnterEffectOffset -> 0x2b0 |
| player_two/engine_facts.h:55 | kControllerBattleTalkActor | 0x5b0 | Player controllers (AEndPlayerController /  | 17 (8) | SynthComponent.ConcurrencySettings -> 0x5a8; EndBikeSplineComponent.EnemySlideMoveLimitWidth -> 0x5a0; EndFeelerAttendantActor.WalkToIdleStateDelayTimeRange -> 0x5d0 |
| player_two/engine_facts.h:56 | kControllerLeader | 0x5a0 | Player controllers (AEndPlayerController /  | 25 (12) | EndBikeSplineComponent.SplineJumpAreaData -> 0x590; EndFeelerAttendantActor.IdleToWalkStateDelayTimeRange -> 0x5c0; EndFeelerBarrierActor.PrimaryRollSize -> 0x5c0 |
| player_two/engine_facts.h:57 | kControllerTrigger | 0x610 | Player controllers (AEndPlayerController /  | 14 (10) | SynthComponent.SourceEffectChain -> 0x608; EndFeelerBarrierActor.LoopAnimations -> 0x630; EndFeelerConflictActor.EndCenterAnimatedOffsetLoopTimeRate -> 0x630 |
| player_two/engine_facts.h:58 | kControllerAutoManageCamera | 0x2d0 | Player controllers (AEndPlayerController / u8 bAutoManageActiveCameraTarget | 46 (3) | EndCinemaWidgetControlComponent.LogicalOperator -> 0x2c8; PlanarReflectionComponent.DistanceFromPlaneFadeEnd -> 0x2c8; SceneCaptureComponentCube.TextureTargetODS -> 0x2c8 |
| player_two/engine_facts.h:59 | kControllerLeaderActions | 0x598 | Player controllers (AEndPlayerController / the move-state tick calls through it unchecked | 25 (13) | EndDolphinJumpSplineComponent.Phases -> 0x588; EndFeelerAttendantActor.WalkTransitionDeltaSizeThreshold -> 0x5b8; EndFeelerConflictActor.PrimaryRollSize -> 0x5b8 |
| player_two/engine_facts.h:60 | kPrimaryControllerByte897 | 0x897 | Player controllers (AEndPlayerController / written by the switch through "the primary" | 0 (0) |  |
| player_two/engine_facts.h:61 | kPrimaryControllerByte8AB | 0x8ab | Player controllers (AEndPlayerController /  | 0 (0) |  |
| player_two/engine_facts.h:64 | kCameraManagerPcOwner | 0x228 | Camera managers (AEndPlayerCameraManager /  | 209 (28) | PrimitiveComponent.bUseMaxLODAsImposter -> 0x220; PrimitiveComponent.bBatchImpostersAsInstances -> 0x220; PrimitiveComponent.bNeverDistanceCull -> 0x220 |
| player_two/engine_facts.h:65 | kCameraManagerViewTarget | 0x1000 | Camera managers (AEndPlayerCameraManager / ViewTarget.Target | 2 (0) |  |
| player_two/engine_facts.h:66 | kCameraManagerOperators | 0x2c50 | Camera managers (AEndPlayerCameraManager / 13 operator pointers | 0 (0) |  |
| player_two/engine_facts.h:67 | kCameraManagerOperatorCount | 0xd | Camera managers (AEndPlayerCameraManager /  | 32 (0) |  |
| player_two/engine_facts.h:68 | kCameraManagerFieldOperator | 0x2cb0 | Camera managers (AEndPlayerCameraManager / operator 12 | 0 (0) |  |
| player_two/engine_facts.h:69 | kCameraManagerCurrentOperator | 0x2cd0 | Camera managers (AEndPlayerCameraManager / u8 index into the operators | 0 (0) |  |
| player_two/engine_facts.h:70 | kCameraManagerCollisionComponent | 0x2d48 | Camera managers (AEndPlayerCameraManager / outer: the collision actor | 1 (0) |  |
| player_two/engine_facts.h:71 | kCameraManagerEndCameraActor | 0x2de8 | Camera managers (AEndPlayerCameraManager /  | 1 (0) |  |
| player_two/engine_facts.h:74 | kFieldOperatorCommand | 0x40 | Field camera operator /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/engine_facts.h:75 | kFieldCommandSize | 0x58 | Field camera operator /  | 416 (1) | NiagaraEmitter.bLocalSpace -> 0x51 |
| player_two/engine_facts.h:76 | kFieldOperatorActive | 0xd0 | Field camera operator /  | 205 (0) |  |
| player_two/engine_facts.h:79 | kCharacterRequestedController | 0x578 | Characters (AEndCharacter) /  | 22 (9) | SplineComponent.bClosedLoop -> 0x568; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyRadius -> 0x568; EndFeelerAttendantActor.SkeletalMesh -> 0x598 |
| player_two/engine_facts.h:80 | kCharacterEffectiveController | 0x580 | Characters (AEndCharacter) /  | 17 (8) | SplineComponent.DefaultUpVector -> 0x570; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyFriction -> 0x570; EndFeelerAttendantActor.AnimationBlueprint -> 0x5a0 |
| player_two/engine_facts.h:81 | kCharacterRetainedAi | 0x588 | Characters (AEndCharacter) /  | 33 (11) | EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyGravity -> 0x578; EndEnvironmentStaticMeshComponent.bOverrideCollisionExtraAttribute -> 0x578; EndFeelerAttendantActor.AnimationStartPositionRange -> 0x5a8 |
| player_two/engine_facts.h:82 | kCharacterPlayerControlled | 0x625 | Characters (AEndCharacter) /  | 0 (0) |  |
| player_two/engine_facts.h:83 | kCharacterMoveState | 0x1750 | Characters (AEndCharacter) /  | 3 (1) | EndFA0122RobotArmCharacter.StopTurningSoundAsset -> 0x1770 |
| player_two/engine_facts.h:86 | kAiActiveBrain | 0x518 | The AI controller's active brain and its /  | 27 (13) | SkinnedMeshComponent.SkeletalMesh -> 0x508; ProceduralMeshComponent.bUseComplexAsSimpleCollision -> 0x508; PaperFlipbookComponent.Material -> 0x508 |
| player_two/engine_facts.h:87 | kBrainIndex | 0x19 | The AI controller's active brain and its / u8; 0 is the base brain | 59 (0) |  |
| player_two/engine_facts.h:88 | kBrainState | 0x20 | The AI controller's active brain and its /  | 810 (0) |  |
| player_two/engine_facts.h:89 | kAiStateType | 0x18 | The AI controller's active brain and its / u8, EEndAIState | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/engine_facts.h:92 | kMoveStateCharacter | 0x8 | Player move state /  | 1705 (0) |  |
| player_two/engine_facts.h:93 | kMoveStateBlocked | 0x91 | Player move state / u8; the gate refuses when nonzero | 22 (0) |  |
| player_two/engine_facts.h:94 | kMoveStateCooldown | 0x94 | Player move state / float; the gate refuses when > 0 | 91 (0) |  |
| player_two/engine_facts.h:102 | kMoveBlockingTagA | 0x11 | request byte): Dash, Dismount, Jet, Scen /  | 36 (0) |  |
| player_two/engine_facts.h:103 | kMoveBlockingTagB | 0x16 | request byte): Dash, Dismount, Jet, Scen /  | 11 (0) |  |
| player_two/engine_facts.h:112 | kPartyManagerPrimaryController | 0x140 | Globals /  | 102 (1) | NiagaraEmitter.RendererBindings -> 0x128 |
| player_two/engine_facts.h:120 | kNameNone | 0x0 | Globals /  | 2161 (0) |  |
| player_two/engine_facts.h:136 | kOptionHolderData | 0x100 | Globals /  | 130 (0) |  |
| player_two/engine_facts.h:143 | kInputMergedSticks | 0x1210 | Globals / float LX, LY, RX, RY | 5 (4) | SkeletalMeshComponentBudgeted.bAutoRegisterWithBudgetAllocator -> 0x1200; SkeletalMeshComponentBudgeted.bAutoCalculateSignificance -> 0x1200; SkeletalMeshComponentBudgeted.bShouldUseActorRenderedFlag -> 0x1200 |
| player_two/engine_facts.h:144 | kInputDeviceBanks | 0x11d0 | Globals / {Record* data; int32 num; int32 max} | 0 (0) |  |
| player_two/engine_facts.h:145 | kInputBankHeaderSize | 0x10 | Globals /  | 1283 (0) |  |
| player_two/engine_facts.h:146 | kXInputBank | 0x1 | Globals /  | 155 (0) |  |
| player_two/engine_facts.h:147 | kDirectInputBank | 0x2 | Globals /  | 90 (0) |  |
| player_two/engine_facts.h:152 | kTagRegistryMap | 0x278 | Globals /  | 62 (5) | PrimitiveComponent.RuntimeVirtualTextures -> 0x268; LightComponent.LightFunctionMaterial -> 0x270; ExponentialHeightFogComponent.Medium -> 0x270 |
| player_two/engine_facts.h:153 | kTagRegistryElementSize | 0x14 | Globals /  | 478 (0) |  |
| player_two/engine_facts.h:154 | kTagRegistryCount | 0x8 | Globals /  | 1705 (0) |  |
| player_two/engine_facts.h:158 | kFieldLane | 0x0 | Globals /  | 2161 (0) |  |
| player_two/engine_facts.h:159 | kNoPlayerType | 0x10 | Globals /  | 1283 (0) |  |
| player_two/engine_facts.h:160 | kMaxRosterEntries | 0x10 | Globals /  | 1283 (0) |  |
| player_two/engine_facts.h:322 | kPlayerTypeSlotOffset | 0x68 | Functions /  | 614 (1) | NiagaraEmitter.UpdateScriptProps -> 0x60 |
| player_two/engine_facts.h:398 | kLeaderActionOwner | 0xa0 | Replaced vtable slots /  | 264 (0) |  |
| player_two/field_camera.cpp:45 | kHudMode | 0x540 | Player 2's command controller's battle H /  | 25 (13) | PaperFlipbookComponent.CachedBodySetup -> 0x530; PaperTileMapComponent.TileMapColor -> 0x530; EndBonamikBoxScaleGroupParamsComponent.m_ChangeChainRootRotDrag -> 0x530 |
| player_two/field_exit.cpp:25 | kFirstInstruction | 0x5 | inside it), so all three reach RequestEx /  | 30 (0) |  |
| player_two/field_exit.cpp:28 | kFieldStep | 0x70 | inside it), so all three reach RequestEx / EEndFieldStep; 13 once the entry steps are done | 515 (0) |  |
| player_two/field_exit.cpp:29 | kRunning | 0x71 | inside it), so all three reach RequestEx / 1 while the field runs | 24 (0) |  |
| player_two/field_exit.cpp:30 | kStepSettled | 0xd | inside it), so all three reach RequestEx /  | 32 (0) |  |
| player_two/field_pickup.cpp:46 | kRootStage | 0x70 | The objects (RE-p2-field-interact.md §1. / u8: the input step's FieldAction part runs from 8 | 515 (0) |  |
| player_two/field_pickup.cpp:47 | kRootGameState | 0x170 | The objects (RE-p2-field-interact.md §1. / GS | 75 (0) |  |
| player_two/field_pickup.cpp:48 | kRootModeHolder | 0x1b8 | The objects (RE-p2-field-interact.md §1. / read by the mode-class gate (no null check of its  | 30 (0) |  |
| player_two/field_pickup.cpp:49 | kRootActionManager | 0x1a0 | The objects (RE-p2-field-interact.md §1. / FieldActionMgr | 49 (0) |  |
| player_two/field_pickup.cpp:50 | kRootSlotHeldOwner | 0x210 | The objects (RE-p2-field-interact.md §1. / -> +8: a u8 TickSlots writes (§12.1) | 52 (24) | SQEX_Bonamik_Component_v2.bHasSkeletalMeshSkippedTransforms -> 0x208; SQEX_BonamikWind_Component_v2.m_BonamikWindBoxAreaHalfSize -> 0x208; AudioComponent.SoundClassOverride -> 0x208 |
| player_two/field_pickup.cpp:51 | kSlotHeldByte | 0x8 | The objects (RE-p2-field-interact.md §1. /  | 1705 (0) |  |
| player_two/field_pickup.cpp:52 | kRootWorldItems | 0x288 | The objects (RE-p2-field-interact.md §1. / WorldItemMgr | 71 (6) | PrimitiveComponent.VirtualTextureLodBias -> 0x278; BoxReflectionCaptureComponent.PreviewInfluenceBox -> 0x280; PlaneReflectionCaptureComponent.PreviewInfluenceRadius -> 0x280 |
| player_two/field_pickup.cpp:54 | kUIEntries | 0xb8 | The objects (RE-p2-field-interact.md §1. / -> +0x5D0: i32, the poll needs it < 0 | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/field_pickup.cpp:55 | kUIEntryValue | 0x5d0 | The objects (RE-p2-field-interact.md §1. /  | 20 (11) | EndCrashBoxMiniGameElectricCableActor.m_StaticMeshComponent -> 0x5f0; EndFeelerAttendantActor.WalkMoveVelocityInputRange -> 0x5f0; EndFeelerBarrierActor.ExtrusionWaitTimeRange -> 0x5f0 |
| player_two/field_pickup.cpp:59 | kGsBlockA | 0x1a12288 | The input step's gate fields in GS (§12. /  | 0 (0) |  |
| player_two/field_pickup.cpp:60 | kGsBlockB | 0x1a17548 | The input step's gate fields in GS (§12. /  | 0 (0) |  |
| player_two/field_pickup.cpp:61 | kGsAllowA | 0x1a12744 | The input step's gate fields in GS (§12. / + kGsAllowB: a non-zero sum polls despite the bloc | 0 (0) |  |
| player_two/field_pickup.cpp:62 | kGsAllowB | 0x1a12494 | The input step's gate fields in GS (§12. /  | 0 (0) |  |
| player_two/field_pickup.cpp:63 | kGsSlot0A | 0x1a12860 | The input step's gate fields in GS (§12. / + kGsSlot0B: non-zero polls slot 0 only (and despi | 0 (0) |  |
| player_two/field_pickup.cpp:64 | kGsSlot0B | 0x1a125b0 | The input step's gate fields in GS (§12. /  | 0 (0) |  |
| player_two/field_pickup.cpp:65 | kGameFlagBlocksPoll | 0xb | The input step's gate fields in GS (§12. /  | 24 (0) |  |
| player_two/field_pickup.cpp:66 | kModeBlocksPoll | 0x2 | The input step's gate fields in GS (§12. /  | 90 (0) |  |
| player_two/field_pickup.cpp:90 | kCandidateMax | 0x29c | The input step's gate fields in GS (§12. / i32: 64 (the inline storage) | 22 (2) | PrimitiveComponent.BoundsScale -> 0x28c; LightComponent.IESBrightnessScale -> 0x294 |
| player_two/field_pickup.cpp:91 | kTraceMapA | 0x2d4 | The input step's gate fields in GS (§12. / i32: 128 (as the ctor leaves it) | 17 (1) | PlanarReflectionComponent.DistanceFromPlaneFadeoutStart -> 0x2cc |
| player_two/field_pickup.cpp:92 | kTraceMapB | 0x2d8 | The input step's gate fields in GS (§12. / i32: -1 | 50 (4) | EndCinemaWidgetControlComponent.TextFieldsByStoryFlag -> 0x2d0; EndStreamingVolume.DataAssets -> 0x2e8; PlanarReflectionComponent.DistanceFromPlaneFadeoutEnd -> 0x2d0 |
| player_two/field_pickup.cpp:93 | kSlotRows | 0x483a8 | The input step's gate fields in GS (§12. / 8 x FWeakObjectPtr {index, serial} | 0 (0) |  |
| player_two/field_pickup.cpp:94 | kSlotTargets | 0x48608 | The input step's gate fields in GS (§12. / 8 x FName | 0 (0) |  |
| player_two/field_pickup.cpp:95 | kSlotLatches | 0x48730 | The input step's gate fields in GS (§12. / held, pressed, executed: 3 x 8 bytes | 0 (0) |  |
| player_two/field_pickup.cpp:96 | kPendingFlag | 0x487d0 | The input step's gate fields in GS (§12. / u8 | 0 (0) |  |
| player_two/field_pickup.cpp:97 | kReExecuted | 0x487e8 | The input step's gate fields in GS (§12. / u8 | 0 (0) |  |
| player_two/field_pickup.cpp:98 | kFieldEnterFlag | 0x487e9 | The input step's gate fields in GS (§12. / u8, shared: the field-enter reset's own flag | 0 (0) |  |
| player_two/field_pickup.cpp:99 | kPointList | 0x48320 | The input step's gate fields in GS (§12. / the live list's head node; node+0x08 next, point = | 0 (0) |  |
| player_two/field_pickup.cpp:100 | kNodeNext | 0x8 | The input step's gate fields in GS (§12. /  | 1705 (0) |  |
| player_two/field_pickup.cpp:101 | kNodePoint | 0x18 | The input step's gate fields in GS (§12. /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/field_pickup.cpp:102 | kPointLock | 0x487f0 | The input step's gate fields in GS (§12. / SRWLOCK | 0 (0) |  |
| player_two/field_pickup.cpp:107 | kPointKey | 0xc | A point. / FName | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| player_two/field_pickup.cpp:108 | kPointPreset | 0x44 | A point. / FName: the point-side range preset (§10.4; rpaVend | 249 (0) |  |
| player_two/field_pickup.cpp:109 | kPointFlags | 0x104 | A point. / u8 | 35 (0) |  |
| player_two/field_pickup.cpp:110 | kTargetBits | 0x6 | A point. / bit 1 slot target, bit 2 runner-up: carried per pl | 23 (0) |  |
| player_two/field_pickup.cpp:111 | kDisabledBit | 0x10 | A point. / the game's "point disabled": the candidate query s | 1283 (0) |  |
| player_two/field_pickup.cpp:112 | kPointMarker | 0x78 | A point. / UFieldTargetWidget*, the point's marker: carried p | 332 (0) |  |
| player_two/field_pickup.cpp:127 | kRootChests | 0x298 | own test). Offsets of the map's fields,  / TreasurePointMgr (not TreasureManager +0x280, the  | 41 (1) | LightComponent.bUseIESBrightness -> 0x290 |
| player_two/field_pickup.cpp:128 | kMapCount | 0x8 | own test). Offsets of the map's fields,  / i32 ArrayNum | 1705 (0) |  |
| player_two/field_pickup.cpp:129 | kMapBitsInline | 0x10 | own test). Offsets of the map's fields,  /  | 1283 (0) |  |
| player_two/field_pickup.cpp:130 | kMapBitsHeap | 0x20 | own test). Offsets of the map's fields,  /  | 810 (0) |  |
| player_two/field_pickup.cpp:131 | kMapFreeCount | 0x34 | own test). Offsets of the map's fields,  / i32 NumFreeIndices | 329 (0) |  |
| player_two/field_pickup.cpp:132 | kMapHashInline | 0x38 | own test). Offsets of the map's fields,  /  | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| player_two/field_pickup.cpp:133 | kMapHashHeap | 0x40 | own test). Offsets of the map's fields,  /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/field_pickup.cpp:134 | kMapHashSize | 0x48 | own test). Offsets of the map's fields,  / i32 | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| player_two/field_pickup.cpp:135 | kMaxItems | 0x4000 | own test). Offsets of the map's fields,  /  | 0 (0) |  |
| player_two/field_pickup.cpp:185 | kRequestFirst | 0x5 | 5 bytes replaced), then push rbp/rsi/rdi /  | 30 (0) |  |
| player_two/field_pickup.cpp:187 | kObjectOuter | 0x20 | 5 bytes replaced), then push rbp/rsi/rdi /  | 810 (0) |  |
| player_two/field_pickup.cpp:188 | kWorldControllers | 0x1f0 | 5 bytes replaced), then push rbp/rsi/rdi /  | 36 (2) | NiagaraEmitter.bSimulationStagesEnabled -> 0x1dd; NiagaraEmitter.bDeprecatedShaderStagesEnabled -> 0x1dd |
| player_two/field_pickup.cpp:189 | kWorldControllerCount | 0x1f8 | 5 bytes replaced), then push rbp/rsi/rdi /  | 116 (52) | SynthComponent.bAutoDestroy -> 0x1f0; SynthComponent.bStopWhenOwnerDestroyed -> 0x1f0; SynthComponent.bAllowSpatialization -> 0x1f0 |
| player_two/field_pickup.cpp:193 | kMarkerList | 0x98 | 5 bytes replaced), then push rbp/rsi/rdi /  | 306 (0) |  |
| player_two/field_pickup.cpp:194 | kMarkerEntry | 0x8c | 5 bytes replaced), then push rbp/rsi/rdi /  | 115 (0) |  |
| player_two/focus_scope.cpp:18 | kInput | 0x10 | Facts read from the database (RE-p2-comm / EndUISystem[2]: Square's input object | 1283 (0) |  |
| player_two/focus_scope.cpp:19 | kController | 0xb8 | Facts read from the database (RE-p2-comm / EndUISystem[23]: the command controller | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/focus_scope.cpp:20 | kKeyGuide | 0x1a8 | Facts read from the database (RE-p2-comm / EndUISystem[53]: the key guide | 34 (0) |  |
| player_two/focus_scope.cpp:33 | kDelegate | 0x1a0 | menu, RE-p2-command-menu-levers.md §5.2) / the menu-core delegate, run when no widget takes a | 49 (0) |  |
| player_two/focus_scope.cpp:38 | kCaptureFirstFree | 0x30 | menu, RE-p2-command-menu-levers.md §5.2) /  | 1052 (0) |  |
| player_two/focus_scope.cpp:72 | kLayerSize | 0x28 | menu's own layers sit at -1000 (RE-p2-co /  | 889 (0) |  |
| player_two/focus_scope.cpp:73 | kLayerPriority | 0x20 | menu's own layers sit at -1000 (RE-p2-co /  | 810 (0) |  |
| player_two/focus_scope.h:64 | kDelegateSize | 0x10 | object. Game thread only. /  | 1283 (0) |  |
| player_two/framing_watch.cpp:27 | kOpInitialized | 0xd2 |  / u8: its lazy init ran (seeds the level from the op | 5 (0) |  |
| player_two/framing_watch.cpp:28 | kOpLayers | 0x108 |  / 8 setting layers, 0 Debug .. 7 Default | 127 (0) |  |
| player_two/framing_watch.cpp:29 | kLayerStride | 0x98 |  /  | 306 (0) |  |
| player_two/framing_watch.cpp:30 | kLayerCount | 0x8 |  /  | 1705 (0) |  |
| player_two/framing_watch.cpp:31 | kLayerWeight | 0x30 |  / float | 1052 (0) |  |
| player_two/framing_watch.cpp:32 | kLayerActive | 0x35 |  / u8 | 18 (0) |  |
| player_two/framing_watch.cpp:33 | kLayerRow | 0x68 |  / FName: the CameraSetting row | 614 (1) | NiagaraEmitter.UpdateScriptProps -> 0x60 |
| player_two/framing_watch.cpp:34 | kOpStateFov | 0x600 |  / the blended state: FOV, Distance, Height | 7 (2) | EndFeelerBarrierActor.SkeletalMesh -> 0x620; EndFeelerSplineActor.CenterAnimatedOffsetCurve -> 0x620 |
| player_two/framing_watch.cpp:35 | kOpStateAim | 0x620 |  / AimOffset X, Y, Z | 9 (8) | SynthComponent.SoundSubmixSends -> 0x618; AudioComponent.ConcurrencySet -> 0x618; EndFeelerBarrierActor.AnimationVariationCount -> 0x640 |
| player_two/framing_watch.cpp:36 | kOpPosePitch | 0x50 |  /  | 488 (0) |  |
| player_two/framing_watch.cpp:37 | kOpPoseFov | 0x5c |  /  | 156 (1) | NiagaraEmitter.RandomSeed -> 0x54 |
| player_two/framing_watch.cpp:38 | kOpPoseDistance | 0x80 |  /  | 393 (0) |  |
| player_two/framing_watch.cpp:39 | kOpPoseHeight | 0x94 |  /  | 91 (0) |  |
| player_two/framing_watch.cpp:40 | kOpPoseAim | 0x84 |  / the pose's AimOffset X, Y (lateral: the shoulder), | 145 (0) |  |
| player_two/framing_watch.cpp:41 | kOpModules | 0x6e0 |  / module table, 16 bytes a slot (module pointer firs | 9 (3) | EndFeelerSplineActor.MasterSkeletalMeshComponents -> 0x700; EndFeelerTornadeActor.MasterSkeletalMeshComponents -> 0x700; ParticleSystemComponent.WarmupTickRate -> 0x6d0 |
| player_two/framing_watch.cpp:42 | kModuleSlots | 0x19 |  /  | 59 (0) |  |
| player_two/framing_watch.cpp:43 | kModuleStride | 0x10 |  /  | 1283 (0) |  |
| player_two/framing_watch.cpp:44 | kModuleActive | 0x44 |  / u8 | 249 (0) |  |
| player_two/framing_watch.cpp:46 | kMinChangeGapMs | 0xfa |  /  | 4 (0) |  |
| player_two/framing_watch.cpp:47 | kOpLevel | 0x738 |  / u8: the Camera Distance level | 28 (3) | SkinnedMeshComponent.PhysicsAssetOverride -> 0x700; NiagaraComponent.bForceSolo -> 0x728; NiagaraComponent.bEnableGpuComputeDebug -> 0x728 |
| player_two/framing_watch.cpp:48 | kOpLevelBlending | 0x744 |  / u8 | 8 (3) | SkinnedMeshComponent.MinLodModel -> 0x730; EndBikeItemActor.AddATBGaugeTime -> 0x764; EndWorldReportActor.CoordinateAnimationDuration -> 0x764 |
| player_two/framing_watch.cpp:49 | kOpPitchMin | 0x748 |  /  | 30 (8) | EndBikeItemActor.bScoreItem -> 0x768; EndCardGame3DToken.bPlayer -> 0x768; EndCatchableRopeSkeletalMeshActor.RopeLength -> 0x768 |
| player_two/framing_watch.cpp:50 | kOpPitchMax | 0x74c |  /  | 8 (1) | EndCardGame3DToken.LaneIndex -> 0x76c |
| player_two/framing_watch.cpp:66 | kHeartbeatMs | 0x1d4c0 | float read with default 0 (rcx unused).  /  | 0 (0) |  |
| player_two/handover_watch.cpp:40 | kAiDefaultState | 0x340 | The AI controller's brain stack and stat / u8, EEndAIState | 23 (1) | EndFieldClimbingFinishPointSetting.CachedEdgeTransform -> 0x330 |
| player_two/handover_watch.cpp:41 | kAiPauseMask | 0x460 | The AI controller's brain stack and stat / u32 | 19 (0) |  |
| player_two/handover_watch.cpp:42 | kAiBrains | 0x478 | The AI controller's brain stack and stat / 20 pointers; index 0 is the base brain | 18 (1) | PrimitiveComponent.LODParentPrimitive -> 0x468 |
| player_two/handover_watch.cpp:44 | kAiActiveBrain | 0x518 | The AI controller's brain stack and stat /  | 27 (13) | SkinnedMeshComponent.SkeletalMesh -> 0x508; ProceduralMeshComponent.bUseComplexAsSimpleCollision -> 0x508; PaperFlipbookComponent.Material -> 0x508 |
| player_two/handover_watch.cpp:45 | kAiActiveIndex | 0x520 | The AI controller's brain stack and stat / u8 (21: none) | 34 (13) | SkinnedMeshComponent.MasterPoseComponent -> 0x510; ProceduralMeshComponent.ProcMeshBodySetup -> 0x510; PaperFlipbookComponent.PlayRate -> 0x510 |
| player_two/handover_watch.cpp:46 | kBrainMode | 0x18 | The AI controller's brain stack and stat / u8 | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/handover_watch.cpp:47 | kBrainIndex | 0x19 | The AI controller's brain stack and stat / u8; 0 is the base brain | 59 (0) |  |
| player_two/handover_watch.cpp:48 | kBrainState | 0x20 | The AI controller's brain stack and stat /  | 810 (0) |  |
| player_two/handover_watch.cpp:49 | kStateType | 0x18 | The AI controller's brain stack and stat / u8, EEndAIState | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/handover_watch.cpp:52 | kFollowRate | 0x6d0 | The states' catch-up fields (§1.2, §1.3) / float | 11 (6) | EndFeelerConflictActor.EmissiveColorSettingsB -> 0x6f0; EndFeelerSplineActor.CullingDeltaHeightMax -> 0x6f0; EndFeelerTornadeActor.bCullingBackSide -> 0x6f0 |
| player_two/handover_watch.cpp:53 | kFollowMode | 0x39e | The states' catch-up fields (§1.2, §1.3) / u8 | 1 (1) | EndBattleSummonTargetPoint.bWarp_Ifrit -> 0x3be |
| player_two/handover_watch.cpp:54 | kLeadTarget | 0x3b0 | The states' catch-up fields (§1.2, §1.3) / FWeakObjectPtr | 21 (4) | EndBattleSummonTargetPoint.bWarp_End2Hinachocobo -> 0x3d0; EndNavSwitchingVolume.bRefreshNav -> 0x3d0; EndNavModifierVolume.Wall -> 0x3d0 |
| player_two/handover_watch.cpp:55 | kLeadArrived | 0x3c1 | The states' catch-up fields (§1.2, §1.3) / u8: within 8 m of the destination | 1 (0) |  |
| player_two/handover_watch.cpp:56 | kLeadDestination | 0x3e8 | The states' catch-up fields (§1.2, §1.3) / three floats | 88 (4) | EndEnvironmentPhysicsStaticMeshActor.OtherStaticMeshComponents -> 0x408; EndBattleSummonTargetPoint.bAppearPointTheGround -> 0x408; EndNavBlockingModifierVolume.CanEverAerial -> 0x408 |
| player_two/handover_watch.cpp:60 | kCharacterMove | 0x1400 | weak pointer at +0x160 of it (§1.1). /  | 0 (0) |  |
| player_two/handover_watch.cpp:61 | kCharacterFlags | 0x1408 | weak pointer at +0x160 of it (§1.1). / u8; bit 2 set by LEAD's and FOLLOW's Enter | 1 (0) |  |
| player_two/handover_watch.cpp:62 | kMoveAnimInstance | 0x160 | weak pointer at +0x160 of it (§1.1). /  | 68 (0) |  |
| player_two/handover_watch.cpp:67 | kAnimRotationScale | 0x550 | pointer at +0x20; 16-byte elements, key  /  | 27 (11) | PaperTileMapComponent.UseSingleLayerIndex -> 0x540; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyDrag -> 0x540; EndFeelerAttendantActor.PitchRange -> 0x570 |
| player_two/handover_watch.cpp:68 | kAnimRotationMap | 0x558 | pointer at +0x20; 16-byte elements, key  /  | 18 (8) | PaperTileMapComponent.TileMap -> 0x548; SimpleDynamicMeshComponent.TangentsType -> 0x548; EndBonamikBoxScaleGroupParamsComponent.m_ChangeBodyLift -> 0x548 |
| player_two/handover_watch.cpp:69 | kAnimRootMotionScale | 0x5a8 | pointer at +0x20; 16-byte elements, key  /  | 20 (7) | EndFeelerAttendantActor.IdleToWalkMoveDelayTimeRange -> 0x5c8; EndFeelerFollowingActor.PrimaryRollSize -> 0x5c8; EndFeelerLaserActor.SecondaryRollLoopTime -> 0x5c8 |
| player_two/handover_watch.cpp:70 | kAnimRootMotionMap | 0x5b0 | pointer at +0x20; 16-byte elements, key  /  | 17 (8) | SynthComponent.ConcurrencySettings -> 0x5a8; EndBikeSplineComponent.EnemySlideMoveLimitWidth -> 0x5a0; EndFeelerAttendantActor.WalkToIdleStateDelayTimeRange -> 0x5d0 |
| player_two/handover_watch.cpp:71 | kAnimSpeedScale | 0x10cc | pointer at +0x20; 16-byte elements, key  / float, for comparison | 0 (0) |  |
| player_two/handover_watch.cpp:72 | kAnimFlags | 0x11dc | pointer at +0x20; 16-byte elements, key  / u32; bit 0 set by LEAD's Enter | 0 (0) |  |
| player_two/handover_watch.cpp:73 | kMapCount | 0x8 | pointer at +0x20; 16-byte elements, key  /  | 1705 (0) |  |
| player_two/handover_watch.cpp:74 | kMapInlineBits | 0x10 | pointer at +0x20; 16-byte elements, key  /  | 1283 (0) |  |
| player_two/handover_watch.cpp:75 | kMapHeapBits | 0x20 | pointer at +0x20; 16-byte elements, key  /  | 810 (0) |  |
| player_two/handover_watch.cpp:76 | kMapElement | 0x10 | pointer at +0x20; 16-byte elements, key  /  | 1283 (0) |  |
| player_two/handover_watch.cpp:77 | kMapShown | 0x20 | pointer at +0x20; 16-byte elements, key  /  | 810 (0) |  |
| player_two/handover_watch.cpp:79 | kStateDoNothing | 0x1 | pointer at +0x20; 16-byte elements, key  /  | 155 (0) |  |
| player_two/handover_watch.cpp:80 | kStateBattle | 0x3 | pointer at +0x20; 16-byte elements, key  /  | 42 (0) |  |
| player_two/handover_watch.cpp:81 | kStateStay | 0x4 | pointer at +0x20; 16-byte elements, key  /  | 542 (0) |  |
| player_two/handover_watch.cpp:82 | kStateLead | 0x6 | pointer at +0x20; 16-byte elements, key  /  | 23 (0) |  |
| player_two/handover_watch.cpp:83 | kStateFollow | 0x7 | pointer at +0x20; 16-byte elements, key  /  | 18 (0) |  |
| player_two/handover_watch.cpp:84 | kUnread | 0xff | pointer at +0x20; 16-byte elements, key  /  | 2 (0) |  |
| player_two/handover_watch.cpp:87 | kComponentWorldLocation | 0x1d0 | USceneComponent's world location (as nav /  | 33 (0) |  |
| player_two/interact_watch.cpp:41 | kRootActionManager | 0x1a0 | The objects (RE-p2-field-interact.md §1. / FieldActionMgr | 49 (0) |  |
| player_two/interact_watch.cpp:42 | kRootWorldItems | 0x288 | The objects (RE-p2-field-interact.md §1. / WorldItemMgr | 71 (6) | PrimitiveComponent.VirtualTextureLodBias -> 0x278; BoxReflectionCaptureComponent.PreviewInfluenceBox -> 0x280; PlaneReflectionCaptureComponent.PreviewInfluenceRadius -> 0x280 |
| player_two/interact_watch.cpp:44 | kPartyController | 0x140 | The objects (RE-p2-field-interact.md §1. / FWeakObjectPtr: the primary controller | 102 (1) | NiagaraEmitter.RendererBindings -> 0x128 |
| player_two/interact_watch.cpp:45 | kPartyLeader | 0x148 | The objects (RE-p2-field-interact.md §1. / FWeakObjectPtr: the leader | 92 (0) |  |
| player_two/interact_watch.cpp:46 | kPartyLeaderType | 0x150 | The objects (RE-p2-field-interact.md §1. / u8 | 90 (0) |  |
| player_two/interact_watch.cpp:49 | kOctree | 0x10 | FieldActionMgr (§1.2, §1.3, §10.D). /  | 1283 (0) |  |
| player_two/interact_watch.cpp:50 | kCandidateCount | 0x298 | FieldActionMgr (§1.2, §1.3, §10.D). / i32 | 41 (1) | LightComponent.bUseIESBrightness -> 0x290 |
| player_two/interact_watch.cpp:51 | kPointList | 0x48320 | FieldActionMgr (§1.2, §1.3, §10.D). / the live list's head node; node+0x08 next, point = | 0 (0) |  |
| player_two/interact_watch.cpp:52 | kNodeNext | 0x8 | FieldActionMgr (§1.2, §1.3, §10.D). /  | 1705 (0) |  |
| player_two/interact_watch.cpp:53 | kNodePoint | 0x18 | FieldActionMgr (§1.2, §1.3, §10.D). /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/interact_watch.cpp:54 | kPointLock | 0x487f0 | FieldActionMgr (§1.2, §1.3, §10.D). / SRWLOCK | 0 (0) |  |
| player_two/interact_watch.cpp:55 | kSlotRows | 0x483a8 | FieldActionMgr (§1.2, §1.3, §10.D). / 8 x FWeakObjectPtr | 0 (0) |  |
| player_two/interact_watch.cpp:56 | kSlotRecords | 0x484a8 | FieldActionMgr (§1.2, §1.3, §10.D). / 8 x 36 bytes: +0 key, +8 owner, +0x18 hold timer | 0 (0) |  |
| player_two/interact_watch.cpp:57 | kSlotRecordSize | 0x24 | FieldActionMgr (§1.2, §1.3, §10.D). /  | 305 (0) |  |
| player_two/interact_watch.cpp:58 | kSlotTargets | 0x48608 | FieldActionMgr (§1.2, §1.3, §10.D). / 8 x FName: this frame's target keys | 0 (0) |  |
| player_two/interact_watch.cpp:59 | kSlotHeld | 0x48730 | FieldActionMgr (§1.2, §1.3, §10.D). / 8 x u8 | 0 (0) |  |
| player_two/interact_watch.cpp:60 | kSlotPressed | 0x48738 | FieldActionMgr (§1.2, §1.3, §10.D). / 8 x u8 | 0 (0) |  |
| player_two/interact_watch.cpp:61 | kSlotExecuted | 0x48740 | FieldActionMgr (§1.2, §1.3, §10.D). / 8 x u8 | 0 (0) |  |
| player_two/interact_watch.cpp:62 | kPending | 0x487d0 | FieldActionMgr (§1.2, §1.3, §10.D). / u8 | 0 (0) |  |
| player_two/interact_watch.cpp:67 | kPointTalk | 0x8 | A point (§1.2 as corrected in §10.B). / u8 | 1705 (0) |  |
| player_two/interact_watch.cpp:68 | kPointKey | 0xc | A point (§1.2 as corrected in §10.B). / FName | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| player_two/interact_watch.cpp:69 | kPointOwner | 0x1c | A point (§1.2 as corrected in §10.B). / FName | 361 (0) |  |
| player_two/interact_watch.cpp:70 | kPointLocation | 0x2c | A point (§1.2 as corrected in §10.B). / FVector | 299 (0) |  |
| player_two/interact_watch.cpp:71 | kPointPreset | 0x44 | A point (§1.2 as corrected in §10.B). / FName rpa<Owner> | 249 (0) |  |
| player_two/interact_watch.cpp:72 | kPointPreset2 | 0x4c | A point (§1.2 as corrected in §10.B). / FName, a second point-side preset | 244 (0) |  |
| player_two/interact_watch.cpp:73 | kPointPresetPlayer | 0x54 | A point (§1.2 as corrected in §10.B). / FName rpa<Owner>Player | 256 (0) |  |
| player_two/interact_watch.cpp:74 | kPointSlot | 0xe8 | A point (§1.2 as corrected in §10.B). / u8 | 156 (0) |  |
| player_two/interact_watch.cpp:75 | kPointKind | 0xf0 | A point (§1.2 as corrected in §10.B). / u8 | 135 (0) |  |
| player_two/interact_watch.cpp:76 | kPointVariant | 0xf1 | A point (§1.2 as corrected in §10.B). / u8 | 2 (0) |  |
| player_two/interact_watch.cpp:77 | kPointFlags | 0x104 | A point (§1.2 as corrected in §10.B). / 4 bytes: bit 1 slot target, 2 runner-up, 4 skipped | 35 (0) |  |
| player_two/interact_watch.cpp:80 | kItemElements | 0x90 | A point (§1.2 as corrected in §10.B). /  | 332 (1) | NiagaraEmitter.SpawnScriptProps -> 0x88 |
| player_two/interact_watch.cpp:81 | kItemCount | 0x98 | A point (§1.2 as corrected in §10.B). / i32 | 306 (0) |  |
| player_two/interact_watch.cpp:82 | kItemBitsInline | 0xa0 | A point (§1.2 as corrected in §10.B). /  | 264 (0) |  |
| player_two/interact_watch.cpp:83 | kItemBitsHeap | 0xb0 | A point (§1.2 as corrected in §10.B). /  | 301 (0) |  |
| player_two/interact_watch.cpp:84 | kItemStride | 0xd0 | A point (§1.2 as corrected in §10.B). /  | 205 (0) |  |
| player_two/interact_watch.cpp:85 | kMaxItems | 0x4000 | A point (§1.2 as corrected in §10.B). /  | 0 (0) |  |
| player_two/interact_watch.cpp:86 | kEntryKeyBack | 0x10 | A point (§1.2 as corrected in §10.B). / AcquireEntry's entry = element + 0x10 | 1283 (0) |  |
| player_two/interact_watch.cpp:98 | kFirstInstruction | 0x5 | listed bytes are checked too. /  | 30 (0) |  |
| player_two/leader_scope.cpp:15 | kPartyManagerLeader | 0x148 |  /  | 92 (0) |  |
| player_two/leader_scope.cpp:16 | kPartyManagerLeaderType | 0x150 |  / u8 | 90 (0) |  |
| player_two/leave_sequence.h:33 | kReleaseTicks | 0x78 | may run between destroying that actor an /  | 332 (0) |  |
| player_two/location_rows.cpp:38 | kListenerOperator | 0x8 | op = *(listener+8); enter -> op vt+0xE0  /  | 1705 (0) |  |
| player_two/location_rows.cpp:44 | kNavAreaController | 0xb0 | op = *(listener+8); enter -> op vt+0xE0  /  | 301 (0) |  |
| player_two/location_rows.cpp:45 | kBattleOperator | 0x2ca8 | op = *(listener+8); enter -> op vt+0xE0  /  | 0 (0) |  |
| player_two/location_rows.cpp:52 | kManagerVolumes | 0x8 | op = *(listener+8); enter -> op vt+0xE0  /  | 1705 (0) |  |
| player_two/location_rows.cpp:53 | kManagerVolumeCount | 0x10 | op = *(listener+8); enter -> op vt+0xE0  /  | 1283 (0) |  |
| player_two/location_rows.cpp:54 | kManagerLock | 0x188 | op = *(listener+8); enter -> op vt+0xE0  /  | 48 (0) |  |
| player_two/location_rows.cpp:55 | kVolumeLocationId | 0x270 | op = *(listener+8); enter -> op vt+0xE0  /  | 67 (6) | PrimitiveComponent.TranslucencySortOffset -> 0x230; AudioComponent.AttenuationOverrides -> 0x268; NiagaraEmitter.m_VfxRandomSeed_EveryFrame_Params -> 0x258 |
| player_two/location_rows.cpp:56 | kVolumeUntouchMargin | 0x278 | op = *(listener+8); enter -> op vt+0xE0  / float: the margin the game's test stays inside by | 62 (5) | PrimitiveComponent.RuntimeVirtualTextures -> 0x268; LightComponent.LightFunctionMaterial -> 0x270; ExponentialHeightFogComponent.Medium -> 0x270 |
| player_two/location_rows.cpp:57 | kVolumeFlags | 0x280 | op = *(listener+8); enter -> op vt+0xE0  / bit 1 bDisabled, bit 8 bChangeCamera | 76 (6) | SQEX_KineDriver_Component.ReplaceKDIs -> 0x278; BoxReflectionCaptureComponent.BoxTransitionDistance -> 0x278; LightComponent.LightFunctionScale -> 0x278 |
| player_two/location_rows.cpp:58 | kVolumeCameraRow | 0x2e0 | op = *(listener+8); enter -> op vt+0xE0  / FName CameraSettingID | 36 (2) | PrimitiveComponent.BodyInstance -> 0x2d0; PlanarReflectionComponent.AngleFromPlaneFadeEnd -> 0x2d8 |
| player_two/location_rows.cpp:59 | kVolumeCameraPriority | 0x2e8 | op = *(listener+8); enter -> op vt+0xE0  / lower = higher priority | 34 (1) | EndStreamingVolume.VolumeLabel -> 0x2f8 |
| player_two/location_rows.cpp:61 | kMaxVolumes | 0x1000 | op = *(listener+8); enter -> op vt+0xE0  /  | 2 (0) |  |
| player_two/location_rows.cpp:65 | kOpSetLocation | 0xe0 | op = *(listener+8); enter -> op vt+0xE0  / op vtable: SetLocationSetting(op, location, row) | 160 (1) | NiagaraEmitter.GlobalSpawnCountScaleOverrides -> 0xd4 |
| player_two/location_rows.cpp:66 | kOpClearLocation | 0xe8 | op = *(listener+8); enter -> op vt+0xE0  / op vtable: ClearLocationSetting(op, location, row) | 156 (0) |  |
| player_two/lock_on.cpp:89 | kCursor | 0x68 | The battle system (a pointer global) and / the target cursor widget | 614 (1) | NiagaraEmitter.UpdateScriptProps -> 0x60 |
| player_two/lock_on.cpp:90 | kDisplayed | 0x70 | The battle system (a pointer global) and / TWeakPtr {object, reference controller} | 515 (0) |  |
| player_two/lock_on.cpp:91 | kTrace | 0x80 | The battle system (a pointer global) and / the cursor's async trace handle | 393 (0) |  |
| player_two/lock_on.cpp:92 | kLock | 0x8c | The battle system (a pointer global) and /  | 115 (0) |  |
| player_two/lock_on.cpp:93 | kLatch | 0x8d | The battle system (a pointer global) and /  | 9 (0) |  |
| player_two/lock_on.cpp:94 | kFreeTime | 0x90 | The battle system (a pointer global) and / float: the lock camera's rest after a stick move | 332 (1) | NiagaraEmitter.SpawnScriptProps -> 0x88 |
| player_two/lock_on.cpp:98 | kBarManager | 0xf8 | the displayed target (a TWeakPtr). /  | 149 (1) | NiagaraEmitter.Platforms -> 0xe8 |
| player_two/lock_on.cpp:99 | kTargetedKey | 0x140 | the displayed target (a TWeakPtr). /  | 102 (1) | NiagaraEmitter.RendererBindings -> 0x128 |
| player_two/lock_on.cpp:102 | kLockOnLayer | 0x7 | byte and cached target (logged). /  | 18 (0) |  |
| player_two/lock_on.cpp:103 | kBattleOperator | 0x2ca8 | byte and cached target (logged). /  | 0 (0) |  |
| player_two/lock_on.cpp:104 | kModules | 0x6e0 | byte and cached target (logged). /  | 9 (3) | EndFeelerSplineActor.MasterSkeletalMeshComponents -> 0x700; EndFeelerTornadeActor.MasterSkeletalMeshComponents -> 0x700; ParticleSystemComponent.WarmupTickRate -> 0x6d0 |
| player_two/lock_on.cpp:105 | kModuleEntry | 0x10 | byte and cached target (logged). /  | 1283 (0) |  |
| player_two/lock_on.cpp:106 | kModuleActive | 0x44 | byte and cached target (logged). /  | 249 (0) |  |
| player_two/lock_on.cpp:107 | kModuleTarget | 0x3f8 | byte and cached target (logged). /  | 73 (4) | EndEnvironmentPhysicsStaticMeshActor.BoundsShapeSize -> 0x418; EndBattleLevelSequenceActor.CameraActor -> 0x418; EndNavWheelieBlockWallVolume.TriggerName -> 0x418 |
| player_two/main_camera_shape.cpp:38 | kCameraComponentOffset | 0x230 | the bit its constructor sets and the Blu /  | 160 (17) | PrimitiveComponent.bHasCustomNavigableGeometry -> 0x228; SQEX_BonamikWind_Component_v2.m_BonamikWind -> 0x228; AudioComponent.VolumeModulationMin -> 0x228 |
| player_two/main_camera_shape.cpp:39 | kConstrainAspectRatioByte | 0x20c | the bit its constructor sets and the Blu /  | 24 (15) | SQEXSEADWhizSeComponent.ApproachJudgmentVelocity -> 0x204; CameraComponent.bConstrainAspectRatio -> 0x204; CameraComponent.bUseFieldOfViewForLOD -> 0x204 |
| player_two/main_camera_shape.cpp:61 | kSetterFrameBytes | 0x10 | the bit its constructor sets and the Blu /  | 1283 (0) |  |
| player_two/main_camera_shape.cpp:62 | kMaxChanged | 0x4 | the bit its constructor sets and the Blu / two players, and a respawned camera each | 542 (0) |  |
| player_two/menu_focus.cpp:21 | kInput | 0x10 | Facts read from the database (RE-p2-comm / EndUISystem[2]: Square's input object | 1283 (0) |  |
| player_two/menu_focus.cpp:22 | kController | 0xb8 | Facts read from the database (RE-p2-comm / EndUISystem[23]: the command controller | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/menu_focus.cpp:25 | kPath | 0x8 | Facts read from the database (RE-p2-comm /  | 1705 (0) |  |
| player_two/menu_focus.cpp:26 | kLayers | 0x40 | Facts read from the database (RE-p2-comm /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/menu_focus.cpp:27 | kLayerSize | 0x28 | Facts read from the database (RE-p2-comm /  | 889 (0) |  |
| player_two/menu_focus.cpp:28 | kLayerSaved | 0x10 | Facts read from the database (RE-p2-comm /  | 1283 (0) |  |
| player_two/menu_focus.cpp:29 | kLayerPrio | 0x20 | Facts read from the database (RE-p2-comm /  | 810 (0) |  |
| player_two/menu_focus.cpp:30 | kFocusCause | 0x78 | Facts read from the database (RE-p2-comm /  | 332 (0) |  |
| player_two/menu_focus.cpp:33 | kWidgetType | 0x220 | UObject's class and name. /  | 46 (21) | PrimitiveComponent.CachedMaxDrawDistance -> 0x218; SQEX_Bonamik_Component_v2.SkeletalMeshComponent -> 0x218; SQEX_BonamikWind_Component_v2.m_BonamikWindAreaMargin -> 0x218 |
| player_two/menu_focus.cpp:34 | kUserWidget | 0x3a8 | UObject's class and name. /  | 27 (7) | EndBattleSummonTargetPoint.bWarp_Titan -> 0x3c8; EndNavSwitchingVolume.NavModifierVolume -> 0x3c8; EndNavModifierAdditionalLayerVolume.LayerType -> 0x3c8 |
| player_two/menu_focus.cpp:35 | kClass | 0x10 | UObject's class and name. /  | 1283 (0) |  |
| player_two/menu_focus.cpp:36 | kName | 0x18 | UObject's class and name. /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/menu_focus.cpp:41 | kMaxPath | 0x10 | suspended (+0x777), menu focus layer (+0 /  | 1283 (0) |  |
| player_two/menu_focus.cpp:42 | kMaxLayers | 0x8 | suspended (+0x777), menu focus layer (+0 /  | 1705 (0) |  |
| player_two/move_state.cpp:65 | kSampleEvery | 0x1e | Game thread only. /  | 20 (0) |  |
| player_two/nav_strand.cpp:86 | kCharacterMoveOwner | 0x13f8 | safe-transform park (read live: 0x20 on  /  | 0 (0) |  |
| player_two/nav_strand.cpp:87 | kOwnerStopMask | 0x1bc | safe-transform park (read live: 0x20 on  /  | 10 (0) |  |
| player_two/nav_strand.cpp:91 | kComponentWorldLocation | 0x1d0 | safe-transform park (read live: 0x20 on  /  | 33 (0) |  |
| player_two/nav_strand.cpp:93 | kComponentAttachParent | 0xc0 | safe-transform park (read live: 0x20 on  /  | 255 (0) |  |
| player_two/nav_strand.cpp:97 | kNavStreamLeaderCell | 0x8 | safe-transform park (read live: 0x20 on  /  | 1705 (0) |  |
| player_two/nav_strand.cpp:100 | kAiBrain | 0x518 | The retained AI's current state (RE-p2-p /  | 27 (13) | SkinnedMeshComponent.SkeletalMesh -> 0x508; ProceduralMeshComponent.bUseComplexAsSimpleCollision -> 0x508; PaperFlipbookComponent.Material -> 0x508 |
| player_two/nav_strand.cpp:101 | kAiPawn | 0x258 | The retained AI's current state (RE-p2-p /  | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| player_two/nav_strand.cpp:102 | kAiPauseMask | 0x460 | The retained AI's current state (RE-p2-p /  | 19 (0) |  |
| player_two/nav_strand.cpp:103 | kBrainState | 0x20 | The retained AI's current state (RE-p2-p /  | 810 (0) |  |
| player_two/nav_strand.cpp:104 | kStateType | 0x18 | The retained AI's current state (RE-p2-p /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/nav_strand.cpp:105 | kStateTeleportRange | 0x344 | The retained AI's current state (RE-p2-p /  | 9 (1) | EndStreamingSwitchVolume.NoneSwitchLabel -> 0x354 |
| player_two/nav_strand.cpp:106 | kStateTeleporting | 0x39d | The retained AI's current state (RE-p2-p /  | 1 (1) | EndBattleSummonTargetPoint.bWarp_All -> 0x3bd |
| player_two/nav_strand.cpp:107 | kStateFollow | 0x7 | The retained AI's current state (RE-p2-p /  | 18 (0) |  |
| player_two/pad_buttons.cpp:57 | kActionEventKey | 0x18 | the game's. / the action event's key (FName) | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/pad_buttons.cpp:74 | kTableNum | 0x8 | allocation bits (4 inline dwords, a heap /  | 1705 (0) |  |
| player_two/pad_buttons.cpp:75 | kTableInlineBits | 0x10 | allocation bits (4 inline dwords, a heap /  | 1283 (0) |  |
| player_two/pad_buttons.cpp:76 | kTableHeapBits | 0x20 | allocation bits (4 inline dwords, a heap /  | 810 (0) |  |
| player_two/pad_buttons.cpp:77 | kTableMaxBits | 0x2c | allocation bits (4 inline dwords, a heap /  | 299 (0) |  |
| player_two/pad_buttons.cpp:78 | kInlineBitCount | 0x80 | allocation bits (4 inline dwords, a heap /  | 393 (0) |  |
| player_two/pad_buttons.cpp:79 | kMaxElements | 0x400 | allocation bits (4 inline dwords, a heap /  | 69 (2) | EndEnvironmentPhysicsStaticMeshActor.BoundsShapeLocation -> 0x420; EndNavWheelieBlockWallVolume.TriggerInterval -> 0x420 |
| player_two/pad_rumble.cpp:37 | kNoteLastDeviceFirst | 0x5 | mov [rsp+8], rbx (the 5 bytes replaced), /  | 30 (0) |  |
| player_two/pad_rumble.cpp:39 | kOwnerPad | 0x74 | mov [rsp+8], rbx (the 5 bytes replaced), / i32: the last XInput pad | 170 (0) |  |
| player_two/pad_rumble.cpp:41 | kAppDeviceCode | 0x1e0 | mov [rsp+8], rbx (the 5 bytes replaced), / i32: the device code PC1's force feedback goes to | 48 (6) | NiagaraEmitter.bFixedBounds -> 0x1dc; NiagaraEmitter.bUseMinDetailLevel -> 0x1dc; NiagaraEmitter.bUseMaxDetailLevel -> 0x1dc |
| player_two/pad_rumble.cpp:43 | kTickPrerequisites | 0x40 | mov [rsp+8], rbx (the 5 bytes replaced), /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/pad_sticks.cpp:40 | kOverridePads | 0x4 | Per controller id, set by control script /  | 542 (0) |  |
| player_two/panel_gate_code.h:37 | kPanelGateCodeSize | 0x28 | tests can execute exactly these bytes. /  | 889 (0) |  |
| player_two/party_lease.h:69 | kAiStateBattle | 0x3 | ---- the member's AI at the hand-over (R /  | 42 (0) |  |
| player_two/party_lease.h:70 | kAiStateLead | 0x6 | ---- the member's AI at the hand-over (R /  | 23 (0) |  |
| player_two/party_lease.h:71 | kAiStateFollow | 0x7 | ---- the member's AI at the hand-over (R /  | 18 (0) |  |
| player_two/party_warp.cpp:21 | kTeleportQuerySlot | 0x58 | driven only if its vtable is a checked o /  | 416 (1) | NiagaraEmitter.bLocalSpace -> 0x51 |
| player_two/party_warp.cpp:22 | kForceTeleportSlot | 0x270 | driven only if its vtable is a checked o /  | 67 (6) | PrimitiveComponent.TranslucencySortOffset -> 0x230; AudioComponent.AttenuationOverrides -> 0x268; NiagaraEmitter.m_VfxRandomSeed_EveryFrame_Params -> 0x258 |
| player_two/party_warp.cpp:23 | kFinishNowSlot | 0x278 | driven only if its vtable is a checked o /  | 62 (5) | PrimitiveComponent.RuntimeVirtualTextures -> 0x268; LightComponent.LightFunctionMaterial -> 0x270; ExponentialHeightFogComponent.Medium -> 0x270 |
| player_two/party_warp.cpp:24 | kUpdateTeleportSlot | 0x298 | driven only if its vtable is a checked o /  | 41 (1) | LightComponent.bUseIESBrightness -> 0x290 |
| player_two/party_warp.cpp:51 | kAiBrain | 0x518 | driven only if its vtable is a checked o /  | 27 (13) | SkinnedMeshComponent.SkeletalMesh -> 0x508; ProceduralMeshComponent.bUseComplexAsSimpleCollision -> 0x508; PaperFlipbookComponent.Material -> 0x508 |
| player_two/party_warp.cpp:52 | kAiSkipsUpdate | 0xad8 | driven only if its vtable is a checked o / the PLAYER update returns at once while set | 2 (1) | SkeletalMeshComponent.BodySetup -> 0xac8 |
| player_two/party_warp.cpp:53 | kBrainState | 0x20 | driven only if its vtable is a checked o /  | 810 (0) |  |
| player_two/party_warp.cpp:54 | kStateType | 0x18 | driven only if its vtable is a checked o /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/party_warp.cpp:55 | kStateForced | 0x39d | driven only if its vtable is a checked o / forced teleport pending; cleared by the commit | 1 (1) | EndBattleSummonTargetPoint.bWarp_All -> 0x3bd |
| player_two/party_warp.cpp:56 | kCharacterWarping | 0xb59 | driven only if its vtable is a checked o / a warp request is queued on the character | 0 (0) |  |
| player_two/party_warp.cpp:57 | kStateFollow | 0x7 | driven only if its vtable is a checked o /  | 18 (0) |  |
| player_two/party_warp.cpp:59 | kStateQueryResult | 0x540 | driven only if its vtable is a checked o /  | 25 (13) | PaperFlipbookComponent.CachedBodySetup -> 0x530; PaperTileMapComponent.TileMapColor -> 0x530; EndBonamikBoxScaleGroupParamsComponent.m_ChangeChainRootRotDrag -> 0x530 |
| player_two/party_warp.cpp:60 | kQueryStatus | 0x28 | driven only if its vtable is a checked o /  | 889 (0) |  |
| player_two/ride_probe.cpp:30 | kActiveVehicle | 0x5b8 | RE-p2-chocobo-sprint.md §1, §5 (this bui / AEndPlayerController::ActiveVehicleCoordinator | 19 (8) | SynthComponent.ConcurrencySet -> 0x5b0; EndBikeSplineComponent.BuggyEnableInputSlideLength -> 0x5a8; EndFeelerAttendantActor.WalkToIdleMoveDelayTimeRange -> 0x5d8 |
| player_two/ride_probe.cpp:31 | kCoordBody | 0x30 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 1052 (0) |  |
| player_two/ride_probe.cpp:32 | kGait | 0x1a20 | RE-p2-chocobo-sprint.md §1, §5 (this bui / u8: 0 walk, 1 run, 2 dash | 0 (0) |  |
| player_two/ride_probe.cpp:33 | kGaitSpeed | 0x1a18 | RE-p2-chocobo-sprint.md §1, §5 (this bui / float, approaches the gait's TargetSpeed | 0 (0) |  |
| player_two/ride_probe.cpp:34 | kSpeedRatio | 0x1928 | RE-p2-chocobo-sprint.md §1, §5 (this bui / float | 1 (0) |  |
| player_two/ride_probe.cpp:35 | kAppliedLevel | 0x1910 | RE-p2-chocobo-sprint.md §1, §5 (this bui / u8: the root-motion scale level applied (4 none) | 1 (0) |  |
| player_two/ride_probe.cpp:36 | kRidden | 0x1924 | RE-p2-chocobo-sprint.md §1, §5 (this bui / u8 | 1 (0) |  |
| player_two/ride_probe.cpp:37 | kMoveHelper | 0x1400 | RE-p2-chocobo-sprint.md §1, §5 (this bui / pointer; its +0x160 holds the anim instance as a w | 0 (0) |  |
| player_two/ride_probe.cpp:38 | kHelperAnim | 0x160 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 68 (0) |  |
| player_two/ride_probe.cpp:39 | kAnimWalkType | 0x498 | RE-p2-chocobo-sprint.md §1, §5 (this bui / u8: the minimum of the walk-type map's values (5 = | 30 (14) | ControlRigComponent.OnPostInitializeDelegate -> 0x488; SQEXSEADLayoutComponent.SoundName -> 0x488; PaperTerrainComponent.bClosedSpline -> 0x488 |
| player_two/ride_probe.cpp:40 | kAnimSprint | 0x3e4 | RE-p2-chocobo-sprint.md §1, §5 (this bui / u8 [ASSUMPTION: the sprint flag] | 8 (2) | EndEnvironmentPhysicsStaticMeshActor.DefaultPartsFadeOutTime -> 0x404; EndBattleSummonTargetPoint.LocalNumber -> 0x404 |
| player_two/ride_probe.cpp:41 | kAnimMapData | 0x4a0 | RE-p2-chocobo-sprint.md §1, §5 (this bui / TSet<{u8 key, u8 value}>: data, num, inline alloca | 29 (15) | MeshComponent.DynamicMaterialsForParameters -> 0x490; SQEXSEADLayoutComponent.LayoutInfo -> 0x490; PaperTerrainComponent.AssociatedSpline -> 0x490 |
| player_two/ride_probe.cpp:42 | kAnimMapNum | 0x4a8 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 25 (12) | ControlRigComponent.OnPreSetupDelegate -> 0x498; PaperTerrainComponent.RandomSeed -> 0x498; BoxComponent.BoxExtent -> 0x498 |
| player_two/ride_probe.cpp:43 | kAnimMapBits | 0x4b0 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 23 (12) | PaperTerrainComponent.TerrainColor -> 0x4a0; EndShapeComponent.ShapeBodySetup -> 0x4a0; EndCylinderComponent.CylinderSides -> 0x4a0 |
| player_two/ride_probe.cpp:44 | kAnimMapFree | 0x4d4 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 9 (7) | EndBonamikBoxScaleGroupParamsComponent.m_InnerConeScale -> 0x4c4; EndBonamikBoxWindComponent.Scale -> 0x4c4; EndFieldCustomParkourRuleVolumeComponent.EnableFallPlaneDistanceMax -> 0x4c4 |
| player_two/ride_probe.cpp:45 | kAnimMapEntry | 0xc | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| player_two/ride_probe.cpp:46 | kInlineBits | 0x80 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 393 (0) |  |
| player_two/ride_probe.cpp:47 | kAnimSpeedScale | 0x10cc | RE-p2-chocobo-sprint.md §1, §5 (this bui / float | 0 (0) |  |
| player_two/ride_probe.cpp:49 | kLayerMove | 0x890 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 10 (1) | EndCardGame3DCardBoard.DeckMoveBeforeTransform -> 0x8b0 |
| player_two/ride_probe.cpp:50 | kLayerDash | 0x8a4 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 2 (0) |  |
| player_two/ride_probe.cpp:51 | kLayerFlags | 0x8b8 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 3 (0) |  |
| player_two/ride_probe.cpp:52 | kLayerCount | 0x8c4 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 1 (0) |  |
| player_two/ride_probe.cpp:53 | kLayers | 0xa | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 53 (0) |  |
| player_two/ride_probe.cpp:54 | kRootGameMode | 0x1b8 | RE-p2-chocobo-sprint.md §1, §5 (this bui / [[game root]+0x1B8]+0x3D558: the game mode byte (3 | 30 (0) |  |
| player_two/ride_probe.cpp:55 | kGameModeByte | 0x3d558 | RE-p2-chocobo-sprint.md §1, §5 (this bui /  | 0 (0) |  |
| player_two/ride_probe.cpp:58 | kAiPawn | 0x258 | RE-p2-chocobo-homing.md §6 (this build). / AController::Pawn | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| player_two/ride_probe.cpp:59 | kAiPauseMask | 0x460 | RE-p2-chocobo-homing.md §6 (this build). / u32: bit 0 SYSTEM (the possession swap's unpossess | 19 (0) |  |
| player_two/ride_probe.cpp:60 | kAiWeakChar | 0x464 | RE-p2-chocobo-homing.md §6 (this build). /  | 5 (0) |  |
| player_two/ride_probe.cpp:61 | kAiWeakPawn | 0x46c | RE-p2-chocobo-homing.md §6 (this build). /  | 6 (0) |  |
| player_two/ride_probe.cpp:62 | kAiMoveComp | 0x528 | RE-p2-chocobo-homing.md §6 (this build). / AEndAIController::AIMoveComponent | 34 (17) | SkinnedMeshComponent.SkinCacheUsage -> 0x518; ProceduralMeshComponent.ProcMeshSections -> 0x518; PaperFlipbookComponent.AccumulatedTime -> 0x518 |
| player_two/ride_probe.cpp:63 | kAiRideLatch | 0xad8 | RE-p2-chocobo-homing.md §6 (this build). / u8 (and +0xAD9) | 2 (1) | SkeletalMeshComponent.BodySetup -> 0xac8 |
| player_two/ride_probe.cpp:64 | kAiBrainIndex | 0x520 | RE-p2-chocobo-homing.md §6 (this build). /  | 34 (13) | SkinnedMeshComponent.MasterPoseComponent -> 0x510; ProceduralMeshComponent.ProcMeshBodySetup -> 0x510; PaperFlipbookComponent.PlayRate -> 0x510 |
| player_two/ride_probe.cpp:65 | kFollowTarget | 0x358 | RE-p2-chocobo-homing.md §6 (this build). / the follow state's weak target | 21 (2) | EndNavMultiSwitchingVolume.EndMultiBoxComponent -> 0x378; WidgetInteractionComponent.LastLocalHitLocation -> 0x350 |
| player_two/ride_probe.cpp:66 | kFollowRange | 0x344 | RE-p2-chocobo-homing.md §6 (this build). / float, teleport distance | 9 (1) | EndStreamingSwitchVolume.NoneSwitchLabel -> 0x354 |
| player_two/ride_probe.cpp:67 | kFollowWarp | 0x39d | RE-p2-chocobo-homing.md §6 (this build). / u8, teleport pending | 1 (1) | EndBattleSummonTargetPoint.bWarp_All -> 0x3bd |
| player_two/ride_probe.cpp:68 | kFollowBusy | 0x400 | RE-p2-chocobo-homing.md §6 (this build). / u8, request in progress | 69 (2) | EndEnvironmentPhysicsStaticMeshActor.BoundsShapeLocation -> 0x420; EndNavWheelieBlockWallVolume.TriggerInterval -> 0x420 |
| player_two/ride_probe.cpp:69 | kMoveStatus | 0x780 | RE-p2-chocobo-homing.md §6 (this build). / u8 x2 | 19 (4) | NiagaraComponent.OnSystemFinished -> 0x770; EndBikeItemActor.OverlapCollisionComponent -> 0x7a0; EndCableCartSkeletalMeshActor.IconTransform -> 0x7a0 |
| player_two/ride_probe.cpp:70 | kMoveVelocity | 0x758 | RE-p2-chocobo-homing.md §6 (this build). / float x4 | 16 (1) | EndWorldReportActor.Monitor_Change_SE -> 0x778 |
| player_two/ride_probe.cpp:71 | kHolder | 0x638 | RE-p2-chocobo-homing.md §6 (this build). / the control holder: +0x18 the move request, +0x24  | 13 (10) | NiagaraComponent.Asset -> 0x628; EndFeelerConflictActor.LoseFadeOutTime -> 0x658; EndFeelerLaserActor.SkeletalMesh -> 0x658 |
| player_two/ride_probe.cpp:72 | kHolderMove | 0x18 | RE-p2-chocobo-homing.md §6 (this build). /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/ride_probe.cpp:368 | kStStick | 0x10 | The chocobo move state's input memory (R / f32 x2, the stored stick (0.01 hysteresis) | 1283 (0) |  |
| player_two/ride_probe.cpp:369 | kStDashLatch | 0x2c | The chocobo move state's input memory (R / u8, set by Dash held, cleared on the stop path | 299 (0) |  |
| player_two/ride_probe.cpp:370 | kStAccelPrev | 0x2d | The chocobo move state's input memory (R / u8, the previous tick's Accel (the edge memory) | 15 (0) |  |
| player_two/ride_probe.cpp:371 | kStRunTimer | 0x34 | The chocobo move state's input memory (R / f32, DashToRunTime | 329 (0) |  |
| player_two/ride_probe.cpp:372 | kStRaiseHold | 0x398 | The chocobo move state's input memory (R / f32 | 21 (3) | EndBattleSummonTargetPoint.bSpawn_NeoBahamut -> 0x3b8; EndShootingCoasterSpeedTriggerBox.RideSpeed -> 0x3b8; EndTurnBackBox.EnableBlockWall -> 0x3b8 |
| player_two/ride_probe.cpp:373 | kStSkipTimer | 0x39c | The chocobo move state's input memory (R / f32, the whole tick is skipped while > 0 | 10 (5) | EndBattleSummonTargetPoint.bSpawn_DLCMoogle -> 0x3bc; EndShootingCoasterEventTriggerBox.MaxLength -> 0x3bc; EndShootingCoasterSpawnerTriggerBox.DeactivateOnStateTrigger -> 0x3bc |
| player_two/ride_probe.cpp:374 | kStRaiseSkip | 0x3ac | The chocobo move state's input memory (R / u8 | 10 (3) | EndBattleSummonTargetPoint.bWarp_Phoenix -> 0x3cc; EndNavModifierInDoorVolume.OverrideCameraSettingID -> 0x3cc; EndNavModifierVolume.WallAffiliationID -> 0x3cc |
| player_two/ride_probe.cpp:375 | kStDashPrev | 0x3ad | The chocobo move state's input memory (R / u8, the previous tick's Dash | 1 (1) | EndBattleSummonTargetPoint.bWarp_NeoBahamut -> 0x3cd |
| player_two/ride_watch.cpp:36 | kActiveVehicle | 0x5b8 | AEndPlayerController (SDK offsets, RE-p2 / ActiveVehicleCoordinator | 19 (8) | SynthComponent.ConcurrencySet -> 0x5b0; EndBikeSplineComponent.BuggyEnableInputSlideLength -> 0x5a8; EndFeelerAttendantActor.WalkToIdleMoveDelayTimeRange -> 0x5d8 |
| player_two/ride_watch.cpp:37 | kVehicleMapCount | 0x5c8 | AEndPlayerController (SDK offsets, RE-p2 / VehicleCoordinators (TMap) element count, i32 | 21 (11) | EndCrashBoxMiniGameElectricCableActor.m_MainStaticMeshComponent -> 0x5e8; EndFeelerAttendantActor.IdleMoveVelocityOutputRange -> 0x5e8; EndFeelerBarrierActor.ExtrusionMoveDeltaMax -> 0x5e8 |
| player_two/ride_watch.cpp:39 | kCoordBody | 0x30 | AEndPlayerController (SDK offsets, RE-p2 /  | 1052 (0) |  |
| player_two/ride_watch.cpp:40 | kCoordRider | 0x38 | AEndPlayerController (SDK offsets, RE-p2 /  | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| player_two/ride_watch.cpp:41 | kChocoboBoarding | 0x155 | AEndPlayerController (SDK offsets, RE-p2 / u8 | 3 (0) |  |
| player_two/ride_watch.cpp:42 | kChocoboStatePtr | 0x58 | AEndPlayerController (SDK offsets, RE-p2 / -> u32 state id | 416 (1) | NiagaraEmitter.bLocalSpace -> 0x51 |
| player_two/ride_watch.cpp:43 | kBuggyStatePtr | 0x1e8 | AEndPlayerController (SDK offsets, RE-p2 / -> u32 state id | 46 (1) | NiagaraEmitter.bLimitDeltaTime -> 0x1dc |
| player_two/ride_watch.cpp:52 | kRootRideManager | 0x270 | entries at +0x40 + 0x2C * player type (s /  | 67 (6) | PrimitiveComponent.TranslucencySortOffset -> 0x230; AudioComponent.AttenuationOverrides -> 0x268; NiagaraEmitter.m_VfxRandomSeed_EveryFrame_Params -> 0x258 |
| player_two/ride_watch.cpp:53 | kManagerState | 0x8 | entries at +0x40 + 0x2C * player type (s / u8 (the next byte read the same value live: 0x0707 | 1705 (0) |  |
| player_two/ride_watch.cpp:54 | kManagerGetOff | 0xeb0 | entries at +0x40 + 0x2C * player type (s /  | 1 (0) |  |
| player_two/ride_watch.cpp:55 | kManagerParty | 0xeb5 | entries at +0x40 + 0x2C * player type (s /  | 0 (0) |  |
| player_two/ride_watch.cpp:56 | kManagerBattle | 0xeb6 | entries at +0x40 + 0x2C * player type (s /  | 0 (0) |  |
| player_two/ride_watch.cpp:57 | kMemberEntries | 0x40 | entries at +0x40 + 0x2C * player type (s /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/ride_watch.cpp:58 | kMemberEntrySize | 0x2c | entries at +0x40 + 0x2C * player type (s /  | 299 (0) |  |
| player_two/ride_watch.cpp:59 | kEntryChocobo | 0x20 | entries at +0x40 + 0x2C * player type (s /  | 810 (0) |  |
| player_two/ride_watch.cpp:60 | kPlayerTypes | 0xf | entries at +0x40 + 0x2C * player type (s /  | 19 (0) |  |
| player_two/ride_watch.cpp:63 | kChocoboRideMode | 0x1910 | entries at +0x40 + 0x2C * player type (s / u8 | 1 (0) |  |
| player_two/ride_watch.cpp:64 | kChocoboRideObject | 0x1918 | entries at +0x40 + 0x2C * player type (s / pointer | 1 (0) |  |
| player_two/ride_watch.cpp:65 | kChocoboFlag1920 | 0x1920 | entries at +0x40 + 0x2C * player type (s / u8 | 1 (0) |  |
| player_two/ride_watch.cpp:66 | kChocoboFlag1922 | 0x1922 | entries at +0x40 + 0x2C * player type (s / u8 | 0 (0) |  |
| player_two/ride_watch.cpp:67 | kChocoboRidden | 0x1924 | entries at +0x40 + 0x2C * player type (s / u8 | 1 (0) |  |
| player_two/ride_watch.cpp:68 | kCoreComponent | 0x638 | entries at +0x40 + 0x2C * player type (s / IsUCPC_Core: mov rcx,[rcx+638h] | 13 (10) | NiagaraComponent.Asset -> 0x628; EndFeelerConflictActor.LoseFadeOutTime -> 0x658; EndFeelerLaserActor.SkeletalMesh -> 0x658 |
| player_two/ride_watch.cpp:72 | kAttachParent | 0xc0 | USceneComponent::AttachParent (as player /  | 255 (0) |  |
| player_two/ride_watch.cpp:89 | kEntryGrace | 0x18 | ride while +0x1C is 0 or the timer runs) / float | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/ride_watch.cpp:90 | kEntry1C | 0x1c | ride while +0x1C is 0 or the timer runs) / u8 | 361 (0) |  |
| player_two/ride_watch.cpp:91 | kEntry1D | 0x1d | ride while +0x1C is 0 or the timer runs) / u8 | 33 (0) |  |
| player_two/ride_watch.cpp:95 | kEvery | 0x5 | ride while +0x1C is 0 or the timer runs) / ticks between samples | 30 (0) |  |
| player_two/rumble_watch.cpp:35 | kAppDeviceCode | 0x1e0 | RE-p2-rumble.md §2.2, §5 (this build). / i32: the last active device (XInput pad k = -2-3k) | 48 (6) | NiagaraEmitter.bFixedBounds -> 0x1dc; NiagaraEmitter.bUseMinDetailLevel -> 0x1dc; NiagaraEmitter.bUseMaxDetailLevel -> 0x1dc |
| player_two/rumble_watch.cpp:37 | kPlayerInput | 0x350 | APlayerController (stock 4.26 fields, th /  | 22 (2) | EndStreamingSwitchVolume.SwitchLevelSets -> 0x360; WidgetInteractionComponent.LocalHitLocation -> 0x348 |
| player_two/rumble_watch.cpp:38 | kEffectsNum | 0x360 | APlayerController (stock 4.26 fields, th / ActiveForceFeedbackEffects.Num | 16 (2) | EndNavMultiSwitchingVolume.ModifierVolume -> 0x380; WidgetInteractionComponent.HoveredWidgetComponent -> 0x358 |
| player_two/rumble_watch.cpp:39 | kValues | 0x3b8 | APlayerController (stock 4.26 fields, th / FForceFeedbackValues: LeftLarge, LeftSmall, RightL | 19 (3) | EndBattleSummonTargetPoint.TerritoryIDs -> 0x3d8; EndNavSwitchingVolume.CollisionAndNavProfileType -> 0x3d8; EndNavModifierVolume.AfiliationBattleScene -> 0x3d8 |
| player_two/rumble_watch.cpp:40 | kFlags | 0x450 | APlayerController (stock 4.26 fields, th /  | 32 (1) | PrimitiveComponent.OnComponentHit -> 0x440 |
| player_two/rumble_watch.cpp:41 | kFeedbackOn | 0x20 | APlayerController (stock 4.26 fields, th / bForceFeedbackEnabled | 810 (0) |  |
| player_two/rumble_watch.cpp:42 | kFeedbackScale | 0x454 | APlayerController (stock 4.26 fields, th /  | 3 (1) | PrimitiveComponent.OnComponentSleep -> 0x444 |
| player_two/rumble_watch.cpp:44 | kMaxEvents | 0x18 | APlayerController (stock 4.26 fields, th / per frame | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/scope_leaks.cpp:32 | kComponentGuard | 0x9a4 | check that the three globals are where t / bit 0: this component advanced the clock this fram | 2 (0) |  |
| player_two/scope_leaks.cpp:38 | kLeaderBlockSize | 0x5c | +0x10, two 12-byte vectors and dt. The b /  | 156 (1) | NiagaraEmitter.RandomSeed -> 0x54 |
| player_two/session.cpp:46 | kPlayerOneWatchTicks | 0x3 | Session: player 2's party member, the le /  | 42 (0) |  |
| player_two/session.cpp:49 | kTakeTicks | 0x78 | Session: player 2's party member, the le /  | 332 (0) |  |
| player_two/session.cpp:53 | kControllerCheckTicks | 0x1e | Session: player 2's party member, the le /  | 20 (0) |  |
| player_two/session.cpp:58 | kBackstopTicks | 0x258 | Session: player 2's party member, the le /  | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| player_two/session.cpp:59 | kBackstopTicksNoEvents | 0x1e | Session: player 2's party member, the le /  | 20 (0) |  |
| player_two/session.cpp:63 | kRetakeLookTicks | 0x258 | Session: player 2's party member, the le /  | 61 (7) | AudioComponent.PitchMultiplier -> 0x250; NiagaraEmitter.Parent -> 0x240; AtmosphericFogComponent.TransmittanceTexture -> 0x250 |
| player_two/session.cpp:66 | kEventLines | 0xc8 | Session: player 2's party member, the le /  | 232 (0) |  |
| player_two/session.cpp:72 | kRejoinSettleTicks | 0xb4 | Session: player 2's party member, the le /  | 112 (0) |  |
| player_two/session.cpp:73 | kRejoinTries | 0x5 | Session: player 2's party member, the le /  | 30 (0) |  |
| player_two/session.cpp:79 | kAwaitCheckTicks | 0x1e | Session: player 2's party member, the le /  | 20 (0) |  |
| player_two/session.cpp:83 | kSlotWaitTicks | 0xa | Session: player 2's party member, the le /  | 53 (0) |  |
| player_two/session.cpp:87 | kHeldAiTicks | 0x6 | Session: player 2's party member, the le /  | 23 (0) |  |
| player_two/session.cpp:88 | kSlotCheckTicks | 0x3c | Session: player 2's party member, the le /  | 318 (0) |  |
| player_two/session.cpp:181 | kLoggedMoves | 0x3 | line per move for the first few; later o /  | 42 (0) |  |
| player_two/shared_ride.cpp:24 | kActiveVehicle | 0x5b8 | AEndPlayerController::ActiveVehicleCoord /  | 19 (8) | SynthComponent.ConcurrencySet -> 0x5b0; EndBikeSplineComponent.BuggyEnableInputSlideLength -> 0x5a8; EndFeelerAttendantActor.WalkToIdleMoveDelayTimeRange -> 0x5d8 |
| player_two/shared_ride.cpp:26 | kCoordBody | 0x30 | A coordinator's VehicleChara (the buggy, /  | 1052 (0) |  |
| player_two/shared_ride.cpp:28 | kPawnController | 0x260 | APawn::Controller, and AActor's bHidden  /  | 99 (6) | NiagaraEmitter.m_VfxRandomSeed_SpawnOnly_Params -> 0x248; EndCinemaWidgetControlComponent.WidgetAsset -> 0x258; AtmosphericFogComponent.IrradianceTexture -> 0x258 |
| player_two/shared_ride.cpp:29 | kActorHidden | 0x58 | APawn::Controller, and AActor's bHidden  /  | 416 (1) | NiagaraEmitter.bLocalSpace -> 0x51 |
| player_two/shared_ride.cpp:30 | kHiddenBit | 0x20 | APawn::Controller, and AActor's bHidden  /  | 810 (0) |  |
| player_two/shared_ride.cpp:44 | kIdle | 0x0 | APawn::Controller, and AActor's bHidden  /  | 2161 (0) |  |
| player_two/shoulder_scale.cpp:30 | kPoseAimY | 0x88 | at op+0x40 (checked by disassembly, 2026 / op+0x40 pose, +0x48 AimOffset Y (lateral) | 292 (0) |  |
| player_two/shoulder_scale.cpp:31 | kBattleOperator | 0x2ca8 | at op+0x40 (checked by disassembly, 2026 /  | 0 (0) |  |
| player_two/stick_select.cpp:9 | kFloatsPerRecord | 0x6 |  /  | 23 (0) |  |
| player_two/vending.cpp:45 | kPressFrames | 0x3c | RE-p2-field-interact.md §15.9 (this buil /  | 318 (0) |  |
| player_two/vending.cpp:55 | kPartyLeader | 0x148 | RE-p2-field-interact.md §15.9 (this buil / the party manager's leader, an FWeakObjectPtr | 92 (0) |  |
| player_two/walk_limits.cpp:33 | kControllerNavArea | 0x590 |  /  | 31 (20) | SimpleDynamicMeshComponent.bExplicitShowWireframe -> 0x580; EndBikeSplineComponent.SplineMetadata -> 0x580; EndSingleSidedSplineComponent.bValidRightSide -> 0x580 |
| player_two/walk_limits.cpp:34 | kNavAreaController | 0xb0 |  /  | 301 (0) |  |
| player_two/walk_limits.cpp:49 | kManagerVolumes | 0x8 | `this` is unused; the game's instance is /  | 1705 (0) |  |
| player_two/walk_limits.cpp:50 | kManagerVolumeCount | 0x10 | `this` is unused; the game's instance is /  | 1283 (0) |  |
| player_two/walk_limits.cpp:51 | kManagerStack | 0xf8 | `this` is unused; the game's instance is / {int32 refcount; u8 move; u8 dash; pad} entries | 149 (1) | NiagaraEmitter.Platforms -> 0xe8 |
| player_two/walk_limits.cpp:52 | kManagerStackCount | 0x100 | `this` is unused; the game's instance is /  | 130 (0) |  |
| player_two/walk_limits.cpp:53 | kManagerPaused | 0x128 | `this` is unused; the game's instance is / byte: the game's update does nothing while set | 106 (1) | NiagaraEmitter.ScalabilityOverrides -> 0x118 |
| player_two/walk_limits.cpp:54 | kManagerLock | 0x188 | `this` is unused; the game's instance is /  | 48 (0) |  |
| player_two/walk_limits.cpp:55 | kVolumeLocationId | 0x270 | `this` is unused; the game's instance is /  | 67 (6) | PrimitiveComponent.TranslucencySortOffset -> 0x230; AudioComponent.AttenuationOverrides -> 0x268; NiagaraEmitter.m_VfxRandomSeed_EveryFrame_Params -> 0x258 |
| player_two/walk_limits.cpp:56 | kVolumeUntouchMargin | 0x278 | `this` is unused; the game's instance is /  | 62 (5) | PrimitiveComponent.RuntimeVirtualTextures -> 0x268; LightComponent.LightFunctionMaterial -> 0x270; ExponentialHeightFogComponent.Medium -> 0x270 |
| player_two/walk_limits.cpp:57 | kVolumeFlags | 0x280 | `this` is unused; the game's instance is /  | 76 (6) | SQEX_KineDriver_Component.ReplaceKDIs -> 0x278; BoxReflectionCaptureComponent.BoxTransitionDistance -> 0x278; LightComponent.LightFunctionScale -> 0x278 |
| player_two/walk_limits.cpp:58 | kVolumeMoveType | 0x2e9 | `this` is unused; the game's instance is / EEndSpecificMoveType | 1 (0) |  |
| player_two/walk_limits.cpp:59 | kVolumeDashType | 0x2ea | `this` is unused; the game's instance is / EEndDashType | 1 (0) |  |
| player_two/walk_limits.cpp:60 | kVolumeForcedOut | 0x308 | `this` is unused; the game's instance is / bool: never entered by position; a forced volume l | 25 (0) |  |
| player_two/walk_limits.cpp:62 | kMaxVolumes | 0x1000 | `this` is unused; the game's instance is /  | 2 (0) |  |
| player_two/warp_camera_watch.cpp:26 | kOpWeight | 0x8 | The Field operator (FEndModularCameraOpe / float | 1705 (0) |  |
| player_two/warp_camera_watch.cpp:27 | kOpPose | 0x40 | The Field operator (FEndModularCameraOpe / the pose floats +0x40..+0x98 | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/warp_camera_watch.cpp:29 | kOpPivot | 0x60 | The Field operator (FEndModularCameraOpe / float[3]: subject location + op +0x760 | 484 (1) | NiagaraEmitter.AllocationMode -> 0x53 |
| player_two/warp_camera_watch.cpp:30 | kOpSubjectLocation | 0x70 | The Field operator (FEndModularCameraOpe / float[3]: the subject's root plus its camera offse | 515 (0) |  |
| player_two/warp_camera_watch.cpp:31 | kOpActive | 0xd0 | The Field operator (FEndModularCameraOpe / u8 | 205 (0) |  |
| player_two/warp_camera_watch.cpp:32 | kOpCurrent | 0xd1 | The Field operator (FEndModularCameraOpe / u8: the operator the camera actor is written from | 7 (0) |  |
| player_two/warp_camera_watch.cpp:33 | kOpSubject | 0xf0 | The Field operator (FEndModularCameraOpe / FWeakObjectPtr: whom the camera follows | 135 (0) |  |
| player_two/warp_camera_watch.cpp:34 | kOpModules | 0x6e0 | The Field operator (FEndModularCameraOpe / module table, 16 bytes a slot | 9 (3) | EndFeelerSplineActor.MasterSkeletalMeshComponents -> 0x700; EndFeelerTornadeActor.MasterSkeletalMeshComponents -> 0x700; ParticleSystemComponent.WarmupTickRate -> 0x6d0 |
| player_two/warp_camera_watch.cpp:35 | kLeaderChangeSlot | 0x16 | The Field operator (FEndModularCameraOpe /  | 11 (0) |  |
| player_two/warp_camera_watch.cpp:36 | kModuleStride | 0x10 | The Field operator (FEndModularCameraOpe /  | 1283 (0) |  |
| player_two/warp_camera_watch.cpp:37 | kModuleActive | 0x44 | The Field operator (FEndModularCameraOpe / u8 | 249 (0) |  |
| player_two/warp_camera_watch.cpp:38 | kModuleRunning | 0x45 | The Field operator (FEndModularCameraOpe / u8 | 12 (0) |  |
| player_two/warp_camera_watch.cpp:39 | kLeaderChangeTarget | 0x3f0 | The Field operator (FEndModularCameraOpe / FWeakObjectPtr: the hand-over's new subject | 75 (1) | EndNavSimpleWallVolume.CylinderInnerRadius -> 0x410 |
| player_two/warp_camera_watch.cpp:41 | kPcmPreviousOperator | 0x2cd1 | The camera manager: / u8, next to the current operator (+0x2CD0) | 0 (0) |  |
| player_two/warp_camera_watch.cpp:42 | kPcmLeader | 0x2cc0 | The camera manager: / the leader its OnLeaderChanged recorded | 0 (0) |  |
| player_two/warp_camera_watch.cpp:43 | kPcmLeaderWeak | 0x2cc8 | The camera manager: /  | 0 (0) |  |
| player_two/wheelie_ride.cpp:120 | kBeginName | 0x4 | slot is replaced. /  | 542 (0) |  |
| player_two/wheelie_ride.cpp:121 | kBeginFlagD | 0xd | slot is replaced. /  | 32 (0) |  |
| player_two/wheelie_ride.cpp:128 | kGetOffType | 0x0 | The 16-byte getoff parameters (§14.6): t /  | 2161 (0) |  |
| player_two/wheelie_ride.cpp:129 | kGetOffInstant | 0x2 | The 16-byte getoff parameters (§14.6): t /  | 90 (0) |  |
| player_two/wheelie_ride.cpp:148 | kScooterKey | 0x4 | Objects and fields (§14; this build). /  | 542 (0) |  |
| player_two/wheelie_ride.cpp:152 | kActiveVehicle | 0x5b8 | Objects and fields (§14; this build). / AEndPlayerController::ActiveVehicleCoordinator | 19 (8) | SynthComponent.ConcurrencySet -> 0x5b0; EndBikeSplineComponent.BuggyEnableInputSlideLength -> 0x5a8; EndFeelerAttendantActor.WalkToIdleMoveDelayTimeRange -> 0x5d8 |
| player_two/wheelie_ride.cpp:153 | kCoordScooter | 0x30 | Objects and fields (§14; this build). /  | 1052 (0) |  |
| player_two/wheelie_ride.cpp:154 | kCoordShared | 0x48 | Objects and fields (§14; this build). / TSharedPtr object: +0 target (weak), +8 list, +0x1 | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| player_two/wheelie_ride.cpp:155 | kCoordInstant | 0x11c | Objects and fields (§14; this build). / u8, from the request | 35 (0) |  |
| player_two/wheelie_ride.cpp:156 | kCoordApplyNow | 0x11e | Objects and fields (§14; this build). / u8, from the request | 0 (0) |  |
| player_two/wheelie_ride.cpp:157 | kCoordCooldown | 0x1f4 | Objects and fields (§14; this build). / float | 16 (1) | NiagaraEmitter.m_VfxMaterialIndependentParameter_RandomSeed -> 0x1d4 |
| player_two/wheelie_ride.cpp:158 | kCoordScripted | 0x1f8 | Objects and fields (§14; this build). / u8: a ride latched for TryBeginRide | 116 (52) | SynthComponent.bAutoDestroy -> 0x1f0; SynthComponent.bStopWhenOwnerDestroyed -> 0x1f0; SynthComponent.bAllowSpatialization -> 0x1f0 |
| player_two/wheelie_ride.cpp:159 | kCoordDisabled | 0x2a | Objects and fields (§14; this build). / u8 | 28 (0) |  |
| player_two/wheelie_ride.cpp:160 | kCoordStateMachine | 0x218 | Objects and fields (§14; this build). /  | 54 (24) | PrimitiveComponent.MinDrawDistance -> 0x210; SQEX_Bonamik_Component_v2.m_BonamikAsset -> 0x210; AudioComponent.bAutoDestroy -> 0x210 |
| player_two/wheelie_ride.cpp:161 | kCoordStatePtr | 0x230 | Objects and fields (§14; this build). / -> u32 state id | 160 (17) | PrimitiveComponent.bHasCustomNavigableGeometry -> 0x228; SQEX_BonamikWind_Component_v2.m_BonamikWind -> 0x228; AudioComponent.VolumeModulationMin -> 0x228 |
| player_two/wheelie_ride.cpp:162 | kCoordPending | 0x238 | Objects and fields (§14; this build). / pending row (null when none) | 137 (10) | PrimitiveComponent.CustomDepthStencilValue -> 0x234; SQEX_Bonamik_Component_v2.m_BonamikEnable -> 0x230; AudioComponent.VolumeMultiplier -> 0x230 |
| player_two/wheelie_ride.cpp:163 | kSharedTarget | 0x0 | Objects and fields (§14; this build). /  | 2161 (0) |  |
| player_two/wheelie_ride.cpp:165 | kScooterRidden | 0x2318 | Objects and fields (§14; this build). / u8 | 0 (0) |  |
| player_two/wheelie_ride.cpp:166 | kScooterExcluded | 0x231a | Objects and fields (§14; this build). / u8 | 0 (0) |  |
| player_two/wheelie_ride.cpp:167 | kScooterSetting | 0x1870 | Objects and fields (§14; this build). / MovementSetting*: +0x394 distance, +0x398 max yaw, | 4 (3) | EndChocoboCharacter.PartsMeshComp_Head -> 0x1890; EndSelfBalancingScooterCharacter.MovementSetting -> 0x1890; EndTinyBroncoCharacter.MovementSetting -> 0x1890 |
| player_two/wheelie_ride.cpp:168 | kSettingDistance | 0x394 | Objects and fields (§14; this build). /  | 16 (6) | EndBattleSummonTargetPoint.bSpawn_Odin -> 0x3b4; EndBattleVolumeElenaPunchMines.GroupId -> 0x3b4; EndMotionSwitchBox.EndOverlapPlayActionName -> 0x3b4 |
| player_two/wheelie_ride.cpp:169 | kSettingMaxYaw | 0x398 | Objects and fields (§14; this build). /  | 21 (3) | EndBattleSummonTargetPoint.bSpawn_NeoBahamut -> 0x3b8; EndShootingCoasterSpeedTriggerBox.RideSpeed -> 0x3b8; EndTurnBackBox.EnableBlockWall -> 0x3b8 |
| player_two/wheelie_ride.cpp:170 | kSettingMinPower | 0x39c | Objects and fields (§14; this build). /  | 10 (5) | EndBattleSummonTargetPoint.bSpawn_DLCMoogle -> 0x3bc; EndShootingCoasterEventTriggerBox.MaxLength -> 0x3bc; EndShootingCoasterSpawnerTriggerBox.DeactivateOnStateTrigger -> 0x3bc |
| player_two/wheelie_ride.cpp:171 | kControlHolder | 0x638 | Objects and fields (§14; this build). / the character's control holder; its vt+0x40 gives  | 13 (10) | NiagaraComponent.Asset -> 0x628; EndFeelerConflictActor.LoseFadeOutTime -> 0x658; EndFeelerLaserActor.SkeletalMesh -> 0x658 |
| player_two/wheelie_ride.cpp:172 | kHolderComponent | 0x40 | Objects and fields (§14; this build). /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/wheelie_ride.cpp:173 | kHolderMoveRequest | 0x18 | Objects and fields (§14; this build). / FVector: SetMoveInput's dir * scale | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/wheelie_ride.cpp:174 | kStateStickX | 0x10 | Objects and fields (§14; this build). / the on-foot state's stick pair (slot 11 TickInput) | 1283 (0) |  |
| player_two/wheelie_ride.cpp:175 | kStateStickY | 0x14 | Objects and fields (§14; this build). /  | 478 (0) |  |
| player_two/wheelie_ride.cpp:180 | kMeshOwner | 0xa0 | Objects and fields (§14; this build). / a component's owner (the notify's mesh -> the ride | 264 (0) |  |
| player_two/wheelie_ride.cpp:181 | kActorRoot | 0x138 | Objects and fields (§14; this build). / an actor's root component (§14.15) | 100 (1) | NiagaraEmitter.bInterpolatedSpawning -> 0x1dc |
| player_two/wheelie_ride.cpp:182 | kAttachParent | 0xc0 | Objects and fields (§14; this build). / a scene component's attach parent | 255 (0) |  |
| player_two/wheelie_ride.cpp:183 | kFieldOperator | 0x2cb0 | Objects and fields (§14; this build). / PCM: the Field operator | 0 (0) |  |
| player_two/wheelie_ride.cpp:184 | kOperatorModules | 0x6e0 | Objects and fields (§14; this build). / op: the module slots (16 bytes each) | 9 (3) | EndFeelerSplineActor.MasterSkeletalMeshComponents -> 0x700; EndFeelerTornadeActor.MasterSkeletalMeshComponents -> 0x700; ParticleSystemComponent.WarmupTickRate -> 0x6d0 |
| player_two/wheelie_watch.cpp:29 | kActiveVehicle | 0x5b8 |  /  | 19 (8) | SynthComponent.ConcurrencySet -> 0x5b0; EndBikeSplineComponent.BuggyEnableInputSlideLength -> 0x5a8; EndFeelerAttendantActor.WalkToIdleMoveDelayTimeRange -> 0x5d8 |
| player_two/wheelie_watch.cpp:30 | kVehicleMap | 0x5c0 |  /  | 17 (8) | EndFeelerAttendantActor.IdleMoveVelocityInputRange -> 0x5e0; EndFeelerConflictActor.BeginCenterAnimatedOffsetLoopTime -> 0x5e0; EndFeelerFollowingActor.SecondaryRollSize -> 0x5e0 |
| player_two/wheelie_watch.cpp:31 | kMapElementSize | 0x18 |  /  | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/wheelie_watch.cpp:32 | kMapElementValue | 0x8 |  /  | 1705 (0) |  |
| player_two/wheelie_watch.cpp:36 | kCoordScooter | 0x30 | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 1052 (0) |  |
| player_two/wheelie_watch.cpp:37 | kCoordRider | 0x38 | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 886 (1) | EndDialogueGenerateSourceShot.Camera -> 0x48 |
| player_two/wheelie_watch.cpp:38 | kCoordShared | 0x48 | UEndPlayerSBScooterCoordinator (RE-p2-wh / TSharedPtr object | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| player_two/wheelie_watch.cpp:39 | kCoordRiderTable | 0x58 | UEndPlayerSBScooterCoordinator (RE-p2-wh / TSharedPtr object | 416 (1) | NiagaraEmitter.bLocalSpace -> 0x51 |
| player_two/wheelie_watch.cpp:40 | kCoordGetOffFast | 0x182 | UEndPlayerSBScooterCoordinator (RE-p2-wh / u8: the getoff is instant | 0 (0) |  |
| player_two/wheelie_watch.cpp:41 | kCoordScripted | 0x1f8 | UEndPlayerSBScooterCoordinator (RE-p2-wh / u8 | 116 (52) | SynthComponent.bAutoDestroy -> 0x1f0; SynthComponent.bStopWhenOwnerDestroyed -> 0x1f0; SynthComponent.bAllowSpatialization -> 0x1f0 |
| player_two/wheelie_watch.cpp:42 | kCoordMiniGame | 0x1f9 | UEndPlayerSBScooterCoordinator (RE-p2-wh / u8 | 7 (2) | EndDolphinJumpOceanFloaterComponent.EnableRotation -> 0x1f1; SceneCaptureComponent.CaptureSource -> 0x1f1 |
| player_two/wheelie_watch.cpp:43 | kCoordUiShown | 0x1fa | UEndPlayerSBScooterCoordinator (RE-p2-wh / u8 | 5 (2) | SceneCaptureComponent.bCaptureEveryFrame -> 0x1f2; SceneCaptureComponent.bCaptureOnMovement -> 0x1f2 |
| player_two/wheelie_watch.cpp:44 | kCoordInstant | 0x1fc | UEndPlayerSBScooterCoordinator (RE-p2-wh / u8 | 26 (14) | SynthComponent.bOutputToBusOnly -> 0x1f4; CameraComponent.OrthoWidth -> 0x1f4; EndCategoryVolumeControlComponent.Volume -> 0x1f4 |
| player_two/wheelie_watch.cpp:45 | kCoordEnabled | 0x29 | UEndPlayerSBScooterCoordinator (RE-p2-wh / u8 (vt+0x300 sets it; its key listener is register | 51 (0) |  |
| player_two/wheelie_watch.cpp:46 | kCoordDisabled | 0x2a | UEndPlayerSBScooterCoordinator (RE-p2-wh / u8 (vt+0x308) | 28 (0) |  |
| player_two/wheelie_watch.cpp:47 | kCoordStatePtr | 0x230 | UEndPlayerSBScooterCoordinator (RE-p2-wh / -> u32 state id | 160 (17) | PrimitiveComponent.bHasCustomNavigableGeometry -> 0x228; SQEX_BonamikWind_Component_v2.m_BonamikWind -> 0x228; AudioComponent.VolumeModulationMin -> 0x228 |
| player_two/wheelie_watch.cpp:50 | kSharedListData | 0x8 | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 1705 (0) |  |
| player_two/wheelie_watch.cpp:51 | kSharedListNum | 0x10 | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 1283 (0) |  |
| player_two/wheelie_watch.cpp:52 | kSharedRequested | 0x28 | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 889 (0) |  |
| player_two/wheelie_watch.cpp:53 | kSharedIdle | 0x29 | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 51 (0) |  |
| player_two/wheelie_watch.cpp:54 | kSharedParking | 0x2a | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 28 (0) |  |
| player_two/wheelie_watch.cpp:55 | kSharedColours | 0x2b | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 20 (0) |  |
| player_two/wheelie_watch.cpp:56 | kListCap | 0x20 | UEndPlayerSBScooterCoordinator (RE-p2-wh /  | 810 (0) |  |
| player_two/wheelie_watch.cpp:58 | kTableNum | 0x12c | The per-rider table: 15 entries of 0x14  /  | 31 (0) |  |
| player_two/wheelie_watch.cpp:59 | kTableState | 0x134 | The per-rider table: 15 entries of 0x14  /  | 43 (0) |  |
| player_two/wheelie_watch.cpp:60 | kTableEntrySize | 0x14 | The per-rider table: 15 entries of 0x14  /  | 478 (0) |  |
| player_two/wheelie_watch.cpp:61 | kTableEntries | 0xf | The per-rider table: 15 entries of 0x14  /  | 19 (0) |  |
| player_two/wheelie_watch.cpp:66 | kScooterRider | 0x2210 | brake as the move state's slot 11 writes /  | 1 (1) | EndSelfBalancingScooterCharacter.RiderActor -> 0x2230 |
| player_two/wheelie_watch.cpp:67 | kScooterRidden | 0x2318 | brake as the move state's slot 11 writes /  | 0 (0) |  |
| player_two/wheelie_watch.cpp:68 | kScooterParked | 0x2319 | brake as the move state's slot 11 writes /  | 0 (0) |  |
| player_two/wheelie_watch.cpp:69 | kScooterExcluded | 0x231a | brake as the move state's slot 11 writes /  | 0 (0) |  |
| player_two/wheelie_watch.cpp:70 | kControlHolder | 0x638 | brake as the move state's slot 11 writes /  | 13 (10) | NiagaraComponent.Asset -> 0x628; EndFeelerConflictActor.LoseFadeOutTime -> 0x658; EndFeelerLaserActor.SkeletalMesh -> 0x658 |
| player_two/wheelie_watch.cpp:71 | kHolderAccel | 0x2c0 | brake as the move state's slot 11 writes /  | 57 (10) | PrimitiveComponent.MoveIgnoreComponents -> 0x2b0; EndCinemaWidgetControlComponent.StoryFlags -> 0x2b8; EndFieldClimbingFinishPointSetting.ManualEnterEffectOffset -> 0x2b0 |
| player_two/wheelie_watch.cpp:72 | kHolderBrake | 0x2c4 | brake as the move state's slot 11 writes /  | 14 (2) | LightComponent.bExposureCondition -> 0x2bc; PlanarReflectionComponent.ScreenPercentage -> 0x2bc |
| player_two/wheelie_watch.cpp:75 | kInputCatchAxis | 0x1240 | brake as the move state's slot 11 writes /  | 2 (1) | EndCharacterBase.MovementExpressionSetting -> 0x1260 |
| player_two/wheelie_watch.cpp:76 | kInputAccelAxis | 0x1244 | brake as the move state's slot 11 writes /  | 0 (0) |  |
| player_two/wheelie_watch.cpp:77 | kInputCatchOther | 0x11b0 | brake as the move state's slot 11 writes /  | 1 (0) |  |
| player_two/wheelie_watch.cpp:78 | kInputAccelOther | 0x11b4 | brake as the move state's slot 11 writes /  | 0 (0) |  |
| player_two/wheelie_watch.cpp:81 | kRootOnOff | 0x170 | The on/off counters (§4): [[game root]+0 /  | 75 (0) |  |
| player_two/wheelie_watch.cpp:82 | kOnOffBase | 0x1a1238c | The on/off counters (§4): [[game root]+0 /  | 0 (0) |  |
| player_two/wheelie_watch.cpp:83 | kOnOffAltBase | 0x1a1263c | The on/off counters (§4): [[game root]+0 /  | 0 (0) |  |
| player_two/wheelie_watch.cpp:92 | kEvery | 0xa | FWeakObjectPtr::Get (hud/hud_updates.cpp /  | 53 (0) |  |
| player_two/wheelie_watch.cpp:93 | kRidingEvery | 0x6 | FWeakObjectPtr::Get (hud/hud_updates.cpp /  | 23 (0) |  |
| player_two/widget_owner.cpp:41 | kPaneOwner | 0x3c8 | shows (+0x3D0). Its owner is recorded at /  | 207 (3) | EndEnvironmentPhysicsStaticMeshActor.PhysicsBehavior -> 0x3e8; EndBattleSummonTargetPoint.UnavailablePhase -> 0x3e8; EndNavModifierVolume.WallGroup -> 0x3e8 |
| player_two/widget_owner.cpp:42 | kPaneType | 0x3d0 | shows (+0x3D0). Its owner is recorded at / u8: the character whose synergy skills it lists | 169 (3) | EndEnvironmentPhysicsStaticMeshActor.PartsMassScale -> 0x3f0; EndBattleVolumeElenaPunchMines.CurrentlyOverlappingActors -> 0x3f0; EndWaterStaticMeshVolume.WaterType -> 0x3f0 |
| player_two/widget_owner.cpp:47 | kCellPartyList | 0x7b0 | leader flag, character type and stacked  /  | 12 (4) | SkinnedMeshComponent.CachedWorldSpaceBounds -> 0x7a0; NiagaraComponent.m_VfxRefDataInterfaceAssetMap -> 0x7a0; EndCableCartSkeletalMeshActor.bUseIcon2 -> 0x7d0 |
| player_two/widget_owner.cpp:48 | kPartyListOwner | 0x20 | leader flag, character type and stacked  /  | 810 (0) |  |
| player_two/widget_owner.cpp:49 | kCellSlot | 0x9e8 | leader flag, character type and stacked  / i32 | 9 (0) |  |
| player_two/widget_owner.cpp:50 | kCellLook | 0x9ec | leader flag, character type and stacked  / u8 | 2 (0) |  |
| player_two/widget_owner.cpp:51 | kCellLeader | 0x9f8 | leader flag, character type and stacked  / u8 | 4 (0) |  |
| player_two/widget_owner.cpp:52 | kCellType | 0x7b8 | leader flag, character type and stacked  / u8 | 11 (1) | AudioComponent.OnQueueSubtitles -> 0x7b0 |
| player_two/widget_owner.cpp:53 | kCellPosition | 0xa24 | leader flag, character type and stacked  / float x, y | 0 (0) |  |
| player_two/widget_owner.cpp:56 | kStoredLeaderType | 0x30 | leader flag, character type and stacked  /  | 1052 (0) |  |
| player_two/widget_owner.cpp:57 | kHudMode | 0x540 | leader flag, character type and stacked  /  | 25 (13) | PaperFlipbookComponent.CachedBodySetup -> 0x530; PaperTileMapComponent.TileMapColor -> 0x530; EndBonamikBoxScaleGroupParamsComponent.m_ChangeChainRootRotDrag -> 0x530 |
| player_two/widget_owner.cpp:60 | kController | 0xb8 | The UI system's slots. / EndUISystem[23] | 231 (1) | NiagaraEmitter.SimTarget -> 0x1d8 |
| player_two/widget_owner.cpp:61 | kKeyGuide | 0x1a8 | The UI system's slots. / EndUISystem[53] | 34 (0) |  |
| player_two/widget_owner.cpp:64 | kGuideWidget | 0x4 | (FWeakObjectPtr); the widget's UWidget:: /  | 542 (0) |  |
| player_two/widget_owner.cpp:65 | kGuideMode | 0x14 | (FWeakObjectPtr); the widget's UWidget:: /  | 478 (0) |  |
| player_two/widget_owner.cpp:66 | kVisibility | 0xc3 | (FWeakObjectPtr); the widget's UWidget:: /  | 6 (0) |  |
| player_two/widget_owner.cpp:67 | kLoggedCells | 0x4 | (FWeakObjectPtr); the widget's UWidget:: /  | 542 (0) |  |
| player_two/world_actor_reach.cpp:28 | kRootWorldActors | 0x190 | RE-p2-world-streaming.md §9.12.3 (this b /  | 47 (0) |  |
| player_two/world_actor_reach.cpp:29 | kItemsData | 0x20 | RE-p2-world-streaming.md §9.12.3 (this b / mgr: the item TSet's sparse array: data | 810 (0) |  |
| player_two/world_actor_reach.cpp:30 | kItemsNum | 0x28 | RE-p2-world-streaming.md §9.12.3 (this b / mgr: i32 ArrayNum | 889 (0) |  |
| player_two/world_actor_reach.cpp:31 | kItemsBits | 0x30 | RE-p2-world-streaming.md §9.12.3 (this b / mgr: allocation bits, inline (4 dwords); heap poin | 1052 (0) |  |
| player_two/world_actor_reach.cpp:32 | kOctrees | 0x70 | RE-p2-world-streaming.md §9.12.3 (this b / mgr: field FName -> octree (TMap): data | 515 (0) |  |
| player_two/world_actor_reach.cpp:33 | kOctreesNum | 0x78 | RE-p2-world-streaming.md §9.12.3 (this b / mgr: i32 ArrayNum | 332 (0) |  |
| player_two/world_actor_reach.cpp:34 | kOctreesBits | 0x80 | RE-p2-world-streaming.md §9.12.3 (this b / mgr: allocation bits, inline; heap pointer at +0x1 | 393 (0) |  |
| player_two/world_actor_reach.cpp:35 | kBitsHeap | 0x10 | RE-p2-world-streaming.md §9.12.3 (this b / the bit array's heap pointer, after its 16 inline  | 1283 (0) |  |
| player_two/world_actor_reach.cpp:36 | kBitsInline | 0x80 | RE-p2-world-streaming.md §9.12.3 (this b /  | 393 (0) |  |
| player_two/world_actor_reach.cpp:37 | kItemElement | 0x120 | RE-p2-world-streaming.md §9.12.3 (this b / element: key FName +0, the item object at +0x10 | 153 (0) |  |
| player_two/world_actor_reach.cpp:38 | kItemAt | 0x10 | RE-p2-world-streaming.md §9.12.3 (this b /  | 1283 (0) |  |
| player_two/world_actor_reach.cpp:39 | kOctreeElement | 0x20 | RE-p2-world-streaming.md §9.12.3 (this b / element: field FName +0, the octree object pointer | 810 (0) |  |
| player_two/world_actor_reach.cpp:40 | kOctreeObject | 0x8 | RE-p2-world-streaming.md §9.12.3 (this b /  | 1705 (0) |  |
| player_two/world_actor_reach.cpp:41 | kItemOctreeId | 0x8 | RE-p2-world-streaming.md §9.12.3 (this b / i32, -1: not in an octree | 1705 (0) |  |
| player_two/world_actor_reach.cpp:42 | kItemField | 0x10 | RE-p2-world-streaming.md §9.12.3 (this b / FName: the field whose octree holds it | 1283 (0) |  |
| player_two/world_actor_reach.cpp:43 | kItemRadius | 0x18 | RE-p2-world-streaming.md §9.12.3 (this b / float R: the pop radius (the tick's add and remove | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| player_two/world_actor_reach.cpp:44 | kItemState | 0x1c | RE-p2-world-streaming.md §9.12.3 (this b / i32: 6 armed, 0 requested, 1 spawned, 4, 2, 3 disa | 361 (0) |  |
| player_two/world_actor_reach.cpp:45 | kItemExtent | 0x20 | RE-p2-world-streaming.md §9.12.3 (this b / float E: the octree half-extent (the add's box que | 810 (0) |  |
| player_two/world_actor_reach.cpp:46 | kItemKey | 0x30 | RE-p2-world-streaming.md §9.12.3 (this b / FName: the CharaPop key | 1052 (0) |  |
| player_two/world_actor_reach.cpp:47 | kItemLocation | 0x50 | RE-p2-world-streaming.md §9.12.3 (this b / FVector | 488 (0) |  |
| player_two/world_actor_reach.cpp:50 | kOctreeAdd | 0x28 | RE-p2-world-streaming.md §9.12.3 (this b /  | 889 (0) |  |
| player_two/world_actor_reach.cpp:51 | kOctreeRemove | 0x30 | RE-p2-world-streaming.md §9.12.3 (this b /  | 1052 (0) |  |
| player_two/world_actor_reach.cpp:52 | kStateDisabled | 0x3 | RE-p2-world-streaming.md §9.12.3 (this b /  | 42 (0) |  |
| player_two/world_actor_reach.cpp:53 | kStatePending | 0x5 | RE-p2-world-streaming.md §9.12.3 (this b /  | 30 (0) |  |
| player_two/world_actor_reach.cpp:54 | kMaxItems | 0x10000 | RE-p2-world-streaming.md §9.12.3 (this b /  | 0 (0) |  |
| player_two/world_actor_reach.cpp:55 | kMaxOctrees | 0x40 | RE-p2-world-streaming.md §9.12.3 (this b /  | 834 (1) | NiagaraEmitter.bVersioningEnabled -> 0x50 |
| player_two/world_actor_reach.cpp:59 | kEvery | 0xa | RE-p2-world-streaming.md §9.12.3 (this b / ticks between passes | 53 (0) |  |
| renderer/family_boundary.cpp:36 | kSubmitSlot | 0xf0 | The entry bytes hold rel32 calls only, w / its RHISubmitCommandsHint entry | 135 (0) |  |
| renderer/family_boundary.cpp:38 | kImmediateListSize | 0x48 | The entry bytes hold rel32 calls only, w /  | 530 (1) | NiagaraEmitter.VersionData -> 0x40 |
| renderer/family_boundary.cpp:39 | kComputeContext | 0x28 | The entry bytes hold rel32 calls only, w / list: ComputeContext, logged against Context | 889 (0) |  |
| renderer/family_boundary.cpp:41 | kAnyThread | 0xff | The entry bytes hold rel32 calls only, w / ENamedThreads::AnyThread: "the caller's thread is  | 2 (0) |  |
| renderer/family_boundary_list.h:29 | kCommandLink | 0x8 | passes it), then advance the cursor, cou /  | 1705 (0) |  |
| renderer/family_boundary_list.h:30 | kExecuting | 0x10 | passes it), then advance the cursor, cou /  | 1283 (0) |  |
| renderer/family_boundary_list.h:31 | kNumCommands | 0x14 | passes it), then advance the cursor, cou /  | 478 (0) |  |
| renderer/family_boundary_list.h:32 | kContext | 0x20 | passes it), then advance the cursor, cou /  | 810 (0) |  |
| renderer/family_boundary_list.h:33 | kArena | 0x30 | passes it), then advance the cursor, cou / cursor; the end follows at +8 | 1052 (0) |  |
| renderer/family_boundary_list.h:37 | kIsAsyncCompute | 0x146 | passes it), then advance the cursor, cou / bIsAsyncComputeContext (byte) | 0 (0) |  |
| renderer/family_boundary_list.h:38 | kDevice | 0x148 | passes it), then advance the cursor, cou / FD3D12Device* | 92 (0) |  |
| renderer/family_boundary_list.h:39 | kDeviceDirect | 0xf70 | passes it), then advance the cursor, cou / device: the direct contexts' array; [0] is the def | 1 (0) |  |
| renderer/family_boundary_list.h:42 | kNodeSize | 0x10 | passes it), then advance the cursor, cou / {vtable, next} | 1283 (0) |  |
| renderer/me_feedback_once.cpp:43 | kCopyFirst | 0x5 | 16-byte block, so its entry is redirecte /  | 30 (0) |  |
| renderer/me_feedback_once.cpp:50 | kHiddenFirst | 0x5 | instruction `mov [rsp+18h],r8`. /  | 30 (0) |  |
| renderer/me_feedback_once.cpp:52 | kRendererAdditionalFamily | 0x84 | instruction `mov [rsp+18h],r8`. / the family copy's bAdditionalViewFamily (+0x74) | 145 (0) |  |
| renderer/me_feedback_once.cpp:57 | kMeManager | 0x50 | instruction `mov [rsp+18h],r8`. /  | 488 (0) |  |
| renderer/me_feedback_once.cpp:58 | kBudgetStateRender | 0xa1 | instruction `mov [rsp+18h],r8`. /  | 11 (0) |  |
| renderer/output_load_action_code.h:30 | kLoadActionClear | 0x2 | Kept apart from the installer so the hos / ERenderTargetLoadAction::EClear | 90 (0) |  |
| renderer/output_load_action_code.h:31 | kLoadActionLoad | 0x1 | Kept apart from the installer so the hos / ERenderTargetLoadAction::ELoad | 155 (0) |  |
| renderer/scene_view_family.h:15 | kViewsData | 0x0 | constructor 0x140CC42D0 and matched to s /  | 2161 (0) |  |
| renderer/scene_view_family.h:16 | kViewsCount | 0x8 | constructor 0x140CC42D0 and matched to s /  | 1705 (0) |  |
| renderer/scene_view_family.h:22 | kSkipFrameNumberIncrement | 0x70 | constructor 0x140CC42D0 and matched to s /  | 515 (0) |  |
| renderer/scene_view_family.h:29 | kAdditionalViewFamily | 0x74 | constructor 0x140CC42D0 and matched to s /  | 170 (0) |  |
| renderer/scene_view_family.h:38 | kUnscaledViewRect | 0x238 | constructor 0x140CC42D0 and matched to s /  | 137 (10) | PrimitiveComponent.CustomDepthStencilValue -> 0x234; SQEX_Bonamik_Component_v2.m_BonamikEnable -> 0x230; AudioComponent.VolumeMultiplier -> 0x230 |
| renderer/view_family_submission.cpp:45 | kRectLoggedViews | 0x4 | still have the pre-display-mode size. / views beyond these are logged but not compared | 542 (0) |  |
| renderer/view_family_submission.cpp:117 | kTimedFamilies | 0x4 | waits inside it. / later families share the last slot | 542 (0) |  |
| views/minimal_view_info.h:11 | kLocation | 0x0 | GetViewPoint's field-by-field copy (stru / FVector, three floats | 2161 (0) |  |
| views/minimal_view_info.h:12 | kPitch | 0xc | GetViewPoint's field-by-field copy (stru / FRotator::Pitch, degrees | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| views/minimal_view_info.h:13 | kYaw | 0x10 | GetViewPoint's field-by-field copy (stru / FRotator::Yaw, degrees (Roll +0x14) | 1283 (0) |  |
| views/minimal_view_info.h:14 | kFov | 0x18 | GetViewPoint's field-by-field copy (stru / float, horizontal degrees; the constructor sets 90 | 795 (1) | ForceFeedbackParameters.EffectID -> 0x1c |
| views/minimal_view_info.h:17 | kAspectRatio | 0x2c | GetViewPoint's field-by-field copy (stru /  | 299 (0) |  |
| views/minimal_view_info.h:22 | kFlags | 0x30 | GetViewPoint's field-by-field copy (stru /  | 1052 (0) |  |
| views/minimal_view_info.h:27 | kPreviousViewTransformSet | 0x690 | GetViewPoint's field-by-field copy (stru /  | 13 (8) | SynthComponent.Synth -> 0x688; EndFeelerConflictActor.AvoidCameraBaseSize -> 0x6b0; EndFeelerSplineActor.SplineDistanceCurve -> 0x6b0 |
| views/player_view_point.cpp:127 | kAxisRecords | 0x4 | (bars) the value left in place makes no  / one per local player the engine allows (4 in stock | 542 (0) |  |
| views/player_view_point.cpp:206 | kFovStillFrames | 0x1e | (bars) the value left in place makes no  /  | 20 (0) |  |
| views/view_shape.h:19 | kMajorAxisFov | 0x2 | MaintainXFOV with the split FOV below).  /  | 90 (0) |  |
| views/view_shape.h:23 | kMaintainXFov | 0x1 | MaintainXFOV with the split FOV below).  /  | 155 (0) |  |
| views/whole_screen.cpp:23 | kState | 0x278 | reads g_AEndGameMode 0x1490FF2B8 +0x288, / uint8: the EndGameState state (+0x279 is its histo | 62 (5) | PrimitiveComponent.RuntimeVirtualTextures -> 0x268; LightComponent.LightFunctionMaterial -> 0x270; ExponentialHeightFogComponent.Medium -> 0x270 |
| views/whole_screen.cpp:24 | kMenuKind | 0x3bc | reads g_AEndGameMode 0x1490FF2B8 +0x288, / uint8: EEndSystemPauseMenuKind | 7 (0) |  |
| views/whole_screen.cpp:29 | kStateManager | 0x170 | and frees), two int32 counts per id; an  /  | 75 (0) |  |
| views/whole_screen.cpp:30 | kOnOffCountA | 0x1a1238c | and frees), two int32 counts per id; an  /  | 0 (0) |  |
| views/whole_screen.cpp:31 | kOnOffCountB | 0x1a1263c | and frees), two int32 counts per id; an  /  | 0 (0) |  |
| views/whole_screen.cpp:38 | kSkippableSceneId | 0xc | scenes while they can be skipped (§7.2). /  | 640 (1) | StaticMeshMeshletBuildOption.bUseMeshOptimizer -> 0xd |
| views/whole_screen.cpp:49 | kLocation | 0x304 | title's, which the game compares against /  | 10 (1) | EndStreamingVolume.ShowStoryFlag -> 0x314 |
| views/whole_screen.cpp:56 | kSkipping | 0xa60 | title's, which the game compares against / uint8: a cutscene skip under way | 3 (0) |  |
| views/whole_screen.h:58 | kStateCinemaPause | 0x7 | The decision is pure, so it is tested on /  | 18 (0) |  |
| views/whole_screen.h:59 | kStatePauseMenu | 0x8 | The decision is pure, so it is tested on /  | 1705 (0) |  |

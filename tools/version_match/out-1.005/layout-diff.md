# Reflected layout diff, 1.004 → 1.005

- classes/structs in both dumps: 9875; with any change: **280** (of which `/Script/End*`: 157)
- only in the 1.004 dump: 0; only in the 1.005 dump: 3 (the dumps were taken at different places, so most of these are just not loaded)
- a moved property means the class (or a base class) gained or lost members before it

| class | moved (first few: prop old→new) | added | removed |
|---|---|---|---|
| `/Script/AnimationBudgetAllocator.SkeletalMeshComponentBudgeted` | bAutoRegisterWithBudgetAllocator 0x1210→0x1200, bAutoCalculateSignificance 0x1210→0x1200, bShouldUseActorRenderedFlag 0x1210→0x1200 | - | - |
| `/Script/AudioMixer.SynthComponent` | bAutoDestroy 0x1f8→0x1f0, bStopWhenOwnerDestroyed 0x1f8→0x1f0, bAllowSpatialization 0x1f8→0x1f0, bOverrideAttenuation 0x1f8→0x1f0 … (22 moved) | - | - |
| `/Script/BonamikRt.SQEX_BonamikWind_Component` | mProps 0x200→0x1f0 | - | - |
| `/Script/BonamikRt.SQEX_BonamikWind_Component_v2` | m_BonamikWindDesc 0x1f8→0x1f0, m_BonamikWindAreaType 0x200→0x1f8, m_BonamikWindGlobalAddition 0x201→0x1f9, m_BonamikWindBoxAreaSize 0x204→0x1fc … (11 moved) | - | - |
| `/Script/BonamikRt.SQEX_Bonamik_Component_v2` | bHasSkeletalMeshSkippedTransforms 0x210→0x208, m_BonamikAsset 0x218→0x210, SkeletalMeshComponent 0x220→0x218, CharacterMovementComponent 0x228→0x220 … (9 moved) | - | - |
| `/Script/ControlRig.ControlRigComponent` | ControlRigClass 0x490→0x480, OnPostInitializeDelegate 0x498→0x488, OnPreSetupDelegate 0x4a8→0x498, OnPostSetupDelegate 0x4b8→0x4a8 … (13 moved) | - | - |
| `/Script/EndDataObject.EndDataTableManual` | - | PlatformType | - |
| `/Script/EndGame.EndActionYuffieBox` | FailLocation 0x7e8→0x808 | - | - |
| `/Script/EndGame.EndAerialIgnoreComponent` | AerialVolumeName 0x1f8→0x1f0, MeshShapeType 0x200→0x1f8, BoxExtent 0x204→0x1fc, SphereRadius 0x210→0x208 | - | - |
| `/Script/EndGame.EndAerialNavBoxComponent` | BoxID 0x4b8→0x4a8 | - | - |
| `/Script/EndGame.EndBattleLevelSequenceActor` | CameraActor 0x3f8→0x418 | - | - |
| `/Script/EndGame.EndBattlePhysicsStaticMeshActor` | DamageSourceCollision 0x7e8→0x808 | - | - |
| `/Script/EndGame.EndBattleSummonTargetPoint` | PointID 0x380→0x3a0, bSpawn_All 0x388→0x3a8, bSpawn_Ifrit 0x389→0x3a9, bSpawn_Chocobo 0x38a→0x3aa … (55 moved) | - | - |
| `/Script/EndGame.EndBattleTalkMuteBox` | TargetMembers 0x388→0x3a8 | - | - |
| `/Script/EndGame.EndBattleVolumeElenaPunchMines` | CliffLocations 0x380→0x3a0, bFinalCliff 0x390→0x3b0, GroupId 0x394→0x3b4, CurrentlyOverlappingActors 0x3d0→0x3f0 | - | - |
| `/Script/EndGame.EndBikeCharacter` | SeSetting 0x14c8→0x14e8 | - | - |
| `/Script/EndGame.EndBikeItemActor` | AddATBGaugeRate 0x740→0x760, AddATBGaugeTime 0x744→0x764, bScoreItem 0x748→0x768, bEngineBoostItem 0x749→0x769 … (6 moved) | - | - |
| `/Script/EndGame.EndBikeSplineComponent` | SplineMetadata 0x590→0x580, SplineJumpAreaData 0x5a0→0x590, EnemySlideMoveLimitWidth 0x5b0→0x5a0, BuggyMaxSlideLength 0x5b4→0x5a4 … (6 moved) | - | - |
| `/Script/EndGame.EndBlackCloakAssetLoadActor` | AssetPackerID 0x380→0x3a0 | - | - |
| `/Script/EndGame.EndBodyCollisionPrimitive` | GroupName 0x4d8→0x4c8, bIgnoreDodgeRoll 0x4e0→0x4d0, bIgnoreBumpNotify 0x4e0→0x4d0, bIgnoreAttackTarget 0x4e0→0x4d0 … (8 moved) | - | - |
| `/Script/EndGame.EndBonamikBoxScaleGroupParamsComponent` | m_Groups 0x4b8→0x4a8, m_Preset 0x4c8→0x4b8, m_ChangeInnerCone 0x4d0→0x4c0, m_InnerConeScale 0x4d4→0x4c4 … (50 moved) | - | - |
| `/Script/EndGame.EndBonamikBoxWindComponent` | WindAsset 0x4c0→0x4b0, Direction 0x4c8→0x4b8, Scale 0x4d4→0x4c4, bDisableWorldWind 0x4d8→0x4c8 … (6 moved) | - | - |
| `/Script/EndGame.EndBonamikBoxWorldWindModifyComponent` | WindAsset 0x4c0→0x4b0, Scale 0x4c8→0x4b8, bEnabled 0x4cc→0x4bc, bDebugDraw 0x4cd→0x4bd | - | - |
| `/Script/EndGame.EndBonamikCapsuleWindComponent` | WindAsset 0x4b8→0x4a8, Direction 0x4c0→0x4b0, Scale 0x4cc→0x4bc, bDisableWorldWind 0x4d0→0x4c0 … (6 moved) | - | - |
| `/Script/EndGame.EndBonamikCapsuleWorldWindModifyComponent` | WindAsset 0x4b8→0x4a8, Scale 0x4c0→0x4b0, bDisableWorldWind 0x4c4→0x4b4, bEnabled 0x4c5→0x4b5 … (5 moved) | - | - |
| `/Script/EndGame.EndBonamikRaycastWindComponent` | WindAsset 0x4d8→0x4c8, RaycastDirection 0x4e0→0x4d0, RaycastLength 0x4ec→0x4dc, WindDirection 0x4f0→0x4e0 … (8 moved) | - | - |
| `/Script/EndGame.EndBonamikSphereWindComponent` | WindAsset 0x4b8→0x4a8, Direction 0x4c0→0x4b0, Scale 0x4cc→0x4bc, bDisableWorldWind 0x4d0→0x4c0 … (6 moved) | - | - |
| `/Script/EndGame.EndBonamikSphereWorldWindModifyComponent` | WindAsset 0x4b8→0x4a8, Scale 0x4c0→0x4b0, bDisableWorldWind 0x4c4→0x4b4, bEnabled 0x4c5→0x4b5 … (5 moved) | - | - |
| `/Script/EndGame.EndCableCartSkeletalMeshActor` | PlugTransform 0x750→0x770, IconTransform 0x780→0x7a0, bUseIcon2 0x7b0→0x7d0, Icon2Transform 0x7c0→0x7e0 … (27 moved) | - | - |
| `/Script/EndGame.EndCardGame3DBoard` | MPC_Player_Status 0x810→0x830, MPC_Player_LanePower1 0x814→0x834, MPC_Player_LanePower2 0x818→0x838, MPC_Player_LanePower3 0x81c→0x83c … (8 moved) | - | - |
| `/Script/EndGame.EndCardGame3DCardBoard` | CardIndex 0x810→0x830, MPC_CardFrame 0x814→0x834, BoardMoveTimer 0x818→0x838, BoardMoveBeforeTransform 0x820→0x840 … (19 moved) | - | - |
| `/Script/EndGame.EndCardGame3DCardHand` | CardIndex 0x810→0x830, MPC_CardHandFront 0x814→0x834, MPC_CardHandBack 0x818→0x838, ScreenAngle 0x81c→0x83c … (13 moved) | - | - |
| `/Script/EndGame.EndCardGame3DManager` | BattleCameraId 0x14c8→0x14e8, BoardCollisionActors 0x14e0→0x1500, CameraLookAt 0x14f0→0x1510, CameraOffset 0x14fc→0x151c … (32 moved) | - | - |
| `/Script/EndGame.EndCardGame3DToken` | MovementComponent 0x740→0x760, bPlayer 0x748→0x768, LaneIndex 0x74c→0x76c, KillZ 0x750→0x770 … (11 moved) | - | - |
| `/Script/EndGame.EndCatchableRopeSkeletalMeshActor` | RopeLength 0x748→0x768, PullActorPtr 0x750→0x770 | - | - |
| `/Script/EndGame.EndCategoryVolumeControlComponent` | Volume 0x1fc→0x1f4, FadeTime 0x200→0x1f8, CategoryName 0x204→0x1fc | - | - |
| `/Script/EndGame.EndCetraCableCartSkeletalMeshActor` | m_SphereRound 0xc38→0xc58, m_CameraSettingId 0xc3c→0xc5c, ReadyStoryFlag 0xc44→0xc64, CompletedStoryFlag 0xc4c→0xc6c … (26 moved) | - | - |
| `/Script/EndGame.EndChangeAppearanceCharacter` | ChangeAppearanceSettings 0x15e8→0x1608, AppearanceResetSetting 0x15f8→0x1618 | - | - |
| `/Script/EndGame.EndCharacter` | bCanFieldAttack 0x14c8→0x14e8, bCreatePhysicsBlockPrimitive 0x14c9→0x14e9, bOverrideBodyDriverTuningSets 0x14f2→0x1512, BodyDriverBaseData 0x14f8→0x1518 … (23 moved) | - | - |
| `/Script/EndGame.EndCharacterBase` | bForceUpdateKinematicBonesUpdateToPhysics 0x11fc→0x121c, bAutoSetMeshRelativeLocation 0x11fd→0x121d, BodyControlRig 0x1200→0x1220, FacialControlRig 0x1208→0x1228 … (29 moved) | - | - |
| `/Script/EndGame.EndCharacterRootComponent` | GroupName 0x4b8→0x4a8, bIgnoreDodgeRoll 0x4c0→0x4b0, bIgnoreAttackTargetAsTarget 0x4c0→0x4b0, bIgnoreAttackTargetAsAttacker 0x4c0→0x4b0 … (6 moved) | - | - |
| `/Script/EndGame.EndCharacterWhizSeComponent` | SoundRef 0x240→0x238, SoundName 0x268→0x260 | - | - |
| `/Script/EndGame.EndChickenLureMiniGameSkeletalCanActor` | m_SkeletalMeshComponent 0x740→0x760 | - | - |
| `/Script/EndGame.EndChocoboCharacter` | PartsMeshComp_Head 0x1870→0x1890, PartsMeshComp_Body 0x1878→0x1898, PartsMeshComp_Legs 0x1880→0x18a0, PartsMeshComp_EquipHead 0x1888→0x18a8 … (12 moved) | - | - |
| `/Script/EndGame.EndChocoboGlideRingOverlapComponent` | Point 0x4b8→0x4a8, VibrationID 0x4bc→0x4ac | - | - |
| `/Script/EndGame.EndChocoboPartsSkeletalMeshComponent` | ColorSettings 0x1478→0x1468 | - | - |
| `/Script/EndGame.EndChocoboRaceBombSearchTriggerBox` | BombActor 0x380→0x3a0, BombActorName 0x388→0x3a8, SearchType 0x390→0x3b0 | - | - |
| `/Script/EndGame.EndChocoboRaceCourseOutTriggerBox` | ReturnPoint 0x380→0x3a0, ReturnPointName 0x388→0x3a8 | - | - |
| `/Script/EndGame.EndChocoboRaceRunEventTriggerBox` | EventType 0x380→0x3a0 | - | - |
| `/Script/EndGame.EndChocoboRaceTrampolineTriggerBox` | JumpTargetPoint 0x380→0x3a0, JumpTargetPointName 0x388→0x3a8 | - | - |
| `/Script/EndGame.EndCinemaEmissiveOverrideComponent` | Intensity 0x220→0x218, EmissiveColor 0x224→0x21c | - | - |
| `/Script/EndGame.EndCinemaMaterialAnimateTimeComponent` | TargetActorName 0x218→0x210, ActorPlacedLevel 0x220→0x218, bNormalized 0x228→0x220, AnimateTime 0x22c→0x224 | - | - |
| `/Script/EndGame.EndCinemaWidgetControlComponent` | WidgetAsset 0x260→0x258, TextFieldValues 0x268→0x260, StoryFlagName 0x2b8→0x2b0, StoryFlags 0x2c0→0x2b8 … (9 moved) | - | - |
| `/Script/EndGame.EndCompositeCollisionComponent` | BodySetup 0x490→0x480 | - | - |
| `/Script/EndGame.EndCrashBoxMiniGameElectricCableActor` | m_CableType 0x488→0x4a8, m_GroupIndex 0x48c→0x4ac, m_CableGimmickDifficultyType 0x490→0x4b0, m_Effect 0x498→0x4b8 … (8 moved) | - | - |
| `/Script/EndGame.EndCrashBoxMiniGameGearBoxSkeletalMeshActor` | m_BoxType 0x810→0x830, m_bHighGreadL 0x811→0x831, m_GroupIndex 0x814→0x834, m_Radius 0x818→0x838 … (11 moved) | - | - |
| `/Script/EndGame.EndCylinderComponent` | CylinderHalfHeight 0x4a8→0x498, CylinderRadius 0x4ac→0x49c, CylinderSides 0x4b0→0x4a0 | - | - |
| `/Script/EndGame.EndDialogueGenerateSourceShot` | Camera 0x38→0x48, Motion 0x228→0x238, Light 0x418→0x428, ShotOverride 0x608→0x618 … (16 moved) | LowLights | - |
| `/Script/EndGame.EndDirectionalTriggerBox` | FrontTriggerName 0x380→0x3a0, BackTriggerName 0x388→0x3a8 | - | - |
| `/Script/EndGame.EndDolphinJumpItemActor` | ItemParameterID 0x740→0x760, OceanFloaterComponent 0x748→0x768 | - | - |
| `/Script/EndGame.EndDolphinJumpOceanFloaterComponent` | EnableDisplacement 0x1f8→0x1f0, EnableRotation 0x1f9→0x1f1 | - | - |
| `/Script/EndGame.EndDolphinJumpPhysicsStaticMeshActor` | EnableTarget 0x7e8→0x808 | - | - |
| `/Script/EndGame.EndDolphinJumpSplineComponent` | Width 0x590→0x580, JumpLineDistance 0x594→0x584, Phases 0x598→0x588 | - | - |
| `/Script/EndGame.EndDynamicBodyCollisionPrimitive` | PushSpeed 0x4e8→0x4d8, IgnoreActorNames 0x4f0→0x4e0 | - | - |
| `/Script/EndGame.EndEmitter` | BeginSoundAudioComponent 0x370→0x390, FinishSoundAudioComponent 0x380→0x3a0, EventSoundAudioComponent 0x390→0x3b0, PlacementSoundAsset 0x3a0→0x3c0 | - | - |
| `/Script/EndGame.EndEnvironmentPhysicsStaticMeshActor` | PhysicsBehavior 0x3c8→0x3e8, InitialDisableMask 0x3c9→0x3e9, ParentCollisionPresetType 0x3ca→0x3ea, PartsCollisionPresetType 0x3cb→0x3eb … (38 moved) | - | - |
| `/Script/EndGame.EndEnvironmentStaticMeshComponent` | bOverrideCollisionExtraAttribute 0x588→0x578, CollisoinExtraAttribute 0x58c→0x57c | - | - |
| `/Script/EndGame.EndFA0122RobotArmCharacter` | Turning 0x15e8→0x1608, StopTurning 0x1620→0x1640, TurnAttachBoneName 0x1658→0x1678, Elevating 0x1660→0x1680 … (14 moved) | - | - |
| `/Script/EndGame.EndFA0150ContainerCharacter` | PutOnFloor 0x15e8→0x1608, PutOnContainer 0x1620→0x1640, AttachBoneName 0x1658→0x1678, EnvironmentHit 0x1660→0x1680 … (9 moved) | - | - |
| `/Script/EndGame.EndFeelerAttendantActor` | InstanceCount 0x528→0x548, RandomScaleRange 0x52c→0x54c, RandomSeed 0x534→0x554, InstanceContexts 0x538→0x558 … (29 moved) | - | - |
| `/Script/EndGame.EndFeelerBarrierActor` | InstanceCount 0x528→0x548, LineCount 0x52c→0x54c, RandomScaleRange 0x530→0x550, RandomSeed 0x538→0x558 … (46 moved) | - | - |
| `/Script/EndGame.EndFeelerConflictActor` | SlaveInstanceCount 0x528→0x548, LineCount 0x52c→0x54c, RandomScaleRange 0x530→0x550, RandomSeed 0x538→0x558 … (51 moved) | - | - |
| `/Script/EndGame.EndFeelerFollowingActor` | InstanceCount 0x528→0x548, RandomScaleRange 0x52c→0x54c, RandomSeed 0x534→0x554, InstanceContexts 0x538→0x558 … (30 moved) | - | - |
| `/Script/EndGame.EndFeelerLaserActor` | SlaveInstanceCount 0x528→0x548, LineCount 0x52c→0x54c, RandomScaleRange 0x530→0x550, ActorMovementApplyRate 0x538→0x558 … (40 moved) | - | - |
| `/Script/EndGame.EndFeelerSplineActor` | InstanceCount 0x528→0x548, LineCount 0x52c→0x54c, RandomScaleRange 0x530→0x550, BeginScale 0x538→0x558 … (61 moved) | - | - |
| `/Script/EndGame.EndFeelerTornadeActor` | InstanceCount 0x528→0x548, LineCount 0x52c→0x54c, RandomScaleRange 0x530→0x550, ActorMovementApplyRate 0x538→0x558 … (62 moved) | - | - |
| `/Script/EndGame.EndFieldActionDamageReciveComponent` | DamageReciveInfo 0x1f8→0x1f0 | - | - |
| `/Script/EndGame.EndFieldActionShootingTargetComponent` | TargetGroupName 0x208→0x200, TargetEnableRadius 0x210→0x208, YuffiTargetEnableRadius 0x214→0x20c, TargetEnableCamDegrees 0x218→0x210 … (15 moved) | - | - |
| `/Script/EndGame.EndFieldActionSplineComponent` | UpVectorSpline 0x590→0x580 | - | - |
| `/Script/EndGame.EndFieldActionTriggerComponent` | ActionType 0x4b8→0x4a8, IconOffsetLocation 0x4bc→0x4ac, IconOffsetRotation 0x4c8→0x4b8, CustomParam 0x4e0→0x4d0 | - | - |
| `/Script/EndGame.EndFieldAddHookShotAllAngleTriggerBoxMaker` | TriggerBox 0x1f8→0x1f0 | - | - |
| `/Script/EndGame.EndFieldAddTriggerBoxMaker` | TriggerBox 0x1f8→0x1f0 | - | - |
| `/Script/EndGame.EndFieldAddWallRunningTriggerBoxMaker` | bBlockBoxOutMove 0x200→0x1f8, bSetJumpDownPoint 0x201→0x1f9, JumpDownPointPriority 0x202→0x1fa, JumpDownPoint 0x204→0x1fc | - | - |
| `/Script/EndGame.EndFieldAdditionalAssetComponent` | SoundBank 0x1f8→0x1f0 | - | - |
| `/Script/EndGame.EndFieldClimbingAttachPointSetting` | bStartOrFinishPoint 0x2b0→0x2a0, bStartIsTopEdge 0x2b1→0x2a1, bDisbleStart 0x2b2→0x2a2, AIBPActionTag_Finish 0x2b4→0x2a4 … (7 moved) | - | - |
| `/Script/EndGame.EndFieldClimbingCatchPointSetting` | LocalLocator 0x270→0x260, JumpPoints 0x2a0→0x290 | - | - |
| `/Script/EndGame.EndFieldClimbingFinishPointSetting` | bFinishIsForceStepOn 0x2b0→0x2a0, bLongFallPoint 0x2b1→0x2a1, bShowEnterEffect 0x2b2→0x2a2, bAutoSnapEnterEffect 0x2b3→0x2a3 … (11 moved) | - | - |
| `/Script/EndGame.EndFieldClimbingPointSettingBase` | EnableCatchStoryFlag 0x1f8→0x1f0, StartTrigger 0x200→0x1f8, EndTrigger 0x208→0x200, JumpFinishTrigger 0x210→0x208 … (12 moved) | - | - |
| `/Script/EndGame.EndFieldCustomParkourRuleVolumeComponent` | bEnableJumpUpHeightMax 0x4b8→0x4a8, EnableJumpUpHeightMax 0x4bc→0x4ac, bEnableJumpUpPlaneDistanceMax 0x4c0→0x4b0, EnableJumpUpPlaneDistanceMax 0x4c4→0x4b4 … (25 moved) | - | - |
| `/Script/EndGame.EndFieldForceParkourSplineAttachComponent` | AttachParkourName 0x590→0x580, bUseGuidEffect 0x598→0x588, GuidEffectTransform 0x5a0→0x590, AIBPActionTag_Start 0x5d0→0x5c0 … (5 moved) | - | - |
| `/Script/EndGame.EndFieldForceParkourSplineComponent` | bUseGuidEffect 0x590→0x580, GuidEffectTransform 0x5a0→0x590 | - | - |
| `/Script/EndGame.EndFieldMineCartRailComponent` | RailName 0x1f8→0x1f0, NextRails 0x200→0x1f8, SplineComponent 0x210→0x208, RailInMineCartAllTrigger 0x218→0x210 … (6 moved) | - | - |
| `/Script/EndGame.EndFieldMineCartTargetComponent` | MineCartActor 0x1f8→0x1f0, DisablePushFromFront 0x200→0x1f8, DisablePushFromBack 0x201→0x1f9 | - | - |
| `/Script/EndGame.EndFieldMovementControlSplineComponent` | bSnapPosition 0x590→0x580, bSnapRotation 0x591→0x581 | - | - |
| `/Script/EndGame.EndFieldMovementSettingVolumeComponent` | MovementSettingFlag 0x4b8→0x4a8, WaterMoveDepth 0x4bc→0x4ac, SwimmingWalkableAngle 0x4c0→0x4b0, HitToLookDownVolumeName 0x4c4→0x4b4 … (6 moved) | - | - |
| `/Script/EndGame.EndFieldMovingWalkwaySplineComponent` | AutoMovingSpeed 0x590→0x580 | - | - |
| `/Script/EndGame.EndFieldNavWorldLinkAttrIgnoreBoxMaker` | NavWorldLinkAttrIgnoreBoxs 0x1f8→0x1f0 | - | - |
| `/Script/EndGame.EndFieldPlugCableStartupInfo` | ActiveStoryFlag 0x590→0x580, ActiveLocationWorkIntegerName 0x598→0x588, ActiveLocationWorkInteger 0x5a0→0x590, FromTransform 0x5b0→0x5a0 … (8 moved) | - | - |
| `/Script/EndGame.EndFieldSplineMoveGateSpline` | bDisableEnterSpline 0x590→0x580, bDisableLeaveSpline 0x591→0x581 | - | - |
| `/Script/EndGame.EndFieldVaultActionLineComponent` | CatchLineDirs 0x590→0x580 | - | - |
| `/Script/EndGame.EndFrogGuysTriggerComponent` | bForceJump 0x4b8→0x4a8, JumpVelocity 0x4bc→0x4ac, JumpGravityScale 0x4c0→0x4b0, bIsToKnockback 0x4c4→0x4b4 … (21 moved) | - | - |
| `/Script/EndGame.EndGatherableLifeCharacter` | m_Worker 0x15e8→0x1608 | - | - |
| `/Script/EndGame.EndInstancedLightweightPhysicsComponent` | Radius 0x630→0x620, ReserveCount 0x634→0x624, InitialTranslationNoise 0x638→0x628, InitialRotationNoise 0x644→0x634 … (19 moved) | - | - |
| `/Script/EndGame.EndJenovaPulseWallActor` | InstanceCount 0x528→0x548, LineCount 0x52c→0x54c, RandomScaleRange 0x530→0x550, RandomSeed 0x538→0x558 … (21 moved) | - | - |
| `/Script/EndGame.EndJenovaSplineWallActor` | RandomScaleRange 0x528→0x548, RandomSeed 0x530→0x550, PlacementOffset 0x534→0x554, Roll 0x538→0x558 … (16 moved) | - | - |
| `/Script/EndGame.EndMainSaveLoadTopMenu` | Throbber 0x468→0x470 | - | - |
| `/Script/EndGame.EndManualPumpAssetData` | - | AssetType | - |
| `/Script/EndGame.EndMobRouteSplineComponent` | MobRouteSplineDefaults 0x5c0→0x5b0, PreviousMobRouteSplineDefaults 0x5c4→0x5b4, MobRouteSplineMetadata 0x5c8→0x5b8, Width 0x5d0→0x5c0 … (5 moved) | - | - |
| `/Script/EndGame.EndModularCharacter` | TopsMesh 0x14c8→0x14e8, BottomsMesh 0x14d0→0x14f0, HairMesh 0x14d8→0x14f8, HaircovMesh 0x14e0→0x1500 … (9 moved) | - | - |
| `/Script/EndGame.EndModularEnemyCharacter` | BodyMesh 0x15e8→0x1608 | - | - |
| `/Script/EndGame.EndMonkeyBarCreateBoxComponent` | BoxID 0x4b8→0x4a8 | - | - |
| `/Script/EndGame.EndMoogleCaptureGimmick` | m_EffectTarget 0x15e8→0x1608, m_RemoveTarget 0x15e9→0x1609, m_EffectRadius 0x15ec→0x160c, m_BreakRadius 0x15f0→0x1610 … (20 moved) | - | - |
| `/Script/EndGame.EndMoogleGimmickSwitchActor` | TargetGimmick 0x810→0x830 | - | - |
| `/Script/EndGame.EndMooglePanicTriggerBox` | TriggerEffect 0x380→0x3a0 | - | - |
| `/Script/EndGame.EndMotionSwitchBox` | MotionSwitchName 0x380→0x3a0, bIgnoreLowerPriorityAnimSet 0x388→0x3a8, BeginOverlapPlayActionName 0x38c→0x3ac, EndOverlapPlayActionName 0x394→0x3b4 | - | - |
| `/Script/EndGame.EndMultiShapeComponent` | CollisionShapes 0x490→0x480, ShapeBodySetup 0x4a0→0x490 | - | - |
| `/Script/EndGame.EndNavBlockingModifierVolume` | IsFirstStart 0x3d8→0x3f8, AerialIgnoreComponent 0x3e0→0x400, CanEverAerial 0x3e8→0x408 | - | - |
| `/Script/EndGame.EndNavModifierAdditionalLayerVolume` | LayerType 0x3a8→0x3c8, bCreateNav 0x3a9→0x3c9 | - | - |
| `/Script/EndGame.EndNavModifierFootBGMVolume` | _footBGMSetID 0x3a8→0x3c8 | - | - |
| `/Script/EndGame.EndNavModifierInDoorVolume` | CameraType 0x3a8→0x3c8, OverrideCameraSettingID 0x3ac→0x3cc, OverridePriorityType 0x3b4→0x3d4, SpecificMoveType 0x3b5→0x3d5 … (5 moved) | - | - |
| `/Script/EndGame.EndNavModifierVolume` | Type 0x3a8→0x3c8, WallAffiliationID 0x3ac→0x3cc, Wall 0x3b0→0x3d0, Group 0x3b4→0x3d4 … (12 moved) | - | - |
| `/Script/EndGame.EndNavMultiSwitchingVolume` | NavPriorInfo 0x330→0x350, EndMultiBoxComponent 0x358→0x378, ModifierVolume 0x360→0x380 | - | - |
| `/Script/EndGame.EndNavSimpleWallVolume` | CylinderInnerRadius 0x3f0→0x410 | - | - |
| `/Script/EndGame.EndNavSwitchingVolume` | NavModifierVolume 0x3a8→0x3c8, bRefreshNav 0x3b0→0x3d0, bNavCreateObbCheck 0x3b1→0x3d1, bUpdateNavArea 0x3b2→0x3d2 … (15 moved) | - | - |
| `/Script/EndGame.EndNavWheelieBlockWallVolume` | TriggerName 0x3f8→0x418, TriggerInterval 0x400→0x420 | - | - |
| `/Script/EndGame.EndPBDIgnoreCollisionComponent` | ExtrusionVectorAsCollsion 0x4b8→0x4a8, bIgnoreEdge 0x4c4→0x4b4, CollisionGroup 0x4c8→0x4b8 | - | - |
| `/Script/EndGame.EndPlannerEventSkipBox` | TargetTalkIDs 0x380→0x3a0 | - | - |
| `/Script/EndGame.EndPlayerCharacter` | PlayerConfig 0x1748→0x1768 | - | - |
| `/Script/EndGame.EndPlayerOverlapSplineComponent` | FalloffStartDistance 0x590→0x580, DisengageDistance 0x594→0x584, FalloffExtraHeight 0x598→0x588 | - | - |
| `/Script/EndGame.EndRedHoldDeliverySkeletalMeshActor` | IconTransform 0x740→0x760, RedAssetUserData 0x770→0x790 | - | - |
| `/Script/EndGame.EndRopeActor` | bEnabledPhysicsCollision 0x740→0x760, AutoConstraintOnAttach 0x748→0x768 | - | - |
| `/Script/EndGame.EndSeismicSensorActor` | _SEPeriod 0x740→0x760, _EffectPeriod 0x7c8→0x7e8, _NegativeEffectPeriod 0x850→0x870, _ConcurrencyAsset 0x8d8→0x8f8 … (7 moved) | - | - |
| `/Script/EndGame.EndSelfBalancingScooterCharacter` | MovementSetting 0x1870→0x1890, ScooterColor 0x1878→0x1898, SeSetting 0x1880→0x18a0, RiderActor 0x2210→0x2230 | - | - |
| `/Script/EndGame.EndShapeComponent` | ShapeType 0x490→0x480, ShapeMesh 0x498→0x488, ShapeSize 0x4a0→0x490, ShapeBodySetup 0x4b0→0x4a0 | - | - |
| `/Script/EndGame.EndShootingCoasterEventTriggerBox` | RailActorReference 0x380→0x3a0, bDefaultActiveState 0x388→0x3a8, ActivateOnStateTrigger 0x38c→0x3ac, DeactivateOnStateTrigger 0x394→0x3b4 … (7 moved) | - | - |
| `/Script/EndGame.EndShootingCoasterSpawnerTriggerBox` | bPrioritySpawner 0x380→0x3a0, RailActorReference 0x388→0x3a8, bDefaultActiveState 0x390→0x3b0, ActivateOnStateTrigger 0x394→0x3b4 … (9 moved) | - | - |
| `/Script/EndGame.EndShootingCoasterSpeedTriggerBox` | RailActorReference 0x380→0x3a0, MaxLength 0x388→0x3a8, AtLength 0x38c→0x3ac, BoxTag 0x390→0x3b0 … (6 moved) | - | - |
| `/Script/EndGame.EndSingleSidedSplineComponent` | bValidRightSide 0x590→0x580 | - | - |
| `/Script/EndGame.EndSkeletalMeshComponent` | bAutoSyncAnimationOnAttach 0x1218→0x1208, bForceUpdateAnimation 0x1219→0x1209, DisableUpdateCondition 0x121a→0x120a, bIgnoreCameraDistanceOptimization 0x121b→0x120b | - | - |
| `/Script/EndGame.EndSmoothTerrainCollisionComponent` | BodySetup 0x490→0x480 | - | - |
| `/Script/EndGame.EndSpiderWallEntryAreaBoxComponent` | bDisableEntry 0x4b8→0x4a8, EntryEffectTransform 0x4c0→0x4b0, bHideEntryEffect 0x4f0→0x4e0, bCachedAroundFixCatchTrans 0x4f1→0x4e1 … (8 moved) | - | - |
| `/Script/EndGame.EndStreamingSwitchVolume` | SwitchType 0x338→0x348, AreaSwitchLabel 0x33c→0x34c, NoneSwitchLabel 0x344→0x354, SwitchLevelSets 0x350→0x360 | - | - |
| `/Script/EndGame.EndStreamingVolume` | DataAssets 0x2d8→0x2e8, VolumeLabel 0x2e8→0x2f8, ActivateProgress 0x2f0→0x300, ShowProgress 0x2f4→0x304 … (10 moved) | ReplaceLevelSets | - |
| `/Script/EndGame.EndSummonShrineGuideActor` | EffectNameOnBreak 0x7f0→0x810, TriggerNameOnBreak 0x7f8→0x818, OverlapComponent 0x800→0x820, ReActivateDistance 0x808→0x828 … (10 moved) | - | - |
| `/Script/EndGame.EndSwitchableBodySkeletalMeshComponent` | SkeletalMeshes 0x12d0→0x12c0 | - | - |
| `/Script/EndGame.EndSwitchableSkeletalMeshComponent` | SkeletalMeshes 0x1478→0x1468 | - | - |
| `/Script/EndGame.EndSwitchableSkeletalMeshPair` | - | VisiblePumpLOD | - |
| `/Script/EndGame.EndSyncPoseSkeletalMeshComponent` | SyncPoseTarget 0x12d0→0x12c0, SyncPoseRootBoneName 0x12d8→0x12c8, bAttachParentIsMasterPose 0x12e0→0x12d0 | - | - |
| `/Script/EndGame.EndThrowableReceiverEnvironmentSkeletalMeshActor` | StoryFlag 0x810→0x830, bOnlyTriggerOnce 0x818→0x838 | - | - |
| `/Script/EndGame.EndTinyBroncoCharacter` | MovementSetting 0x1870→0x1890, SeSetting 0x1878→0x1898 | - | - |
| `/Script/EndGame.EndTreasureBreakBox` | bDestroyWithoutBreak 0x380→0x3a0 | - | - |
| `/Script/EndGame.EndTreasureCrystalLifestream` | m_EmitStoryFlag 0x7f0→0x810 | - | - |
| `/Script/EndGame.EndTurnBackBox` | m_pTurnBackTriggerBox 0x380→0x3a0, m_pFollowDiscardTriggerBox 0x388→0x3a8, m_pBlockWall 0x390→0x3b0, EnableBlockWall 0x398→0x3b8 … (6 moved) | - | - |
| `/Script/EndGame.EndUniqueSimpleCollisionComponent` | BodySetup 0x490→0x480 | - | - |
| `/Script/EndGame.EndWallNavBoxComponent` | BoxID 0x4b8→0x4a8 | - | - |
| `/Script/EndGame.EndWaterFlowSplineComponent` | FlowSpeeds 0x590→0x580 | - | - |
| `/Script/EndGame.EndWaterStaticMeshVolume` | WaterType 0x3d0→0x3f0, WaterFlowComponents 0x3d8→0x3f8, BuildType 0x3e8→0x408 | - | - |
| `/Script/EndGame.EndWeaponSkeletalMeshActor` | PlayAnimationNames 0x740→0x760, bEndAnimationLooping 0x750→0x770, MateriaMaterialIndex 0x754→0x774 | - | - |
| `/Script/EndGame.EndWireReelActor` | BoneAttach 0x740→0x760, WireReelAnimToPlay 0x748→0x768 | - | - |
| `/Script/EndGame.EndWorldReportActor` | DisplayMaterialIndex 0x740→0x760, CoordinateAnimationDuration 0x744→0x764, DisplayMeshComponent 0x748→0x768, MovieMeshComponent 0x750→0x770 … (5 moved) | - | - |
| `/Script/EndGame.ToroccoYuffieTriggerBox` | TargetBox 0x380→0x3a0, TotalTime 0x388→0x3a8, Tolerance_MinimumPassTime 0x38c→0x3ac, MinimumPassTime 0x390→0x3b0 … (5 moved) | - | - |
| `/Script/EndGame.WallRunningBlockBoxVolume` | bForceHitToJump 0x4b8→0x4a8, bDisableWallCorrect 0x4b9→0x4a9 | - | - |
| `/Script/EndGame.WallRunningCustomParamVolume` | bUseWakeLimitMoveDir 0x4b8→0x4a8, WakeLimitMoveRot 0x4bc→0x4ac | - | - |
| `/Script/Engine.Actor` | InputPriority 0xfc→0xa4 | - | - |
| `/Script/Engine.ArrowComponent` | ArrowColor 0x490→0x480, ArrowSize 0x494→0x484, ArrowLength 0x498→0x488, ScreenSize 0x49c→0x48c … (6 moved) | - | - |
| `/Script/Engine.AtmosphericFogComponent` | SunMultiplier 0x1f8→0x1f0, FogMultiplier 0x1fc→0x1f4, DensityMultiplier 0x200→0x1f8, DensityOffset 0x204→0x1fc … (18 moved) | - | - |
| `/Script/Engine.AudioComponent` | Sound 0x1f8→0x1f0, InstanceParameters 0x200→0x1f8, SoundClassOverride 0x210→0x208, bAutoDestroy 0x218→0x210 … (45 moved) | - | - |
| `/Script/Engine.BillboardComponent` | Sprite 0x490→0x480, bIsScreenSizeScaled 0x498→0x488, ScreenSize 0x49c→0x48c, U 0x4a0→0x490 … (7 moved) | - | - |
| `/Script/Engine.BoxComponent` | BoxExtent 0x4a8→0x498, LineThickness 0x4b4→0x4a4 | - | - |
| `/Script/Engine.BoxReflectionCaptureComponent` | BoxTransitionDistance 0x280→0x278, PreviewInfluenceBox 0x288→0x280, PreviewCaptureBox 0x290→0x288 | - | - |
| `/Script/Engine.BrushComponent` | Brush 0x490→0x480, BrushBodySetup 0x498→0x488 | - | - |
| `/Script/Engine.CameraComponent` | FieldOfView 0x1f8→0x1f0, OrthoWidth 0x1fc→0x1f4, OrthoNearClipPlane 0x200→0x1f8, OrthoFarClipPlane 0x204→0x1fc … (10 moved) | - | - |
| `/Script/Engine.CameraShakeSourceComponent` | Attenuation 0x1f8→0x1f0, InnerAttenuationRadius 0x1fc→0x1f4, OuterAttenuationRadius 0x200→0x1f8, CameraShake 0x208→0x200 … (5 moved) | - | - |
| `/Script/Engine.CapsuleComponent` | CapsuleHalfHeight 0x4a8→0x498, CapsuleRadius 0x4ac→0x49c | - | - |
| `/Script/Engine.ChildActorComponent` | ChildActorClass 0x1f8→0x1f0, ChildActor 0x200→0x1f8, ChildActorTemplate 0x208→0x200 | - | - |
| `/Script/Engine.CollisionAssetComponent` | CollisionAsset 0x490→0x480 | - | - |
| `/Script/Engine.DecalComponent` | DecalMaterial 0x1f8→0x1f0, SortOrder 0x200→0x1f8, FadeScreenSize 0x204→0x1fc, FadeStartDelay 0x208→0x200 … (9 moved) | - | - |
| `/Script/Engine.DirectionalLightComponent` | ShadowCascadeBiasDistribution 0x610→0x620, bEnableLightShaftOcclusion 0x614→0x624, OcclusionMaskDarkness 0x618→0x628, OcclusionDepthRange 0x61c→0x62c … (43 moved) | - | - |
| `/Script/Engine.DrawFrustumComponent` | FrustumColor 0x490→0x480, FrustumAngle 0x494→0x484, FrustumAspectRatio 0x498→0x488, FrustumStartDist 0x49c→0x48c … (6 moved) | - | - |
| `/Script/Engine.ExponentialHeightFogComponent` | ContextA 0x1f8→0x1f0, Transmit 0x20c→0x204, Region 0x214→0x20c, Light 0x218→0x210 … (8 moved) | - | - |
| `/Script/Engine.FXSystemComponent` | m_EndEmitterMaterialMap 0x490→0x480, m_VfxCategory 0x4e0→0x4d0 | - | - |
| `/Script/Engine.ForceFeedbackComponent` | ForceFeedbackEffect 0x1f8→0x1f0, bAutoDestroy 0x200→0x1f8, bStopWhenOwnerDestroyed 0x200→0x1f8, bLooping 0x200→0x1f8 … (10 moved) | - | - |
| `/Script/Engine.ForceFeedbackParameters` | EffectID 0x18→0x1c | - | - |
| `/Script/Engine.HierarchicalInstancedStaticMeshComponent` | SortedInstances 0x640→0x630, NumBuiltInstances 0x650→0x640, BuiltInstanceBounds 0x658→0x648, UnbuiltInstanceBounds 0x674→0x664 … (10 moved) | - | - |
| `/Script/Engine.InstancedStaticMeshComponent` | PerInstanceSMData 0x588→0x578, NumCustomDataFloats 0x598→0x588, PerInstanceSMCustomData 0x5a0→0x590, InstancingRandomSeed 0x5b0→0x5a0 … (9 moved) | - | - |
| `/Script/Engine.LightComponent` | MaxDrawDistance 0x228→0x220, MaxDistanceFadeRange 0x22c→0x224, MaxDrawShadowDistance 0x230→0x228, MaxDistanceStaticShadowMap 0x234→0x22c … (38 moved) | - | - |
| `/Script/Engine.LightComponentBase` | LightGuid 0x1f8→0x1f0, Brightness 0x208→0x200, Intensity 0x20c→0x204, LightColor 0x210→0x208 … (20 moved) | - | - |
| `/Script/Engine.LightmassPortalComponent` | PreviewBox 0x1f8→0x1f0 | - | - |
| `/Script/Engine.LocalLightComponent` | IntensityUnits 0x610→0x620, Radius 0x614→0x624, AttenuationRadius 0x618→0x628, LightmassSettings 0x61c→0x62c | - | - |
| `/Script/Engine.MapBuildDataRegistry` | - | MassiveEnvironmentTextures | - |
| `/Script/Engine.MassiveEnvironmentComponent` | bHasStreamingUpdatePending 0x1fc→0x1f4, StreamingProxy 0x200→0x1f8 | LODGroup | - |
| `/Script/Engine.MassiveEnvironmentSourceMeshesComponent` | MassiveEnvironmentSourceMeshes 0x490→0x480, VisibleDistance 0x498→0x488 | - | - |
| `/Script/Engine.MaterialBillboardComponent` | Elements 0x490→0x480 | - | - |
| `/Script/Engine.MeshComponent` | OverrideMaterials 0x490→0x480, DynamicMaterialsForParameters 0x4a0→0x490, bEnableMaterialParameterCaching 0x509→0x4f9 | - | - |
| `/Script/Engine.ModelComponent` | ModelBodySetup 0x4a8→0x498 | - | - |
| `/Script/Engine.OceanComponent` | OceanSize 0x510→0x500, CoordinateDistance 0x518→0x508, CoordinateCenter 0x520→0x510, MaterialInterface 0x528→0x518 … (5 moved) | - | - |
| `/Script/Engine.ParticleSystemComponent` | Template 0x638→0x628, EmitterMaterials 0x640→0x630, SkelMeshComponents 0x650→0x640, bResetOnDetach 0x661→0x651 … (33 moved) | - | - |
| `/Script/Engine.PhysicsConstraintComponent` | ConstraintActor1 0x1f8→0x1f0, ComponentName1 0x200→0x1f8, ConstraintActor2 0x208→0x200, ComponentName2 0x210→0x208 … (7 moved) | - | - |
| `/Script/Engine.PhysicsSpringComponent` | SpringStiffness 0x1f8→0x1f0, SpringDamping 0x1fc→0x1f4, SpringLengthAtRest 0x200→0x1f8, SpringRadius 0x204→0x1fc … (7 moved) | - | - |
| `/Script/Engine.PhysicsThrusterComponent` | ThrustStrength 0x1f8→0x1f0 | - | - |
| `/Script/Engine.PlanarReflectionComponent` | PreviewBox 0x2b0→0x2a8, NormalDistortionStrength 0x2b8→0x2b0, PrefilterRoughness 0x2bc→0x2b4, PrefilterRoughnessDistance 0x2c0→0x2b8 … (14 moved) | - | - |
| `/Script/Engine.PlaneReflectionCaptureComponent` | InfluenceRadiusScale 0x280→0x278, PreviewInfluenceRadius 0x288→0x280, PreviewCaptureBox 0x290→0x288 | - | - |
| `/Script/Engine.PointLightComponent` | bUseInverseSquaredFalloff 0x628→0x638, bAmbiguousLight 0x628→0x638, LightFalloffExponent 0x62c→0x63c, SourceRadius 0x630→0x640 … (7 moved) | - | - |
| `/Script/Engine.PrecomputedBrightnessEnvironmentComponent` | ExposureBiasShift 0x1f8→0x1f0, ExposureBiasMinimum 0x1fc→0x1f4, ExposureBiasMaximum 0x200→0x1f8, TransitionDistance 0x204→0x1fc … (5 moved) | - | - |
| `/Script/Engine.PrecomputedLightEnvironmentComponent` | BlendTarget 0x1f8→0x1f0, Weight 0x200→0x1f8 | - | - |
| `/Script/Engine.PrefabStaticMeshComponent` | PrefabStaticMesh 0x490→0x480 | - | - |
| `/Script/Engine.PrimitiveComponent` | MassiveEnvironmentImportSettings 0x200→0x1f8, MinDrawDistance 0x218→0x210, LDMaxDrawDistance 0x21c→0x214, CachedMaxDrawDistance 0x220→0x218 … (97 moved) | - | - |
| `/Script/Engine.RadialForceComponent` | Radius 0x1f8→0x1f0, Falloff 0x1fc→0x1f4, ImpulseStrength 0x200→0x1f8, bImpulseVelChange 0x204→0x1fc … (8 moved) | - | - |
| `/Script/Engine.RectLightComponent` | SourceWidth 0x628→0x638, SourceHeight 0x62c→0x63c, BarnDoorAngle 0x630→0x640, BarnDoorLength 0x634→0x644 … (5 moved) | - | - |
| `/Script/Engine.ReflectionCaptureComponent` | CaptureOffsetComponent 0x1f8→0x1f0, RaytracedReflectionMesh 0x200→0x1f8, TextureSize 0x208→0x200, ReflectionSourceType 0x20c→0x204 … (14 moved) | - | - |
| `/Script/Engine.RuntimeVirtualTextureComponent` | BoundsAlignActor 0x1f8→0x1f0, bSetBoundsButton 0x220→0x218, bSnapBoundsToLandscape 0x221→0x219, VirtualTexture 0x228→0x220 … (13 moved) | - | - |
| `/Script/Engine.SceneCaptureComponent` | PrimitiveRenderMode 0x1f8→0x1f0, CaptureSource 0x1f9→0x1f1, bCaptureEveryFrame 0x1fa→0x1f2, bCaptureOnMovement 0x1fa→0x1f2 … (15 moved) | - | - |
| `/Script/Engine.SceneCaptureComponent2D` | ProjectionType 0x2b0→0x2a8, FOVAngle 0x2b4→0x2ac, OrthoWidth 0x2b8→0x2b0, TextureTarget 0x2c0→0x2b8 … (5 moved) | - | - |
| `/Script/Engine.SceneCaptureComponentCube` | TextureTarget 0x2b0→0x2a8, bCaptureRotation 0x2b8→0x2b0, TextureTargetLeft 0x2c0→0x2b8, TextureTargetRight 0x2c8→0x2c0 … (6 moved) | - | - |
| `/Script/Engine.ShapeComponent` | ShapeBodySetup 0x490→0x480, AreaClass 0x498→0x488, ShapeColor 0x4a0→0x490, bDrawOnlyIfSelected 0x4a4→0x494 … (6 moved) | - | - |
| `/Script/Engine.SkeletalMesh` | SkelMirrorAxis 0x17e→0x184, SkelMirrorFlipAxis 0x17f→0x185, bUseFullPrecisionUVs 0x180→0x186, bUseHighPrecisionTangentBasis 0x180→0x186 … (7 moved) | LODRadiusScaleLimit | - |
| `/Script/Engine.SkeletalMeshComponent` | AnimBlueprintGeneratedClass 0x830→0x820, AnimClass 0x838→0x828, AnimScriptInstance 0x840→0x830, PostProcessAnimInstance 0x848→0x838 … (76 moved) | - | - |
| `/Script/Engine.SkeletalMeshLODInfo` | - | PerformanceLowVisibilityLevels | - |
| `/Script/Engine.SkinnedMeshComponent` | SkeletalMesh 0x518→0x508, MasterPoseComponent 0x520→0x510, SkinCacheUsage 0x528→0x518, VertexOffsetUsage 0x538→0x528 … (36 moved) | LODRadiusScaleLimit | - |
| `/Script/Engine.SkyAtmosphereComponent` | TransformMode 0x1f8→0x1f0, BottomRadius 0x1fc→0x1f4, GroundAlbedo 0x200→0x1f8, AtmosphereHeight 0x204→0x1fc … (24 moved) | - | - |
| `/Script/Engine.SkyLightComponent` | bRealTimeCapture 0x228→0x220, SourceType 0x229→0x221, Cubemap 0x230→0x228, SourceCubemapAngle 0x238→0x230 … (20 moved) | - | - |
| `/Script/Engine.SphereComponent` | SphereRadius 0x4a8→0x498 | - | - |
| `/Script/Engine.SphereReflectionCaptureComponent` | InfluenceRadius 0x280→0x278, CaptureDistanceScale 0x284→0x27c, PreviewInfluenceRadius 0x288→0x280 | - | - |
| `/Script/Engine.SplineComponent` | SplineCurves 0x490→0x480, SplineInfo 0x500→0x4f0, SplineRotInfo 0x518→0x508, SplineScaleInfo 0x530→0x520 … (17 moved) | - | - |
| `/Script/Engine.SplineMeshComponent` | SplineParams 0x590→0x580, SplineUpDir 0x5e8→0x5d8, SplineBoundaryMin 0x5f4→0x5e4, CachedMeshBodySetupGuid 0x5f8→0x5e8 … (11 moved) | - | - |
| `/Script/Engine.SpotLightComponent` | InnerFullConeAngle 0x648→0x658, OuterFullConeAngle 0x64c→0x65c | - | - |
| `/Script/Engine.SpringArmComponent` | TargetArmLength 0x1f8→0x1f0, SocketOffset 0x1fc→0x1f4, TargetOffset 0x208→0x200, ProbeSize 0x214→0x20c … (18 moved) | - | - |
| `/Script/Engine.StaticMesh` | - | bEnableScreenSizeCulling | - |
| `/Script/Engine.StaticMeshComponent` | ForcedLodModel 0x510→0x500, PreviousLODLevel 0x514→0x504, MinLOD 0x518→0x508, SubDivisionStepSize 0x51c→0x50c … (30 moved) | - | - |
| `/Script/Engine.StaticMeshMeshletBuildOption` | bUseMeshOptimizer 0xc→0xd | bUseCustomMeshletSizeOption, bUseCustomMeshOptimizerOption, bUseOptimizeMeshlet, bUseCustomOverrideMeshletLODScale … | bUseMeshletBuildOption |
| `/Script/Engine.StereoLayerComponent` | bLiveTexture 0x1f8→0x1f0, bSupportsDepth 0x1f8→0x1f0, bNoAlphaChannel 0x1f8→0x1f0, Texture 0x200→0x1f8 … (16 moved) | - | - |
| `/Script/Engine.TextRenderComponent` | Text 0x490→0x480, TextMaterial 0x4a8→0x498, Font 0x4b0→0x4a0, HorizontalAlignment 0x4b8→0x4a8 … (13 moved) | - | - |
| `/Script/Engine.VectorFieldComponent` | VectorField 0x490→0x480, Intensity 0x498→0x488, Tightness 0x49c→0x48c, bPreviewVectorField 0x4a0→0x490 | - | - |
| `/Script/Engine.VfxEffectSettings` | - | m_CreateStaticMeshLOD_NumVertices | - |
| `/Script/Engine.VolumetricCloudComponent` | LayerBottomAltitude 0x1f8→0x1f0, LayerHeight 0x1fc→0x1f4, TracingStartMaxDistance 0x200→0x1f8, TracingMaxDistance 0x204→0x1fc … (14 moved) | - | - |
| `/Script/Engine.VolumetricFogProbeComponent` | Visibility 0x1f8→0x1f0, Shape 0x1fc→0x1f4, Radius 0x200→0x1f8, HalfExtent 0x204→0x1fc … (17 moved) | - | - |
| `/Script/Engine.WindDirectionalSourceComponent` | Strength 0x1f8→0x1f0, Speed 0x1fc→0x1f4, MinGustAmount 0x200→0x1f8, MaxGustAmount 0x204→0x1fc … (6 moved) | - | - |
| `/Script/FacialAnimation.AudioCurveSourceComponent` | CurveSourceBindingName 0x870→0x868, CurveSyncOffset 0x878→0x870 | - | - |
| `/Script/Foliage.FoliageInstancedStaticMeshComponent` | OnInstanceTakePointDamage 0x720→0x710, OnInstanceTakeRadialDamage 0x730→0x720, GenerationGuid 0x740→0x730 | - | - |
| `/Script/InteractiveToolsFramework.GizmoArrowComponent` | Direction 0x4b0→0x4a0, Gap 0x4bc→0x4ac, Length 0x4c0→0x4b0, Thickness 0x4c4→0x4b4 | - | - |
| `/Script/InteractiveToolsFramework.GizmoBaseComponent` | Color 0x490→0x480, HoverSizeMultiplier 0x4a0→0x490, PixelHitDistanceThreshold 0x4a4→0x494 | - | - |
| `/Script/InteractiveToolsFramework.GizmoBoxComponent` | Origin 0x4b0→0x4a0, Rotation 0x4c0→0x4b0, Dimensions 0x4d0→0x4c0, LineThickness 0x4dc→0x4cc … (6 moved) | - | - |
| `/Script/InteractiveToolsFramework.GizmoCircleComponent` | Normal 0x4b0→0x4a0, Radius 0x4bc→0x4ac, Thickness 0x4c0→0x4b0, NumSides 0x4c4→0x4b4 … (6 moved) | - | - |
| `/Script/InteractiveToolsFramework.GizmoLineHandleComponent` | Normal 0x4b0→0x4a0, HandleSize 0x4bc→0x4ac, Thickness 0x4c0→0x4b0, Direction 0x4c4→0x4b4 … (6 moved) | - | - |
| `/Script/InteractiveToolsFramework.GizmoRectangleComponent` | DirectionX 0x4b0→0x4a0, DirectionY 0x4bc→0x4ac, OffsetX 0x4c8→0x4b8, OffsetY 0x4cc→0x4bc … (8 moved) | - | - |
| `/Script/KBDRt.SQEX_AnimNotifyState_Bonamik_ResetAndPreRoll` | - | m_ForceOverride | - |
| `/Script/KBDRt.SQEX_AnimNotify_Bonamik_ResetAndPreRoll` | - | m_ForceOverride | - |
| `/Script/KBDRt.SQEX_KBD_Component` | CharacterMovementComponent 0x440→0x430 | - | - |
| `/Script/KineDriverRt.SQEX_KineDriver_Component` | KineDriverIndex 0x1f8→0x1f0, KineDriverData 0x200→0x1f8, EnableScaleOpChildSSC 0x210→0x208, TargetNodeDisableScaleOpChildSSC 0x218→0x210 … (12 moved) | - | - |
| `/Script/Landscape.ControlPointMeshComponent` | VirtualTextureMainPassMaxDrawDistance 0x588→0x578 | - | - |
| `/Script/Landscape.LandscapeComponent` | SectionBaseX 0x490→0x480, SectionBaseY 0x494→0x484, ComponentSizeQuads 0x498→0x488, SubsectionSizeQuads 0x49c→0x48c … (38 moved) | - | - |
| `/Script/Landscape.LandscapeHeightfieldCollisionComponent` | ComponentLayerInfos 0x490→0x480, SectionBaseX 0x4a0→0x490, SectionBaseY 0x4a4→0x494, CollisionSizeQuads 0x4a8→0x498 … (11 moved) | - | - |
| `/Script/Landscape.LandscapeMeshCollisionComponent` | MeshGuid 0x570→0x560 | - | - |
| `/Script/Landscape.LandscapeMeshProxyComponent` | LandscapeGuid 0x588→0x578, ProxyComponentBases 0x598→0x588, ProxyLOD 0x5a8→0x598 | - | - |
| `/Script/Landscape.LandscapeSplinesComponent` | ControlPoints 0x490→0x480, Segments 0x4a0→0x490, CookedForeignMeshComponents 0x4b0→0x4a0 | - | - |
| `/Script/MediaAssets.MediaSoundComponent` | Channels 0x6d0→0x6c0, DynamicRateAdjustment 0x6d4→0x6c4, RateAdjustmentFactor 0x6d8→0x6c8, RateAdjustmentRange 0x6dc→0x6cc … (5 moved) | - | - |
| `/Script/ModelingComponents.LineSetComponent` | LineMaterial 0x510→0x500, bounds 0x518→0x508, bBoundsDirty 0x534→0x524 | - | - |
| `/Script/ModelingComponents.OctreeDynamicMeshComponent` | bExplicitShowWireframe 0x570→0x560 | - | - |
| `/Script/ModelingComponents.PointSetComponent` | PointMaterial 0x510→0x500, bounds 0x518→0x508, bBoundsDirty 0x534→0x524 | - | - |
| `/Script/ModelingComponents.SimpleDynamicMeshComponent` | TangentsType 0x558→0x548, bInvalidateProxyOnChange 0x559→0x549, bExplicitShowWireframe 0x590→0x580, bDrawOnTop 0x5e0→0x5d0 | - | - |
| `/Script/ModelingComponents.TriangleSetComponent` | bounds 0x510→0x500, bBoundsDirty 0x52c→0x51c | - | - |
| `/Script/MovieScene.MovieScene` | EvaluationType 0x118→0x119, ClockSource 0x119→0x11a | - | - |
| `/Script/NavigationSystem.NavLinkComponent` | Links 0x498→0x488 | - | - |
| `/Script/NavigationSystem.NavigationGraphNodeComponent` | Node 0x1f8→0x1f0, NextNodeComponent 0x210→0x208, PrevNodeComponent 0x218→0x210 | - | - |
| `/Script/Niagara.NiagaraComponent` | Asset 0x638→0x628, TickBehavior 0x640→0x630, RandomSeedOffset 0x644→0x634, OverrideParameters 0x648→0x638 … (22 moved) | - | - |
| `/Script/Niagara.NiagaraCullProxyComponent` | Instances 0x8e0→0x8d0 | - | - |
| `/Script/Niagara.NiagaraEmitter` | bVersioningEnabled 0x40→0x50, VersionData 0x48→0x40, bLocalSpace 0x58→0x51, bDeterminism 0x59→0x52 … (45 moved) | m_VfxLowPerformance_SpawnCountScale, m_VfxDisableHandheld | m_VfxDisableIntelArc |
| `/Script/Niagara.NiagaraRendererProperties` | - | VfxDisableLowPerformance | - |
| `/Script/Niagara.NiagaraSystem` | - | m_bVfxIgnoreLowPerformance_ScalabilitySettings_MaxDistance | bAllowCullingForLocalPlayersOverride |
| `/Script/Paper2D.PaperFlipbookComponent` | SourceFlipbook 0x510→0x500, Material 0x518→0x508, PlayRate 0x520→0x510, bLooping 0x524→0x514 … (11 moved) | - | - |
| `/Script/Paper2D.PaperGroupedSpriteComponent` | InstanceMaterials 0x510→0x500, PerInstanceSpriteData 0x520→0x510 | - | - |
| `/Script/Paper2D.PaperSpriteComponent` | SourceSprite 0x510→0x500, MaterialOverride 0x518→0x508, SpriteColor 0x520→0x510 | - | - |
| `/Script/Paper2D.PaperTerrainComponent` | TerrainMaterial 0x490→0x480, bClosedSpline 0x498→0x488, bFilledSpline 0x499→0x489, AssociatedSpline 0x4a0→0x490 … (11 moved) | - | - |
| `/Script/Paper2D.PaperTileMapComponent` | MapWidth 0x510→0x500, MapHeight 0x514→0x504, TileWidth 0x518→0x508, TileHeight 0x51c→0x50c … (11 moved) | - | - |
| `/Script/ProceduralMeshComponent.ProceduralMeshComponent` | bUseComplexAsSimpleCollision 0x518→0x508, bUseAsyncCooking 0x519→0x509, ProcMeshBodySetup 0x520→0x510, ProcMeshSections 0x528→0x518 … (7 moved) | - | - |
| `/Script/SQEXSEAD.SQEXSEADLayoutComponent` | Sound 0x490→0x480, SoundName 0x498→0x488, LayoutInfo 0x4a0→0x490, bIgnoreObstruction 0x500→0x4f0 … (8 moved) | - | - |
| `/Script/SQEXSEAD.SQEXSEADSound` | EndCustomType 0x41a→0x41b | bStreamingAsset_PlatformSW2 | - |
| `/Script/SQEXSEAD.SQEXSEADWhizSeComponent` | WhizSoundAsset 0x1f8→0x1f0, WhizSoundName 0x200→0x1f8, ApproachJudgmentDistance 0x208→0x200, ApproachJudgmentVelocity 0x20c→0x204 … (10 moved) | - | - |
| `/Script/UMG.WidgetComponent` | Space 0x510→0x500, TimingPolicy 0x511→0x501, WidgetClass 0x518→0x508, DrawSize 0x520→0x510 … (38 moved) | - | - |
| `/Script/UMG.WidgetInteractionComponent` | OnHoveredWidgetChanged 0x1f8→0x1f0, VirtualUserIndex 0x218→0x210, PointerIndex 0x21c→0x214, TraceChannel 0x220→0x218 … (19 moved) | - | - |

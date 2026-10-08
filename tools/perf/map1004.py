"""Map the 2026-10-03/04 perf doc's 1.004 function addresses to 1.005 with tools/version_match (Matcher.match_code)."""
import sys
sys.path.insert(0, r'C:\_Projects\FinalFantasy7Modding\Rebirth\LocalSplitScreen\tools\version_match')
from pe_image import Image
from match import Matcher
OLD = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.004\ff7rebirth_.exe"
NEW = r"C:\_Projects\FinalFantasy7Modding\Rebirth\builds\1.005\ff7rebirth_.exe"
NAMES = {
 0x140E1623C:'GameThreadWaitForTask',0x140E15F68:'FRenderCommandFence_Wait_cand',0x14109EA24:'frame tick (GT RT-fence site)',0x1410A1600:'frame pacing (SleepEx)',
 0x141658D58:'FRHICommandListBase_WaitForDispatch',0x1414FBBA8:'FD3D12Fence_WaitForFence',0x141732094:'D3D12_SyncPointQueue_WaitHoldingLock_cand',
 0x14083FF60:'RHIGetRenderQueryResult',0x140FEB7A4:'FetchVisibilityForPrimitives',0x140FDCE70:'OcclusionCull',0x1409455A0:'FSceneRenderer__ComputeViewVisibility_cand',
 0x140FEB75C:'FSceneRenderer_WaitOcclusionTests_cand',0x140FEC44C:'WaitOnRHIThreadFence',0x14496AEE4:'FRHIThread__Run',0x140D035E8:'D3D12_SyncPointQueue_PollCompleted_cand',
 0x140C9DEE8:'FD3D12CmdContext_RHIDispatchComputeShader',0x14081AB64:'D3D12ResidencyManager_ExecuteCommandLists',0x140DF6868:'FD3D12Viewport_SwapChainPresent1_cand',
 0x140814D20:'ME_RenderPipeline_BindSRVsAndPrepare',0x140D034D4:'D3D12_FlushPendingBarriers',0x14090DDA0:'parallel RHI translate (sub_14090DDA0)',0x141911DD4:'game tick tasks (sub_141911DD4)',
 0x140843F10:'MasterIconPaint_NaviMapHolder_FrameUpdate',0x140FD666C:'Array28_WithBitArray_CopyAssign_cand',0x1414B5414:'LargeBlock_VirtualFree',0x140FDD1F0:'SceneRenderer_Render_Main',
 0x14082E608:'SceneRenderer_MasterRenderStaticMeshes',0x1408F7AB8:'FRDGBuilder_Execute_DrainAllPasses',0x140B4AC20:'SceneRenderer_RenderBasePass_SerialViewLoop',
 0x140F59C50:'UUserWidget__SetPlayerContext',0x140C95CC0:'SEndRootOverlay__RenderToCompositeLayer',0x141627E98:'FSlateApplication__ExecuteDelegate1000_RenderRootOverlay',
 0x140FE9E60:'FSlateRHIRenderer_DrawWindow_RenderThread',0x14190E8C4:'GT world tick (sub_14190E8C4)',0x14190E35C:'tick-group run (0x14190E35C)',0x141913158:'EndGame__InvokeTickPhaseCallback',
 0x140994B38:'component tick dispatch (0x140994B38)',0x1425BED30:'FActorComponentTickFunction_ExecuteTick shape (0x1425BED30)',0x1409ED68C:'TickComponent 0x1409ED68C',
 0x1408867C0:'skinned-mesh TickComponent (0x1408867C0)',0x140A21174:'UEndCharacterMovementComponent_TickComponent',0x14106DA88:'text relayout (0x14106DA88)',
 0x140E71030:'MinimapIcon_RenderLoop',0x140E55E40:'P2 camera fn (0x140E55E40)',0x140970114:'UWidgetTree__ForWidgetAndChildren',0x1409D5F28:'RT fn 0x1409D5F28',
 0x14090BF18:'(renamed 2026-10-03) 0x14090BF18',0x140812430:'RHI direct chain 0x140812430',0x1408969D0:'0x1408969D0',0x140F0B27C:'ME LOD rule',0x143D934E8:'Settings_ApplyResolutionDrivenCVars',
 0x140EB5164:'FD3D12TextureAllocator_AllocateCommittedTexture_cand',0x140D009A8:'D3D12_CmdCtx_CloseSubmitReset',0x14098EC24:'SEndTextBlock_SetText_RelayoutIfChanged_cand',
 0x140CD04FC:'SEndNaviMap paint',0x1408C1614:'SWidget__Paint',0x141081AF0:'relayout path 0x141081AF0',
}
old, new = Image(OLD), Image(NEW)
m = Matcher(old, new)
for a, n in NAMES.items():
    try:
        r = m.match_code(a)
        na = (a - r.old + r.new) if r else None
        nf = new.function_at(na) if na else None
        print(f'{a:#x}\t{na:#x}\t{nf[0]:#x}\t{n}\t{r.how}' if na else f'{a:#x}\t-\t-\t{n}\tunmatched')
    except Exception as e:
        print(f'{a:#x}\t-\t-\t{n}\terror {e}')

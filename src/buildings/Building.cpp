#include "common.h"

#include "Building.h"
#include "Streaming.h"
#include "Pools.h"
#include "ModelInfo.h"
#include "SimpleModelInfo.h"

void *CBuilding::operator new(size_t sz) throw() { return CPools::GetBuildingPool()->New();  }
void CBuilding::operator delete(void *p, size_t sz) throw() { CPools::GetBuildingPool()->Delete((CBuilding*)p); }

#ifdef FIX_BUGS
// ============= SilentPatch: Construction site LOD fix =============
// After the "Demolition Man" mission, the construction site building
// model is swapped from `bldngst2mesh` to `bldngst2meshdam` via
// CBuilding::ReplaceWithNewModel. The LOD model `LODngst2mesh` keeps a
// pointer to its non-LOD model in `m_atomics[2]` (see
// CSimpleModelInfo::SetRelatedModel). The original game never updates
// this pointer when the building is swapped, so the LOD ends up
// pointing at the wrong (no longer used) HQ model, and on top of that
// the LOD keeps showing at all distances because the LOD's LOD distance
// table is no longer consistent with the swapped HQ model.
//
// This fix mirrors the construction-site swap on the LOD side:
// whenever ReplaceWithNewModel swaps between bldngst2mesh and
// bldngst2meshdam, we also repoint the related-model slot of
// LODngst2mesh to the new HQ model.
//
// The fix is a no-op if `LODngst2meshdam` exists in the loaded data
// set - in that case a mod (or a future reVC build) has already
// resolved the issue via map edits, and we should not fight it.
namespace ConstructionSiteLODFix
{
        static bool bInitialized = false;
        static bool bActivateFix = false;
        static int32 MI_BLDNGST2MESH = -1;
        static int32 MI_BLDNGST2MESHDAM = -1;
        static CSimpleModelInfo *LODngst2mesh_ModelInfo = nil;

        static void MatchModelIndices()
        {
                bInitialized = true;
                bActivateFix = false;
                MI_BLDNGST2MESH = -1;
                MI_BLDNGST2MESHDAM = -1;
                LODngst2mesh_ModelInfo = nil;

                CSimpleModelInfo *bldngst2mesh     = (CSimpleModelInfo*)CModelInfo::GetModelInfo("bldngst2mesh",     &MI_BLDNGST2MESH);
                CSimpleModelInfo *bldngst2meshdam  = (CSimpleModelInfo*)CModelInfo::GetModelInfo("bldngst2meshdam",  &MI_BLDNGST2MESHDAM);
                CSimpleModelInfo *lodngst2mesh     = (CSimpleModelInfo*)CModelInfo::GetModelInfo("LODngst2mesh",     nil);
                CSimpleModelInfo *lodngst2meshdam = (CSimpleModelInfo*)CModelInfo::GetModelInfo("LODngst2meshdam",   nil);

                // If a mod author has already added `LODngst2meshdam` to fix this
                // via map edits, our swap would just fight with theirs - bail out.
                if (lodngst2meshdam != nil)
                        return;

                if (bldngst2mesh == nil || bldngst2meshdam == nil || lodngst2mesh == nil)
                        return;

                bActivateFix = true;
                LODngst2mesh_ModelInfo = lodngst2mesh;
        }

        // Called from CBuilding::ReplaceWithNewModel after the model swap.
        static void FixConstructionSiteLOD(int32 oldModelID, int32 newModelID)
        {
                if (!bActivateFix)
                        return;

                if (oldModelID == MI_BLDNGST2MESH && newModelID == MI_BLDNGST2MESHDAM) {
                        // HQ was destroyed -> point LOD at the damaged HQ model
                        CSimpleModelInfo *dam = (CSimpleModelInfo*)CModelInfo::GetModelInfo(MI_BLDNGST2MESHDAM);
                        if (dam != nil)
                                LODngst2mesh_ModelInfo->SetRelatedModel(dam);
                } else if (oldModelID == MI_BLDNGST2MESHDAM && newModelID == MI_BLDNGST2MESH) {
                        // HQ was restored -> point LOD at the intact HQ model
                        CSimpleModelInfo *intact = (CSimpleModelInfo*)CModelInfo::GetModelInfo(MI_BLDNGST2MESH);
                        if (intact != nil)
                                LODngst2mesh_ModelInfo->SetRelatedModel(intact);
                }
        }
};
#endif // FIX_BUGS

void
CBuilding::ReplaceWithNewModel(int32 id)
{
#ifdef FIX_BUGS
        // SilentPatch: lazily resolve the construction-site model indices the
        // first time ReplaceWithNewModel is called. By then CModelInfo has
        // already loaded all IDE entries, so GetModelInfo(name, &id) works.
        if (!ConstructionSiteLODFix::bInitialized)
                ConstructionSiteLODFix::MatchModelIndices();
        const int32 oldModelID = m_modelIndex;
#else
        (void)0;
#endif

        DeleteRwObject();

        if (CModelInfo::GetModelInfo(m_modelIndex)->GetNumRefs() == 0)
                CStreaming::RemoveModel(m_modelIndex);
        m_modelIndex = id;

        if(bIsBIGBuilding)
                if(m_level == LEVEL_GENERIC || m_level == CGame::currLevel)
                        CStreaming::RequestModel(id, STREAMFLAGS_DONT_REMOVE);

#ifdef FIX_BUGS
        ConstructionSiteLODFix::FixConstructionSiteLOD(oldModelID, id);
#endif
}

bool
IsBuildingPointerValid(CBuilding* pBuilding)
{
        if (!pBuilding)
                return false;
        if (pBuilding->GetIsATreadable()) {
                int index = CPools::GetTreadablePool()->GetJustIndex_NoFreeAssert((CTreadable*)pBuilding);
#ifdef FIX_BUGS
                return index >= 0 && index < CPools::GetTreadablePool()->GetSize();
#else
                return index >= 0 && index <= CPools::GetTreadablePool()->GetSize();
#endif
        } else {
                int index = CPools::GetBuildingPool()->GetJustIndex_NoFreeAssert(pBuilding);
#ifdef FIX_BUGS
                return index >= 0 && index < CPools::GetBuildingPool()->GetSize();
#else
                return index >= 0 && index <= CPools::GetBuildingPool()->GetSize();
#endif
        }
}

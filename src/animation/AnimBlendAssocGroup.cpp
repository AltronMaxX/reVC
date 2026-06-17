#include "common.h"

#if defined _WIN32 && !defined __MINGW32__
#if defined __MWERKS__
#include <wctype.h>
#else
#include "ctype.h"
#endif
#else
#include <cwctype>
#endif

#include "General.h"
#include "RwHelper.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "AnimManager.h"
#include "RpAnimBlend.h"
#include "AnimBlendAssociation.h"
#include "AnimBlendAssocGroup.h"

CAnimBlendAssocGroup::CAnimBlendAssocGroup()
{
	animBlock = nullptr;
	assocList = nullptr;
	numAssociations = 0;
	firstAnimId = 0;
	groupId = -1;
}

CAnimBlendAssocGroup::~CAnimBlendAssocGroup()
{
	DestroyAssociations();
}

void
CAnimBlendAssocGroup::DestroyAssociations()
{
	if(assocList){
		delete[] assocList;
		assocList = nullptr;
		numAssociations = 0;
	}
}

CAnimBlendAssociation*
CAnimBlendAssocGroup::GetAnimation(const uint32 id) const
{
	return &assocList[id - firstAnimId];
}

CAnimBlendAssociation*
CAnimBlendAssocGroup::GetAnimation(const char *name) const
{
	for(int i = 0; i < numAssociations; i++)
		if(!CGeneral::faststricmp(assocList[i].hierarchy->name, name))
			return &assocList[i];
	debug("\n\nCan't find the fucking animation %s\n\n\n", name);
	return nullptr;
}


CAnimBlendAssociation*
CAnimBlendAssocGroup::CopyAnimation(const uint32 id) const
{
	const CAnimBlendAssociation *anim = GetAnimation(id);
	if(anim == nullptr)
		return nullptr;
	CAnimManager::UncompressAnimation(anim->hierarchy);
	return new CAnimBlendAssociation(*anim);
}

CAnimBlendAssociation*
CAnimBlendAssocGroup::CopyAnimation(const char *name) const
{
	const CAnimBlendAssociation *anim = GetAnimation(name);
	if(anim == nullptr)
		return nullptr;
	CAnimManager::UncompressAnimation(anim->hierarchy);
	return new CAnimBlendAssociation(*anim);
}

bool
strcmpIgnoringDigits(const char *s1, const char *s2)
{
	for(;;){
		char c1 = *s1;
		char c2 = *s2;
		if(c1) s1++;
		if(c2) s2++;
		if(c1 == '\0' && c2 == '\0') return true;
#ifndef ASCII_STRCMP
		if(iswdigit(c1) && iswdigit(c2))
#else
		if(__ascii_iswdigit(c1) && __ascii_iswdigit(c2))
#endif
			continue;
#ifndef ASCII_STRCMP
		c1 = toupper(c1);
		c2 = toupper(c2);
#else
		c1 = __ascii_toupper(c1);
		c2 = __ascii_toupper(c2);
#endif

		if(c1 && c2 && c1 != c2)
			return false;
	}
}

CBaseModelInfo*
GetModelFromName(const char *name)
{
	char playername[32];

	if(strncasecmp(name, "CSplay", 6) == 0 &&
	   strncasecmp(CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName(), "ig", 2) == 0){
		strcpy(playername, CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName());
		playername[0] = 'C';
		playername[1] = 'S';
		name = playername;
	}

	for(int i = 0; i < MODELINFOSIZE; i++){
		if(CBaseModelInfo *mi = CModelInfo::GetModelInfo(i); mi && mi->GetRwObject() && RwObjectGetType(mi->GetRwObject()) == rpCLUMP &&
		                                                     strcmpIgnoringDigits(mi->GetModelName(), name))
			return mi;
	}
	return nullptr;
}

void
CAnimBlendAssocGroup::CreateAssociations(const char *name)
{
	DestroyAssociations();

	CAnimBlock *animBlock = CAnimManager::GetAnimationBlock(name);
	assocList = new CAnimBlendAssociation[animBlock->numAnims];
	numAssociations = 0;

	for(int i = 0; i < animBlock->numAnims; i++){
		CAnimBlendHierarchy *anim = CAnimManager::GetAnimation(animBlock->firstIndex + i);
		if(CBaseModelInfo *model = GetModelFromName(anim->name)){
			debug("Associated anim %s with model %s\n", anim->name, model->GetModelName());
			auto *clump = reinterpret_cast<RpClump *>(model->CreateInstance());
			RpAnimBlendClumpInit(clump);
			assocList[i].Init(clump, anim);
			if(IsClumpSkinned(clump))
				RpClumpForAllAtomics(clump, AtomicRemoveAnimFromSkinCB, nullptr);
			RpClumpDestroy(clump);
			assocList[i].animId = firstAnimId + i;
			assocList[i].groupId = groupId;
		}else
			debug("\n\nCANNOT FIND MODELINFO WITH NAME %s\n\n\n", anim->name);
	}
	numAssociations = animBlock->numAnims;
}

// Create associations from hierarchies for a given clump
void
CAnimBlendAssocGroup::CreateAssociations(const char *blockName, RpClump *clump, const char **animNames, int numAssocs)
{
	DestroyAssociations();

	animBlock = CAnimManager::GetAnimationBlock(blockName);
	assocList = new CAnimBlendAssociation[numAssocs];

	numAssociations = 0;
	for(int i = 0; i < numAssocs; i++){
		assocList[i].Init(clump, CAnimManager::GetAnimation(animNames[i], animBlock));
		assocList[i].animId = firstAnimId + i;
		assocList[i].groupId = groupId;
	}
	numAssociations = numAssocs;
}

#pragma once

class CAnimBlendAssociation;
struct CAnimBlock;

class CAnimBlendAssocGroup
{
public:
	CAnimBlock *animBlock;
	CAnimBlendAssociation *assocList;
	int32 numAssociations;
	int32 firstAnimId;
	int32 groupId;	// id of self in ms_aAnimAssocGroups

	CAnimBlendAssocGroup();
	~CAnimBlendAssocGroup();
	void DestroyAssociations();

	[[nodiscard]] CAnimBlendAssociation *GetAnimation(uint32 id) const;

	CAnimBlendAssociation *GetAnimation(const char *name) const;
	[[nodiscard]] CAnimBlendAssociation *CopyAnimation(uint32 id) const;
	CAnimBlendAssociation *CopyAnimation(const char *name) const;
	void CreateAssociations(const char *name);
	void CreateAssociations(const char *blockName, RpClump *clump, const char **animNames, int numAssocs);
};

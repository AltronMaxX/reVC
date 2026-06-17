#include "common.h"
#include "ColModel.h"
#include "Collision.h"
#include "Game.h"
#include "MemoryHeap.h"
#include "Pools.h"

CColModel::CColModel()
{
	numSpheres = 0;
	spheres = nullptr;
	numLines = 0;
	lines = nullptr;
	numBoxes = 0;
	boxes = nullptr;
	numTriangles = 0;
	vertices = nullptr;
	triangles = nullptr;
	trianglePlanes = nullptr;
	level = LEVEL_GENERIC;	// generic col slot
	ownsCollisionVolumes = true;
}

CColModel::~CColModel()
{
	RemoveCollisionVolumes();
}

void*
CColModel::operator new(size_t)
{
	CColModel* node = CPools::GetColModelPool()->New();
	assert(node);
	return node;
}

void
CColModel::operator delete(void *p, size_t)
{
	CPools::GetColModelPool()->Delete(static_cast<CColModel *>(p));
}

void
CColModel::RemoveCollisionVolumes()
{
	if(ownsCollisionVolumes){
		RwFree(spheres);
		RwFree(lines);
		RwFree(boxes);
		RwFree(vertices);
		RwFree(triangles);
		CCollision::RemoveTrianglePlanes(this);
	}
	numSpheres = 0;
	numLines = 0;
	numBoxes = 0;
	numTriangles = 0;
	spheres = nullptr;
	lines = nullptr;
	boxes = nullptr;
	vertices = nullptr;
	triangles = nullptr;
}

void
CColModel::CalculateTrianglePlanes()
{
	PUSH_MEMID(MEMID_COLLISION);

	// HACK: allocate space for one more element to stuff the link pointer into
	trianglePlanes = static_cast<CColTrianglePlane *>(RwMalloc(sizeof(CColTrianglePlane) * (numTriangles + 1)));
	REGISTER_MEMPTR(&trianglePlanes);
	for(int i = 0; i < numTriangles; i++)
		trianglePlanes[i].Set(vertices, triangles[i]);

	POP_MEMID();
}

void
CColModel::RemoveTrianglePlanes()
{
	RwFree(trianglePlanes);
	trianglePlanes = nullptr;
}

void
CColModel::SetLinkPtr(CLink<CColModel*> *lptr) const {
	assert(trianglePlanes);
	*static_cast<CLink<CColModel *> **>(ALIGNPTR(&trianglePlanes[numTriangles])) = lptr;
}

CLink<CColModel*>*
CColModel::GetLinkPtr() const {
	assert(trianglePlanes);
	return *static_cast<CLink<CColModel *> **>(ALIGNPTR(&trianglePlanes[numTriangles]));
}

void
CColModel::GetTrianglePoint(CVector &v, const int i) const
{
	v = vertices[i].Get();
}

CColModel&
CColModel::operator=(const CColModel &other)
{
	int i;

	boundingSphere = other.boundingSphere;
	boundingBox = other.boundingBox;

	// copy spheres
	if(other.numSpheres){
		if(numSpheres != other.numSpheres){
			numSpheres = other.numSpheres;
			if(spheres)
				RwFree(spheres);
			spheres = static_cast<CColSphere *>(RwMalloc(numSpheres * sizeof(CColSphere)));
		}
		for(i = 0; i < numSpheres; i++)
			spheres[i] = other.spheres[i];
	}else{
		numSpheres = 0;
		if(spheres)
			RwFree(spheres);
		spheres = nullptr;
	}

	// copy lines
	if(other.numLines){
		if(numLines != other.numLines){
			numLines = other.numLines;
			if(lines)
				RwFree(lines);
			lines = static_cast<CColLine *>(RwMalloc(numLines * sizeof(CColLine)));
		}
		for(i = 0; i < numLines; i++)
			lines[i] = other.lines[i];
	}else{
		numLines = 0;
		if(lines)
			RwFree(lines);
		lines = nullptr;
	}

	// copy boxes
	if(other.numBoxes){
		if(numBoxes != other.numBoxes){
			numBoxes = other.numBoxes;
			if(boxes)
				RwFree(boxes);
			boxes = static_cast<CColBox *>(RwMalloc(numBoxes * sizeof(CColBox)));
		}
		for(i = 0; i < numBoxes; i++)
			boxes[i] = other.boxes[i];
	}else{
		numBoxes = 0;
		if(boxes)
			RwFree(boxes);
		boxes = nullptr;
	}

	// copy mesh
	if(other.numTriangles){
		// copy vertices
		int numVerts = 0;
		for(i = 0; i < other.numTriangles; i++){
			if(other.triangles[i].a > numVerts)
				numVerts = other.triangles[i].a;
			if(other.triangles[i].b > numVerts)
				numVerts = other.triangles[i].b;
			if(other.triangles[i].c > numVerts)
				numVerts = other.triangles[i].c;
		}
		numVerts++;
		if(vertices)
			RwFree(vertices);
		if(numVerts){
			vertices = static_cast<CompressedVector *>(RwMalloc(numVerts * sizeof(CompressedVector)));
			for(i = 0; i < numVerts; i++)
				vertices[i] = other.vertices[i];
		}

		// copy triangles
		if(numTriangles != other.numTriangles){
			numTriangles = other.numTriangles;
			if(triangles)
				RwFree(triangles);
			triangles = static_cast<CColTriangle *>(RwMalloc(numTriangles * sizeof(CColTriangle)));
		}
		for(i = 0; i < numTriangles; i++)
			triangles[i] = other.triangles[i];
	}else{
		numTriangles = 0;
		if(triangles)
			RwFree(triangles);
		triangles = nullptr;
		if(vertices)
			RwFree(vertices);
		vertices = nullptr;
	}
	return *this;
}

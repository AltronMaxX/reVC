#pragma once

struct CompressedVector
{
#ifdef COMPRESSED_COL_VECTORS
	int16 x, y, z;
	CVector Get(void) const { return CVector(x, y, z)/128.0f; };
	void Set(float x, float y, float z) { this->x = x*128.0f; this->y = y*128.0f; this->z = z*128.0f; };
	void Unpack(int32 *qword) const {
		qword[0] = x;
		qword[1] = y;
		qword[2] = z;
		qword[3] = 0;	// junk
	}
#else
	float x, y, z;
	CVector Get(void) const { return CVector(x, y, z); };
	void Set(float x, float y, float z) { this->x = x; this->y = y; this->z = z; };
#endif
};
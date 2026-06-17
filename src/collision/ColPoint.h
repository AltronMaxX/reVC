#pragma once

struct CColPoint
{
	CVector point;
	int pad1;
	// the surface normal on the surface of point
	CVector normal;
	int pad2;
	uint8 surfaceA;
	uint8 pieceA;
	uint8 surfaceB;
	uint8 pieceB;
	float depth;

	[[nodiscard]] const CVector &GetNormal() const { return normal; }
	[[nodiscard]] float GetDepth() const { return depth; }
	void Set(float depth, const uint8 surfA, uint8 pieceA, const uint8 surfB, const uint8 pieceB) {
		this->depth = depth;
		this->surfaceA = surfA;
		this->pieceA = pieceA;
		this->surfaceB = surfB;
		this->pieceB = pieceB;
	}
	void Set(const uint8 surfA, const uint8 pieceA, const uint8 surfB, const uint8 pieceB) {
		this->surfaceA = surfA;
		this->pieceA = pieceA;
		this->surfaceB = surfB;
		this->pieceB = pieceB;
	}

	CColPoint &operator=(const CColPoint &other);
};


#ifndef ALDEVICELIST_H
#define ALDEVICELIST_H

#ifdef AUDIO_OAL
#pragma warning(disable: 4786)  //disable warning "identifier was truncated to '255' characters in the browser information"

enum
{
	ADEXT_EXT_CAPTURE = (1 << 0),
	ADEXT_EXT_EFX = (1 << 1),
	ADEXT_EXT_OFFSET = (1 << 2),
	ADEXT_EXT_LINEAR_DISTANCE = (1 << 3),
	ADEXT_EXT_EXPONENT_DISTANCE = (1 << 4),
	ADEXT_EAX2 = (1 << 5),
	ADEXT_EAX3 = (1 << 6),
	ADEXT_EAX4 = (1 << 7),
	ADEXT_EAX5 = (1 << 8),
	ADEXT_EAX_RAM = (1 << 9),
};

struct ALDEVICEINFO {
	const char		*strDeviceName;
	int				iMajorVersion;
	int				iMinorVersion;
	unsigned int	uiSourceCount;
	unsigned short  Extensions;
	bool			bSelected;

	ALDEVICEINFO() : iMajorVersion(0), iMinorVersion(0), uiSourceCount(0), bSelected(false)
	{
		strDeviceName = nullptr;
		Extensions = 0;
	}
};

typedef ALDEVICEINFO *LPALDEVICEINFO;

class ALDeviceList
{
private:
	ALDEVICEINFO aDeviceInfo[64];
	unsigned int nNumOfDevices;
	int defaultDeviceIndex;
	int filterIndex{};

public:
	ALDeviceList ();
	~ALDeviceList ();

	[[nodiscard]] unsigned int GetNumDevices() const;
	[[nodiscard]] const char *GetDeviceName(unsigned int index) const;
	void GetDeviceVersion(unsigned int index, int *major, int *minor) const;
	[[nodiscard]] unsigned int GetMaxNumSources(unsigned int index) const;
	[[nodiscard]] bool IsExtensionSupported(int index, unsigned short ext) const;

	[[nodiscard]] int GetDefaultDevice() const;
	void FilterDevicesMinVer(int major, int minor);
	void FilterDevicesMaxVer(int major, int minor);
	void FilterDevicesExtension(unsigned short ext);
	void ResetFilters();
	int GetFirstFilteredDevice();
	int GetNextFilteredDevice();

private:
	static unsigned int GetMaxNumSources();
};
#endif

#endif // ALDEVICELIST_H

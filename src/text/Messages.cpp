#include "common.h"

#include "Messages.h"
#include "RwHelper.h"
#include "Hud.h"
#include "User.h"
#include "Timer.h"

#include "ControllerConfig.h"

#include "Font.h"

tMessage CMessages::BriefMessages[NUMBRIEFMESSAGES];
tPreviousBrief CMessages::PreviousBriefs[NUMPREVIOUSBRIEFS];
tBigMessage CMessages::BIGMessages[NUMBIGMESSAGES];
char CMessages::PreviousMissionTitle[16]; // unused

void
CMessages::Init()
{
	ClearMessages();

	for (auto & PreviousBrief : PreviousBriefs) {
		PreviousBrief.m_pText = nullptr;
		PreviousBrief.m_pString = nullptr;
	}
}

uint16
CMessages::GetWideStringLength(const wchar *src)
{
	uint16 length = 0;
	while (*(src++)) length++;
	return length;
}

void
CMessages::WideStringCopy(wchar *dst, const wchar *src, const uint16 size)
{
	int32 i = 0;
	if (src) {
		while (i < size - 1) {
			if (!src[i]) break;
			dst[i] = src[i];
			i++;
		}
	} else {
		while (i < size - 1)
			dst[i++] = '\0';
	}
	dst[i] = '\0';
}

wchar FixupChar(const wchar c)
{
#ifdef MORE_LANGUAGES
	if (CFont::IsJapanese())
		return c & 0x7fff;
#endif
	return c;
}

bool
CMessages::WideStringCompare(const wchar *str1, const wchar *str2, const uint16 size)
{
	const uint16 len1 = GetWideStringLength(str1);
	if (const uint16 len2 = GetWideStringLength(str2); len1 != len2 && (len1 < size || len2 < size))
		return false;

	for (int32 i = 0; i < size && FixupChar(str1[i]) != '\0'; i++) {
		if (FixupChar(str1[i]) != FixupChar(str2[i]))
			return false;
	}
	return true;
}

void
CMessages::Process()
{
	for (auto &[m_Stack] : BIGMessages) {
		if (m_Stack[0].m_pText != nullptr && CTimer::GetTimeInMilliseconds() > m_Stack[0].m_nTime + m_Stack[0].m_nStartTime) {
			m_Stack[0].m_pText = nullptr;

			int32 i = 0;
			while (i < 3) {
				if (m_Stack[i + 1].m_pText == nullptr) break;
				m_Stack[i] = m_Stack[i + 1];
				i++;
			}

			m_Stack[i].m_pText = nullptr;
			m_Stack[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
		}
	}

	if (BriefMessages[0].m_pText != nullptr && CTimer::GetTimeInMilliseconds() > BriefMessages[0].m_nTime + BriefMessages[0].m_nStartTime) {
		BriefMessages[0].m_pText = nullptr;
		int32 i;
		for (i = 0; i < NUMBRIEFMESSAGES-1 && BriefMessages[i + 1].m_pText != nullptr; i++) {
			BriefMessages[i] = BriefMessages[i + 1];
		}
		BriefMessages[i].m_pText = nullptr;
		BriefMessages[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
		if (BriefMessages[0].m_pText != nullptr)
			AddToPreviousBriefArray(
				BriefMessages[0].m_pText,
				BriefMessages[0].m_nNumber[0],
				BriefMessages[0].m_nNumber[1],
				BriefMessages[0].m_nNumber[2],
				BriefMessages[0].m_nNumber[3],
				BriefMessages[0].m_nNumber[4],
				BriefMessages[0].m_nNumber[5],
				BriefMessages[0].m_pString);
	}
}

void
CMessages::Display()
{
	wchar outstr[256];

	DefinedState();

	for (int32 i = 0; i < NUMBIGMESSAGES; i++) {
		InsertNumberInString(
			BIGMessages[i].m_Stack[0].m_pText,
			BIGMessages[i].m_Stack[0].m_nNumber[0],
			BIGMessages[i].m_Stack[0].m_nNumber[1],
			BIGMessages[i].m_Stack[0].m_nNumber[2],
			BIGMessages[i].m_Stack[0].m_nNumber[3],
			BIGMessages[i].m_Stack[0].m_nNumber[4],
			BIGMessages[i].m_Stack[0].m_nNumber[5],
			outstr);
		InsertStringInString(outstr, BIGMessages[i].m_Stack[0].m_pString);
		InsertPlayerControlKeysInString(outstr);
		CHud::SetBigMessage(outstr, i);
	}

	InsertNumberInString(
		BriefMessages[0].m_pText,
		BriefMessages[0].m_nNumber[0],
		BriefMessages[0].m_nNumber[1],
		BriefMessages[0].m_nNumber[2],
		BriefMessages[0].m_nNumber[3],
		BriefMessages[0].m_nNumber[4],
		BriefMessages[0].m_nNumber[5],
		outstr);
	InsertStringInString(outstr, BriefMessages[0].m_pString);
	InsertPlayerControlKeysInString(outstr);
	CHud::SetMessage(outstr);
}

void
CMessages::AddMessage(wchar *msg, const uint32 time, const uint16 flag)
{
	wchar outstr[512]; // unused
	WideStringCopy(outstr, msg, 256);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	int32 i = 0;
	while (i < NUMBRIEFMESSAGES && BriefMessages[i].m_pText != nullptr)
		i++;
	if (i >= NUMBRIEFMESSAGES) return;

	BriefMessages[i].m_pText = msg;
	BriefMessages[i].m_nFlag = flag;
	BriefMessages[i].m_nTime = time;
	BriefMessages[i].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BriefMessages[i].m_nNumber[0] = -1;
	BriefMessages[i].m_nNumber[1] = -1;
	BriefMessages[i].m_nNumber[2] = -1;
	BriefMessages[i].m_nNumber[3] = -1;
	BriefMessages[i].m_nNumber[4] = -1;
	BriefMessages[i].m_nNumber[5] = -1;
	BriefMessages[i].m_pString = nullptr;
	if (i == 0)
		AddToPreviousBriefArray(
			BriefMessages[0].m_pText,
			BriefMessages[0].m_nNumber[0],
			BriefMessages[0].m_nNumber[1],
			BriefMessages[0].m_nNumber[2],
			BriefMessages[0].m_nNumber[3],
			BriefMessages[0].m_nNumber[4],
			BriefMessages[0].m_nNumber[5],
			BriefMessages[0].m_pString);
}

void
CMessages::AddMessageJumpQ(wchar *msg, const uint32 time, const uint16 flag)
{
	wchar outstr[512]; // unused
	WideStringCopy(outstr, msg, 256);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	BriefMessages[0].m_pText = msg;
	BriefMessages[0].m_nFlag = flag;
	BriefMessages[0].m_nTime = time;
	BriefMessages[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BriefMessages[0].m_nNumber[0] = -1;
	BriefMessages[0].m_nNumber[1] = -1;
	BriefMessages[0].m_nNumber[2] = -1;
	BriefMessages[0].m_nNumber[3] = -1;
	BriefMessages[0].m_nNumber[4] = -1;
	BriefMessages[0].m_nNumber[5] = -1;
	BriefMessages[0].m_pString = nullptr;
	AddToPreviousBriefArray(msg, -1, -1, -1, -1, -1, -1, nullptr);
}

void
CMessages::AddMessageSoon(wchar *msg, const uint32 time, const uint16 flag)
{
	wchar outstr[512]; // unused
	WideStringCopy(outstr, msg, 256);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	if (BriefMessages[0].m_pText != nullptr) {
		for (int i = NUMBRIEFMESSAGES-1; i > 1; i--)
			BriefMessages[i] = BriefMessages[i-1];

		BriefMessages[1].m_pText = msg;
		BriefMessages[1].m_nFlag = flag;
		BriefMessages[1].m_nTime = time;
		BriefMessages[1].m_nStartTime = CTimer::GetTimeInMilliseconds();
		BriefMessages[1].m_nNumber[0] = -1;
		BriefMessages[1].m_nNumber[1] = -1;
		BriefMessages[1].m_nNumber[2] = -1;
		BriefMessages[1].m_nNumber[3] = -1;
		BriefMessages[1].m_nNumber[4] = -1;
		BriefMessages[1].m_nNumber[5] = -1;
		BriefMessages[1].m_pString = nullptr;
	}else{
		BriefMessages[0].m_pText = msg;
		BriefMessages[0].m_nFlag = flag;
		BriefMessages[0].m_nTime = time;
		BriefMessages[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
		BriefMessages[0].m_nNumber[0] = -1;
		BriefMessages[0].m_nNumber[1] = -1;
		BriefMessages[0].m_nNumber[2] = -1;
		BriefMessages[0].m_nNumber[3] = -1;
		BriefMessages[0].m_nNumber[4] = -1;
		BriefMessages[0].m_nNumber[5] = -1;
		BriefMessages[0].m_pString = nullptr;
		AddToPreviousBriefArray(msg, -1, -1, -1, -1, -1, -1, nullptr);
	}
}

void
CMessages::ClearMessages()
{
	for (auto &[m_Stack] : BIGMessages) {
		for (int32 j = 0; j < 4; j++) {
			m_Stack[j].m_pText = nullptr;
			m_Stack[j].m_pString = nullptr;
		}
	}
	ClearSmallMessagesOnly();
}

void
CMessages::ClearSmallMessagesOnly()
{
	for (auto & BriefMessage : BriefMessages) {
		BriefMessage.m_pText = nullptr;
		BriefMessage.m_pString = nullptr;
	}
}

void
CMessages::AddBigMessage(wchar *msg, const uint32 time, const uint16 style)
{
	wchar outstr[512]; // unused
	WideStringCopy(outstr, msg, 256);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	BIGMessages[style].m_Stack[0].m_pText = msg;
	BIGMessages[style].m_Stack[0].m_nFlag = 0;
	BIGMessages[style].m_Stack[0].m_nTime = time;
	BIGMessages[style].m_Stack[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BIGMessages[style].m_Stack[0].m_nNumber[0] = -1;
	BIGMessages[style].m_Stack[0].m_nNumber[1] = -1;
	BIGMessages[style].m_Stack[0].m_nNumber[2] = -1;
	BIGMessages[style].m_Stack[0].m_nNumber[3] = -1;
	BIGMessages[style].m_Stack[0].m_nNumber[4] = -1;
	BIGMessages[style].m_Stack[0].m_nNumber[5] = -1;
	BIGMessages[style].m_Stack[0].m_pString = nullptr;
}

void
CMessages::AddBigMessageQ(wchar *msg, const uint32 time, const uint16 style)
{
	wchar outstr[512]; // unused
	WideStringCopy(outstr, msg, 256);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	int32 i = 0;
	while (i < 4 && BIGMessages[style].m_Stack[i].m_pText != nullptr)
		i++;

	if (i >= 4) return;

	BIGMessages[style].m_Stack[i].m_pText = msg;
	BIGMessages[style].m_Stack[i].m_nFlag = 0;
	BIGMessages[style].m_Stack[i].m_nTime = time;
	BIGMessages[style].m_Stack[i].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BIGMessages[style].m_Stack[i].m_nNumber[0] = -1;
	BIGMessages[style].m_Stack[i].m_nNumber[1] = -1;
	BIGMessages[style].m_Stack[i].m_nNumber[2] = -1;
	BIGMessages[style].m_Stack[i].m_nNumber[3] = -1;
	BIGMessages[style].m_Stack[i].m_nNumber[4] = -1;
	BIGMessages[style].m_Stack[i].m_nNumber[5] = -1;
	BIGMessages[style].m_Stack[i].m_pString = nullptr;
}

void
CMessages::AddToPreviousBriefArray(wchar *text, const int32 n1, const int32 n2, const int32 n3, const int32 n4, const int32 n5, const int32 n6, wchar *string)
{
	int32 i;
	for (i = 0; i < NUMPREVIOUSBRIEFS && PreviousBriefs[i].m_pText != nullptr; i++) {
		if (PreviousBriefs[i].m_nNumber[0] == n1
			&& PreviousBriefs[i].m_nNumber[1] == n2
			&& PreviousBriefs[i].m_nNumber[2] == n3
			&& PreviousBriefs[i].m_nNumber[3] == n4
			&& PreviousBriefs[i].m_nNumber[4] == n5
			&& PreviousBriefs[i].m_nNumber[5] == n6
			&& PreviousBriefs[i].m_pText == text
			&& PreviousBriefs[i].m_pString == string)
			return;
	}

	if (i != 0) {
		if (i == NUMPREVIOUSBRIEFS) i -= 2;
		else i--;

		while (i >= 0) {
			PreviousBriefs[i + 1] = PreviousBriefs[i];
			i--;
		}
	}
	PreviousBriefs[0].m_pText = text;
	PreviousBriefs[0].m_nNumber[0] = n1;
	PreviousBriefs[0].m_nNumber[1] = n2;
	PreviousBriefs[0].m_nNumber[2] = n3;
	PreviousBriefs[0].m_nNumber[3] = n4;
	PreviousBriefs[0].m_nNumber[4] = n5;
	PreviousBriefs[0].m_nNumber[5] = n6;
	PreviousBriefs[0].m_pString = string;
}

void
CMessages::InsertNumberInString(wchar *str, const int32 n1, const int32 n2, const int32 n3, const int32 n4, const int32 n5, const int32 n6, wchar *outstr)
{
	char numStr[10];
	wchar wNumStr[10];

	if (str == nullptr) {
		*outstr = '\0';
		return;
	}

	sprintf(numStr, "%d", n1);
	size_t outLen = strlen(numStr);
	AsciiToUnicode(numStr, wNumStr);
	if (str[0] == 0) {
		*outstr = '\0';
		return;
	}

	const int32 size = GetWideStringLength(str);

	int32 i = 0;

	for (int32 c = 0; c < size;) {
#ifdef MORE_LANGUAGES
		if ((CFont::IsJapanese() && str[c] == (0x8000 | '~') && str[c + 1] == (0x8000 | '1') && str[c + 2] == (0x8000 | '~')) ||
			(!CFont::IsJapanese() && str[c] == '~' && str[c + 1] == '1' && str[c + 2] == '~')) {
#else
		if (str[c] == '~' && str[c + 1] == '1' && str[c + 2] == '~') {
#endif
			c += 3;
			for (int j = 0; j < outLen; )
				*(outstr++) = wNumStr[j++];

			i++;
			switch (i) {
			case 1: sprintf(numStr, "%d", n2); break;
			case 2: sprintf(numStr, "%d", n3); break;
			case 3: sprintf(numStr, "%d", n4); break;
			case 4: sprintf(numStr, "%d", n5); break;
			case 5: sprintf(numStr, "%d", n6); break;
			}
			outLen = strlen(numStr);
			AsciiToUnicode(numStr, wNumStr);
		} else {
			*(outstr++) = str[c++];
		}
	}
	*outstr = '\0';
}

void
CMessages::InsertStringInString(wchar *str1, const wchar *str2)
{
	wchar tempstr[256];

	if (!str1 || !str2) return;

	const int32 str1_size = GetWideStringLength(str1);
	const int32 str2_size = GetWideStringLength(str2);
	const int32 total_size = str1_size + str2_size;

	const wchar *_str1 = str1;
	uint16 i;
	for (i = 0; i < total_size; ) {
#ifdef MORE_LANGUAGES
		if ((CFont::IsJapanese() && *_str1 == (0x8000 | '~') && *(_str1 + 1) == (0x8000 | 'a') && *(_str1 + 2) == (0x8000 | '~'))
			|| (*_str1 == '~' && *(_str1 + 1) == 'a' && *(_str1 + 2) == '~')) {
#else
		if (*_str1 == '~' && *(_str1 + 1) == 'a' && *(_str1 + 2) == '~') {
#endif
			_str1 += 3;
			for (int j = 0; j < str2_size; j++) {
				tempstr[i++] = str2[j];
			}
		} else {
			tempstr[i++] = *(_str1++);
		}
	}
	tempstr[i] = '\0';

	for (i = 0; i < total_size; i++)
		str1[i] = tempstr[i];

	while (i < 256)
		str1[i++] = '\0';
}

void
CMessages::InsertPlayerControlKeysInString(wchar *str)
{
	uint16 i;
	wchar outstr[256];
	wchar keybuf[256];

	if (!str) return;
	const uint16 strSize = GetWideStringLength(str);
	memset(keybuf, 0, 256*sizeof(wchar));

	wchar *_outstr = outstr;
	for (i = 0; i < strSize;) {
#ifdef MORE_LANGUAGES
		if ((CFont::IsJapanese() && str[i] == (0x8000 | '~') && str[i + 1] == (0x8000 | 'k') && str[i + 2] == (0x8000 | '~')) ||
			(!CFont::IsJapanese() && str[i] == '~' && str[i + 1] == 'k' && str[i + 2] == '~')) {
#else
		if (str[i] == '~' && str[i + 1] == 'k' && str[i + 2] == '~') {
#endif
			i += 4;
			bool done = false;
			for (int32 cont = 0; cont < MAX_CONTROLLERACTIONS && !done; cont++) {
				if (const uint16 contSize = GetWideStringLength(ControlsManager.m_aActionNames[cont]); contSize != 0) {
					if (WideStringCompare(&str[i], ControlsManager.m_aActionNames[cont], contSize)) {
						done = true;
						ControlsManager.GetWideStringOfCommandKeys(cont, keybuf, 256);
						const uint16 keybuf_size = GetWideStringLength(keybuf);
						for (uint16 j = 0; j < keybuf_size; j++) {
							*_outstr++ = keybuf[j];
							keybuf[j] = '\0';
						}
						i += contSize + 1;
					}
				}
			}
		} else {
			*_outstr++ = str[i++];
		}
	}
	*_outstr = '\0';

	for (i = 0; i < GetWideStringLength(outstr); i++)
		str[i] = outstr[i];

	while (i < 256)
		str[i++] = '\0';
}

void
CMessages::AddMessageWithNumber(wchar *str, const uint32 time, const uint16 flag, const int32 n1, const int32 n2, const int32 n3, const int32 n4, const int32 n5, const int32 n6)
{
	wchar outstr[512]; // unused
	InsertNumberInString(str, n1, n2, n3, n4, n5, n6, outstr);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	uint16 i = 0;
	while (i < NUMBRIEFMESSAGES && BriefMessages[i].m_pText != nullptr)
		i++;

	if (i >= NUMBRIEFMESSAGES) return;

	BriefMessages[i].m_pText = str;
	BriefMessages[i].m_nFlag = flag;
	BriefMessages[i].m_nTime = time;
	BriefMessages[i].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BriefMessages[i].m_nNumber[0] = n1;
	BriefMessages[i].m_nNumber[1] = n2;
	BriefMessages[i].m_nNumber[2] = n3;
	BriefMessages[i].m_nNumber[3] = n4;
	BriefMessages[i].m_nNumber[4] = n5;
	BriefMessages[i].m_nNumber[5] = n6;
	BriefMessages[i].m_pString = nullptr;
	if (i == 0)
		AddToPreviousBriefArray(
			BriefMessages[0].m_pText,
			BriefMessages[0].m_nNumber[0],
			BriefMessages[0].m_nNumber[1],
			BriefMessages[0].m_nNumber[2],
			BriefMessages[0].m_nNumber[3],
			BriefMessages[0].m_nNumber[4],
			BriefMessages[0].m_nNumber[5],
			BriefMessages[0].m_pString);
}

void 
CMessages::AddMessageJumpQWithNumber(wchar *str, const uint32 time, const uint16 flag, const int32 n1, const int32 n2, const int32 n3, const int32 n4, const int32 n5, const int32 n6)
{
	wchar outstr[512]; // unused
	InsertNumberInString(str, n1, n2, n3, n4, n5, n6, outstr);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	BriefMessages[0].m_pText = str;
	BriefMessages[0].m_nFlag = flag;
	BriefMessages[0].m_nTime = time;
	BriefMessages[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BriefMessages[0].m_nNumber[0] = n1;
	BriefMessages[0].m_nNumber[1] = n2;
	BriefMessages[0].m_nNumber[2] = n3;
	BriefMessages[0].m_nNumber[3] = n4;
	BriefMessages[0].m_nNumber[4] = n5;
	BriefMessages[0].m_nNumber[5] = n6;
	BriefMessages[0].m_pString = nullptr;
	AddToPreviousBriefArray(str, n1, n2, n3, n4, n5, n6, nullptr);
}

void
CMessages::AddMessageSoonWithNumber(wchar *str, const uint32 time, const uint16 flag, const int32 n1, const int32 n2, const int32 n3, const int32 n4, const int32 n5, const int32 n6)
{
	wchar outstr[512]; // unused
	InsertNumberInString(str, n1, n2, n3, n4, n5, n6, outstr);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	if (BriefMessages[0].m_pText != nullptr) {
		for (int32 i = NUMBRIEFMESSAGES-1; i > 1; i--)
			BriefMessages[i] = BriefMessages[i-1];

		BriefMessages[1].m_pText = str;
		BriefMessages[1].m_nFlag = flag;
		BriefMessages[1].m_nTime = time;
		BriefMessages[1].m_nStartTime = CTimer::GetTimeInMilliseconds();
		BriefMessages[1].m_nNumber[0] = n1;
		BriefMessages[1].m_nNumber[1] = n2;
		BriefMessages[1].m_nNumber[2] = n3;
		BriefMessages[1].m_nNumber[3] = n4;
		BriefMessages[1].m_nNumber[4] = n5;
		BriefMessages[1].m_nNumber[5] = n6;
		BriefMessages[1].m_pString = nullptr;
	} else {
		BriefMessages[0].m_pText = str;
		BriefMessages[0].m_nFlag = flag;
		BriefMessages[0].m_nTime = time;
		BriefMessages[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
		BriefMessages[0].m_nNumber[0] = n1;
		BriefMessages[0].m_nNumber[1] = n2;
		BriefMessages[0].m_nNumber[2] = n3;
		BriefMessages[0].m_nNumber[3] = n4;
		BriefMessages[0].m_nNumber[4] = n5;
		BriefMessages[0].m_nNumber[5] = n6;
		BriefMessages[0].m_pString = nullptr;
		AddToPreviousBriefArray(str, n1, n2, n3, n4, n5, n6, nullptr);
	}
}

void
CMessages::AddBigMessageWithNumber(wchar *str, const uint32 time, const uint16 style, const int32 n1, const int32 n2, const int32 n3, const int32 n4, const int32 n5, const int32 n6)
{
	wchar outstr[512]; // unused
	InsertNumberInString(str, n1, n2, n3, n4, n5, n6, outstr);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	BIGMessages[style].m_Stack[0].m_pText = str;
	BIGMessages[style].m_Stack[0].m_nFlag = 0;
	BIGMessages[style].m_Stack[0].m_nTime = time;
	BIGMessages[style].m_Stack[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BIGMessages[style].m_Stack[0].m_nNumber[0] = n1;
	BIGMessages[style].m_Stack[0].m_nNumber[1] = n2;
	BIGMessages[style].m_Stack[0].m_nNumber[2] = n3;
	BIGMessages[style].m_Stack[0].m_nNumber[3] = n4;
	BIGMessages[style].m_Stack[0].m_nNumber[4] = n5;
	BIGMessages[style].m_Stack[0].m_nNumber[5] = n6;
	BIGMessages[style].m_Stack[0].m_pString = nullptr;
}

void
CMessages::AddBigMessageWithNumberQ(wchar *str, const uint32 time, const uint16 style, const int32 n1, const int32 n2, const int32 n3, const int32 n4, const int32 n5, const int32 n6)
{
	wchar outstr[512]; // unused
	InsertNumberInString(str, n1, n2, n3, n4, n5, n6, outstr);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	int32 i = 0;

	while (i < 4 && BIGMessages[style].m_Stack[i].m_pText != nullptr)
		i++;

	if (i >= 4) return;

	BIGMessages[style].m_Stack[i].m_pText = str;
	BIGMessages[style].m_Stack[i].m_nFlag = 0;
	BIGMessages[style].m_Stack[i].m_nTime = time;
	BIGMessages[style].m_Stack[i].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BIGMessages[style].m_Stack[i].m_nNumber[0] = n1;
	BIGMessages[style].m_Stack[i].m_nNumber[1] = n2;
	BIGMessages[style].m_Stack[i].m_nNumber[2] = n3;
	BIGMessages[style].m_Stack[i].m_nNumber[3] = n4;
	BIGMessages[style].m_Stack[i].m_nNumber[4] = n5;
	BIGMessages[style].m_Stack[i].m_nNumber[5] = n6;
	BIGMessages[style].m_Stack[i].m_pString = nullptr;
}

void
CMessages::AddMessageWithString(wchar *text, const uint32 time, const uint16 flag, wchar *str)
{
	wchar outstr[512]; // unused
	WideStringCopy(outstr, text, 256);
	InsertStringInString(outstr, str);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	int32 i = 0;
	while (i < NUMBRIEFMESSAGES && BriefMessages[i].m_pText != nullptr)
		i++;

	if (i >= NUMBRIEFMESSAGES) return;

	BriefMessages[i].m_pText = text;
	BriefMessages[i].m_nFlag = flag;
	BriefMessages[i].m_nTime = time;
	BriefMessages[i].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BriefMessages[i].m_nNumber[0] = -1;
	BriefMessages[i].m_nNumber[1] = -1;
	BriefMessages[i].m_nNumber[2] = -1;
	BriefMessages[i].m_nNumber[3] = -1;
	BriefMessages[i].m_nNumber[4] = -1;
	BriefMessages[i].m_nNumber[5] = -1;
	BriefMessages[i].m_pString = str;
	if (i == 0)
		AddToPreviousBriefArray(
			BriefMessages[0].m_pText,
			BriefMessages[0].m_nNumber[0],
			BriefMessages[0].m_nNumber[1],
			BriefMessages[0].m_nNumber[2],
			BriefMessages[0].m_nNumber[3],
			BriefMessages[0].m_nNumber[4],
			BriefMessages[0].m_nNumber[5],
			BriefMessages[0].m_pString);
}

void
CMessages::AddMessageJumpQWithString(wchar *text, const uint32 time, const uint16 flag, wchar *str)
{
	wchar outstr[512]; // unused
	WideStringCopy(outstr, text, 256);
	InsertStringInString(outstr, str);
	InsertPlayerControlKeysInString(outstr);
	GetWideStringLength(outstr);

	BriefMessages[0].m_pText = text;
	BriefMessages[0].m_nFlag = flag;
	BriefMessages[0].m_nTime = time;
	BriefMessages[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
	BriefMessages[0].m_nNumber[0] = -1;
	BriefMessages[0].m_nNumber[1] = -1;
	BriefMessages[0].m_nNumber[2] = -1;
	BriefMessages[0].m_nNumber[3] = -1;
	BriefMessages[0].m_nNumber[4] = -1;
	BriefMessages[0].m_nNumber[5] = -1;
	BriefMessages[0].m_pString = str;
	AddToPreviousBriefArray(text, -1, -1, -1, -1, -1, -1, str);
}

inline bool
FastWideStringComparison(const wchar *str1, const wchar *str2)
{
	while (*str1 == *str2) {
		++str1;
		++str2;
		if (!*str1 && !*str2) return true;
	}
	return false;
}

void
CMessages::ClearThisPrint(const wchar *str)
{
	bool equal;

	do {
		equal = false;
		uint16 i;
		for (i = 0; i < NUMBRIEFMESSAGES && BriefMessages[i].m_pText != nullptr; i++) {
			equal = FastWideStringComparison(str, BriefMessages[i].m_pText);

			if (equal) break;
		}

		if (equal) {
			if (i != 0) {
				BriefMessages[i].m_pText = nullptr;
				for (; i < NUMBRIEFMESSAGES-1 && BriefMessages[i+1].m_pText != nullptr; i++) {
					BriefMessages[i] = BriefMessages[i + 1];
				}
				BriefMessages[i].m_pText = nullptr;
			} else {
				BriefMessages[0].m_pText = nullptr;
				for (; i < NUMBRIEFMESSAGES-1 && BriefMessages[i+1].m_pText != nullptr; i++) {
					BriefMessages[i] = BriefMessages[i + 1];
				}
				BriefMessages[i].m_pText = nullptr;
				BriefMessages[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
				if (BriefMessages[0].m_pText != nullptr)
					AddToPreviousBriefArray(
						BriefMessages[0].m_pText,
						BriefMessages[0].m_nNumber[0],
						BriefMessages[0].m_nNumber[1],
						BriefMessages[0].m_nNumber[2],
						BriefMessages[0].m_nNumber[3],
						BriefMessages[0].m_nNumber[4],
						BriefMessages[0].m_nNumber[5],
						BriefMessages[0].m_pString);
			}
		}
	} while (equal);
}

void
CMessages::ClearThisBigPrint(const wchar *str)
{
	bool equal;

	do {
		uint16 i = 0;
		equal = false;
		uint16 style = 0;
		while (style < NUMBIGMESSAGES)
		{
			if (i >= 4)
				break;

			if (BIGMessages[style].m_Stack[i].m_pText == nullptr || equal)
				break;

			equal = FastWideStringComparison(str, BIGMessages[style].m_Stack[i].m_pText);

			if (!equal && ++i == 4) {
				i = 0;
				style++;
			}
		}
		if (equal) {
			if (i != 0) {
				BIGMessages[style].m_Stack[i].m_pText = nullptr;
				while (i < 3) {
					if (BIGMessages[style].m_Stack[i + 1].m_pText == nullptr)
						break;
					BIGMessages[style].m_Stack[i] = BIGMessages[style].m_Stack[i + 1];
					i++;
				}
				BIGMessages[style].m_Stack[i].m_pText = nullptr;
			} else {
				BIGMessages[style].m_Stack[0].m_pText = nullptr;
				i = 0;
				while (i < 3) {
					if (BIGMessages[style].m_Stack[i + 1].m_pText == nullptr)
						break;
					BIGMessages[style].m_Stack[i] = BIGMessages[style].m_Stack[i + 1];
					i++;
				}
				BIGMessages[style].m_Stack[i].m_pText = nullptr;
				BIGMessages[style].m_Stack[0].m_nStartTime = CTimer::GetTimeInMilliseconds();
			}
		}
	} while (equal);
}

void
CMessages::ClearAllMessagesDisplayedByGame()
{
	ClearMessages();
	for (auto & PreviousBrief : PreviousBriefs) {
		PreviousBrief.m_pText = nullptr;
		PreviousBrief.m_pString = nullptr;
	}
	CHud::GetRidOfAllHudMessages();
	CUserDisplay::Pager.ClearMessages();
}

#include "FileSystemManager.h"

#include "stdafx.h"
#include "MFT.h"
#include "NTFS.h"
#include "NTFS_Structures.h"
#include <windows.h>
#include "iostream"

using namespace std;

#define DRIVE_MAX 4			//������ ���� �� ���� ������ ��������� �����!!!!

DRIVEPACKET OurDrive;		//���� � ������� �� ��������
BYTE* m_pMFTRecord;
BYTE* m_puchFileData;
DWORD m_dwCurPos;
ATTR_STANDARD m_attrStandard;
ATTR_FILENAME m_attrFilename;
//BYTE *uchTmpData;
DWORD m_dwFileDataSz;

void ScanPartition()	//���� ��� ������� �� �����
{
	PARTITION* Partition;
	DRIVEPACKET stDrive[DRIVE_MAX];
	WORD wDrive = 0;
	DWORD dwMainPrevRelSector = 0;
	DWORD dwPrevRelSector = 0;
	BYTE szSector[512];
	DWORD dwBytes;
	int i, nRet;
	HANDLE hDrive = CreateFile(TEXT("\\\\.\\PhysicalDrive0"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hDrive == INVALID_HANDLE_VALUE)
		return;
	ReadFile(hDrive, szSector, 512, &dwBytes, 0);
	Partition = (PARTITION*)(szSector + 0x1BE);
	for (i = 0; i < 4; i++) {
		stDrive[wDrive].wCylinder = Partition->chCylinder;
		stDrive[wDrive].wHead = Partition->chHead;
		stDrive[wDrive].wSector = Partition->chSector;
		stDrive[wDrive].dwNumSectors = Partition->dwNumberSectors;
		stDrive[wDrive].wType = ((Partition->chType == PART_EXTENDED) || (Partition->chType == PART_DOSX13X)) ? EXTENDED_PART : BOOT_RECORD;
		if ((Partition->chType == PART_EXTENDED) || (Partition->chType == PART_DOSX13X)) {
			dwMainPrevRelSector = Partition->dwRelativeSector;
			stDrive[wDrive].dwNTRelativeSector = dwMainPrevRelSector;
		}
		else stDrive[wDrive].dwNTRelativeSector = dwMainPrevRelSector + Partition->dwRelativeSector;
		if (stDrive[wDrive].wType == EXTENDED_PART) break;
		if (Partition->chType == 0) break;
		Partition++;
		cout << "Was found partition: " << wDrive << "\n";
		wDrive++;
	}
	if (i == 4) return;
	for (int LogiHard = 0; LogiHard < 50; LogiHard++) // scanning extended partitions
	{
		if (stDrive[wDrive].wType == EXTENDED_PART) {
			LARGE_INTEGER n64Pos;
			n64Pos.QuadPart = ((LONGLONG)stDrive[wDrive].dwNTRelativeSector) * 512;
			nRet = SetFilePointer(hDrive, n64Pos.LowPart, &n64Pos.HighPart, FILE_BEGIN);
			if (nRet == 0xffffffff) return;
			dwBytes = 0;
			nRet = ReadFile(hDrive, szSector, 512, (DWORD*)&dwBytes, NULL);
			if (!nRet) return;
			if (dwBytes != 512) return;
			Partition = (PARTITION*)(szSector + 0x1BE);
			for (i = 0; i < 4; i++) {
				stDrive[wDrive].wCylinder = Partition->chCylinder;
				stDrive[wDrive].wHead = Partition->chHead;
				stDrive[wDrive].dwNumSectors = Partition->dwNumberSectors;
				stDrive[wDrive].wSector = Partition->chSector;
				stDrive[wDrive].dwRelativeSector = 0;
				stDrive[wDrive].wType = ((Partition->chType == PART_EXTENDED) || (Partition->chType == PART_DOSX13X)) ? EXTENDED_PART : BOOT_RECORD;
				if ((Partition->chType == PART_EXTENDED) || (Partition->chType == PART_DOSX13X)) {
					dwPrevRelSector = Partition->dwRelativeSector;
					stDrive[wDrive].dwNTRelativeSector = dwPrevRelSector + dwMainPrevRelSector;
				}
				else stDrive[wDrive].dwNTRelativeSector = dwMainPrevRelSector + dwPrevRelSector + Partition->dwRelativeSector;
				if (stDrive[wDrive].wType == EXTENDED_PART) break;
				if (Partition->chType == 0) break;
				Partition++;
				cout << "Was found partition: " << wDrive << "\n";
				wDrive++;
			}
			if (i == 4) break;
		}
	}
	cout << "Please, choose the drive: ";
	int DriveNum;
	cin >> DriveNum;
	memcpy(&OurDrive, &stDrive[DriveNum], sizeof(DRIVEPACKET));
	CloseHandle(hDrive);
	return;
}

// ������ ������ � �����
int ReadRaw(LONGLONG n64LCN, BYTE* chData, DWORD& dwLen, OUR_WORK_INFO* info)
{
	int nRet;
	LARGE_INTEGER n64Pos;	//���������� �����

	n64Pos.QuadPart = (n64LCN) * (info->dwBytesPerCluster);	//��������� �������� � �����
	n64Pos.QuadPart += (LONGLONG)info->dwStartSector * info->dwBytesPerSector;	//� ������ �������� ������������ ������ ����������

	//������������� ������� �� �����
	nRet = SetFilePointer(info->hDrive, n64Pos.LowPart, &n64Pos.HighPart, FILE_BEGIN);
	if (nRet == 0xFFFFFFFF) return GetLastError();

	BYTE* pTmp = chData;
	DWORD dwBytesRead = 0;
	DWORD dwBytes = 0;
	DWORD dwTotRead = 0;

	//������ ������ � ������� ��������
	while (dwTotRead < dwLen)
	{
		//������ ������
		dwBytesRead = info->dwBytesPerCluster;
		//������
		nRet = ReadFile(info->hDrive, pTmp, dwBytesRead, &dwBytes, NULL);
		if (!nRet) return GetLastError();
		dwTotRead += dwBytes;
		pTmp += dwBytes;
	}
	//����� ����������� ����
	dwLen = dwTotRead;

	return ERROR_SUCCESS;
}

int ExtractData(NTFS_ATTRIBUTE ntfsAttr, BYTE*& puchData, DWORD& dwDataLen, DWORD m_dwCurPos, OUR_WORK_INFO* info, BYTE* m_pMFTRecord)
{
	DWORD dwCurPos = m_dwCurPos;
	if (!ntfsAttr.uchNonResFlag)	//����������� ����? ���� ��, �� ������ �������� ������
	{
		puchData = new BYTE[ntfsAttr.Attr.Resident.dwLength];
		dwDataLen = ntfsAttr.Attr.Resident.dwLength;
		memcpy(puchData, &m_pMFTRecord[m_dwCurPos + ntfsAttr.Attr.Resident.wAttrOffset], dwDataLen);
	}
	else	//������������� ���� => ����� �������� ���������� �� ����� �����
	{

		//if(!ntfsAttr.Attr.NonResident.n64AllocSize) // i don't know Y, but fails when its zero
		//	ntfsAttr.Attr.NonResident.n64AllocSize = (ntfsAttr.Attr.NonResident.n64EndVCN - ntfsAttr.Attr.NonResident.n64StartVCN) + 1;

		dwDataLen = ntfsAttr.Attr.NonResident.n64RealSize;				//����������� ������ ����������� ��������
		puchData = new BYTE[ntfsAttr.Attr.NonResident.n64AllocSize];	//���������� ������ ����������� ��������

		BYTE chLenOffSz;			//
		BYTE chLenSz;				//
		BYTE chOffsetSz;			//
		LONGLONG n64Len, n64Offset; //�������� ������ � ��������
		LONGLONG n64LCN = 0;			//�������� �������� ����� �����
		BYTE* pTmpBuff = puchData;
		int nRet;

		dwCurPos += ntfsAttr.Attr.NonResident.wDatarunOffset;;			//����� �������� � ������ (���� = 64, �� ������� ����� �� ���� NTFS_ATTRIBUTE)

		for (;;)
		{
			//����� ������ ���� ������ ����� NTFS_ATTRIBUTE
			chLenOffSz = 0;
			memcpy(&chLenOffSz, &m_pMFTRecord[dwCurPos], sizeof(BYTE));

			//��������� �� ��. ����
			dwCurPos += sizeof(BYTE);
			//�����?
			if (!chLenOffSz) break;	//������� ���� ������������� �� ����� ������
			/* ��������� ���������� 16-������ ����� �� 2. ��������:			 *
			 *			����� ��������� �����: 21h
			 * ����� �� ��� ��������� �������������� �� 2 � 1
			 */
			chLenSz = chLenOffSz & 0x0F;		//��������� ������ ���� ����� ������� (��. ����)
			chOffsetSz = (chLenOffSz & 0xF0) >> 4;	//������ ���� ���������� �������� (��. ����) - ����������� �����

			//��������� ����� �������
			n64Len = 0;
			memcpy(&n64Len, &m_pMFTRecord[dwCurPos], chLenSz);
			dwCurPos += chLenSz;

			//��������� ����� ���������� �������� �������
			n64Offset = 0;
			memcpy(&n64Offset, &m_pMFTRecord[dwCurPos], chOffsetSz);
			dwCurPos += chOffsetSz;

			/*
			 * �� ������ ����� �������� ���������:
			 *		chLenSz		-	������ ���� ����� �������
			 *		chOffsetSz	-	������ ���� ���������� ��������
			 *		n64Len		-	����� ������� � ���������
			 *		n64Offset	-	��������� ����� ���������� ��������
			 *	������:
			 *		����, ������������� ����� NTFS_ATTRIBUTE:
			 *				21 18 34 56 00
			 *		chLenSz    = 1h
			 *		chOffsetSz = 2h
			 *		n64Len     = 18h
			 *		n64Offset  = 34h 56h
			 *  �.�. ������ �����: ��������� ������� - 5634h, � �������� ������� - 5634h+18h == 564Ch
			 */

			 ////// if the last bit of n64Offset is 1 then its -ve so u got to make it -ve /////
			 //if((((char*)&n64Offset)[chOffsetSz-1])&0x80)
			 //	for(int i=sizeof(LONGLONG)-1;i>(chOffsetSz-1);i--)
			 //		((char*)&n64Offset)[i] = 0xff;

			n64LCN += n64Offset;				//���. ��������� �� ����
			n64Len *= info->dwBytesPerCluster;	//��������� ����� �� ��������� � �����
			//������ ������ � ��������� ����
			nRet = ReadRaw(n64LCN, pTmpBuff, (DWORD&)n64Len, info);
			if (nRet) return nRet;
			pTmpBuff += n64Len;					//��������� �� ��. �������� ��� �������
		}
	}
	return ERROR_SUCCESS;
}

int ExtractFile(BYTE* puchMFTBuffer, DWORD dwLen, bool bExcludeData, OUR_WORK_INFO* info)
{
	//��������� �� ������ �� ������� �������
	if (info->dwMFTRecordSize > dwLen) return ERROR_INVALID_PARAMETER;
	if (!puchMFTBuffer) return ERROR_INVALID_PARAMETER;

	NTFS_MFT_FILE	ntfsMFT;		//��������� MFT-�����
	NTFS_ATTRIBUTE	ntfsAttr;		//�������  DATA
	BYTE* puchTmp = 0;
	BYTE* uchTmpData = 0;			//�������� ���� ����������, ���. ������� �� ExtractData
	DWORD dwTmpDataLen;				//����� ������
	int nRet;
	m_pMFTRecord = puchMFTBuffer;	//����� ��������� �� ����������� ����
	m_dwCurPos = 0;					//������� ������� � ����������� ������
	int Finish = 0;

	if (m_puchFileData) delete m_puchFileData;

	m_puchFileData = 0;
	m_dwFileDataSz = 0;

	//��������� ��������� ntfsMFT
	memcpy(&ntfsMFT, &m_pMFTRecord[m_dwCurPos], sizeof(NTFS_MFT_FILE));
	//���������: ��������� �� �����?
	if (memcmp(ntfsMFT.szSignature, "FILE", 4))
		return ERROR_INVALID_PARAMETER; // not the right signature
	//����� �������� �� ���������
	m_dwCurPos = ntfsMFT.wAttribOffset;
	//������������ ����� ��������������� ������
	DWORD Max_Pos = m_dwCurPos + dwLen;
	//���������� ���� ��������� ������� �����
	do
	{	//��������� ��������� ��������� ���������
		memcpy(&ntfsAttr, &m_pMFTRecord[m_dwCurPos], sizeof(NTFS_ATTRIBUTE));
		//����������� ���
		switch (ntfsAttr.dwType)
		{
		case 0x30: //FILE_NAME
			nRet = ExtractData(ntfsAttr, uchTmpData, dwTmpDataLen, m_dwCurPos, info, puchMFTBuffer);
			if (nRet) return nRet;
			memcpy(&m_attrFilename, uchTmpData, dwTmpDataLen);
			delete uchTmpData;
			uchTmpData = 0;
			dwTmpDataLen = 0;
			break;
		case 0x80: //DATA
			Finish = 1;
			if (!bExcludeData)
			{
				nRet = ExtractData(ntfsAttr, uchTmpData, dwTmpDataLen, m_dwCurPos, info, puchMFTBuffer);
				if (nRet) return nRet;
				m_dwFileDataSz = dwTmpDataLen;
				if (m_puchFileData != 0) delete[] m_puchFileData;
				m_puchFileData = new BYTE[dwTmpDataLen];
				memcpy(m_puchFileData, uchTmpData, dwTmpDataLen);

				delete uchTmpData;
				uchTmpData = 0;
				dwTmpDataLen = 0;
			}
			break;
		case 0xFFFFFFFF: // ����� 
			if (uchTmpData) delete uchTmpData;
			uchTmpData = 0;
			dwTmpDataLen = 0;
			return ERROR_SUCCESS;
		default:
			break;
		};
		m_dwCurPos += ntfsAttr.dwFullLength; //��������� �� ��������� �������
	} while (m_dwCurPos < Max_Pos && Finish == 0);
	if (uchTmpData) delete uchTmpData;
	uchTmpData = 0;
	dwTmpDataLen = 0;
	return ERROR_SUCCESS;
}
void LoadMFT(LONGLONG nStartCluster, OUR_WORK_INFO* info)
{
	LARGE_INTEGER m64;
	BYTE* buff = new BYTE[info->dwMFTRecordSize];
	DWORD dwBytes;
	BYTE* uchTmpData = 0;
	BYTE* puchTmp = 0;
	//���. ����� ������� �����
	m64.QuadPart = (LONGLONG)info->dwBytesPerSector * info->dwStartSector;
	//�������� �� MFT ������
	m64.QuadPart += (LONGLONG)info->dwBytesPerCluster * nStartCluster;
	//���������...
	SetFilePointer(info->hDrive, m64.LowPart, &m64.HighPart, FILE_BEGIN);
	//������ ������ ������ � ������� (� ������ ������ ���� FILE0)	//?????????????????????7
	//int First = 0;
	ReadFile(info->hDrive, buff, info->dwMFTRecordSize, &dwBytes, NULL);
	if (ExtractFile(buff, dwBytes, false, info) != ERROR_SUCCESS) {
		cout << "Error\n";
		return;
	}
	//�� ���� �����:
	//		m_puchFileData - �������� ����� MFT � ������
	//		m_dwFileDataSz - ������ MFT
	int i = 30;

	//����� ��� ������
	BYTE* buffer = new BYTE[m_dwFileDataSz];
	memcpy(buffer, m_puchFileData, m_dwFileDataSz);

	//��������� ������ ������ � ������
	DWORD dwLength = m_dwFileDataSz;

	//���������� ������� � ��������� �� ����� �� ������� MFT
	while (((i + 1) * info->dwMFTRecordSize) <= dwLength)
	{
		//��������� ������ �� ������
		if (ExtractFile(&buffer[i * info->dwMFTRecordSize], info->dwMFTRecordSize, true, info) != ERROR_SUCCESS) {
			cout << "Error\n";
			return;
		}
		//����������: ����� ��� ���?
		if (wmemcmp((WCHAR*)m_attrFilename.wFilename, TEXT("wcx_ftp.ini"), 10) == 0)
			break;
		i++;
	}
	//�������� �� ������� �����
	if (((i + 1) * info->dwMFTRecordSize) >= dwLength) {
		cout << "Error: File not found\n";
		return;
	}

	//������ ��������� ����.

	//��������� ��� ����
	if (ExtractFile(&buffer[i * info->dwMFTRecordSize], info->dwMFTRecordSize, false, info) != ERROR_SUCCESS) {
		cout << "Error\n";
		return;
	}
	//������� ����� ����������
	m_puchFileData[m_dwFileDataSz] = 0;
	cout << "\n----------------------------------------------------\n";
	cout << "\n         Was found information:\n\n";
	//������� ��� ����
	cout << m_puchFileData;
	cout << "\n\n----------------------------------------------------\n";

	delete[] buff;
	delete[] buffer;
}

int _tmain(int argc, _TCHAR* argv[])
{
	NTFS_PART_BOOT_SEC ntfsBS;
	OUR_WORK_INFO info;
	LARGE_INTEGER StartPos;
	DWORD dwBytes;
	//�������� ������ ������
	ScanPartition();
	//��������� HDD!!!
	info.hDrive = CreateFile(TEXT("\\\\.\\PhysicalDrive0"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (info.hDrive == INVALID_HANDLE_VALUE)
		return -1;
	//��������� ���� ���������
	info.dwStartSector = OurDrive.dwNTRelativeSector;
	info.dwBytesPerSector = 512;
	//��������� �������� ������������ ������ �����
	StartPos.QuadPart = (LONGLONG)info.dwBytesPerSector * info.dwStartSector;
	DWORD dwCur = SetFilePointer(info.hDrive, StartPos.LowPart, &StartPos.HighPart, FILE_BEGIN);	//������������� �� ������ MFT (�������� ������� ����� ������ ������)
	//������ ������� ���������� � ������ ����� � ��������� �� ������������
	if (!ReadFile(info.hDrive, &ntfsBS, sizeof(NTFS_PART_BOOT_SEC), &dwBytes, NULL))
		return -1;
	if (memcmp(ntfsBS.chOemID, "NTFS", 4)) {
		cout << "Error: this partition is not NTFS\n";
		return -1;
	}
	//����������� ���������
	info.dwBytesPerCluster = ntfsBS.bpb.uchSecPerClust * ntfsBS.bpb.wBytesPerSec;
	//info.dwMFTRecordSize = 0x01<<((-1)*((char)ntfsBS.bpb.nClustPerMFTRecord));
	info.dwMFTRecordSize = 1024;
	//
	LoadMFT(ntfsBS.bpb.n64MFTLogicalClustNum, &info);
	CloseHandle(info.hDrive);
	int i;
	cin >> i;
	return 0;
}
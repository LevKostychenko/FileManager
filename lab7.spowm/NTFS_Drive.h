#pragma once
#include <windows.h>
#include <stdlib.h>
#include "NTFS_Drive.h"
#include "MFT_Record.h"

struct NTFS_PART_BOOT_SEC
{
	char		chJumpInstruction[3];
	char		chOemID[4];
	char		chDummy[4];

	struct NTFS_BPB
	{
		WORD		wBytesPerSec;
		BYTE		uchSecPerClust;
		WORD		wReservedSec;
		BYTE		uchReserved[3];
		WORD		wUnused1;
		BYTE		uchMediaDescriptor;
		WORD		wUnused2;
		WORD		wSecPerTrack;
		WORD		wNumberOfHeads;
		DWORD		dwHiddenSec;
		DWORD		dwUnused3;
		DWORD		dwUnused4;
		LONGLONG	n64TotalSec;
		LONGLONG	n64MFTLogicalClustNum;
		LONGLONG	n64MFTMirrLogicalClustNum;
		int			nClustPerMFTRecord;
		int			nClustPerIndexRecord;
		LONGLONG	n64VolumeSerialNum;
		DWORD		dwChecksum;
	} bpb;

	char		chBootstrapCode[426];
	WORD		wSecMark;
};
class CNTFSDrive
{
protected:
	//////////////// physical drive info/////////////
	HANDLE	m_hDrive;
	DWORD m_dwStartSector;
	bool m_bInitialized;

	DWORD m_dwBytesPerCluster;
	DWORD m_dwBytesPerSector;

	int LoadMFT(LONGLONG nStartCluster);

	/////////////////// the MFT info /////////////////
	BYTE* m_puchMFT;  /// the var to hold the loaded entire MFT
	DWORD m_dwMFTLen; // size of MFT

	BYTE* m_puchMFTRecord; // 1K, or the cluster size, whichever is larger
	DWORD m_dwMFTRecordSz; // MFT record size

public:
	struct ST_FILEINFO // this struct is to retrieve the file detail from this class
	{
		char szFilename[_MAX_PATH]; // file name
		LONGLONG	n64Create;		// Creation time
		LONGLONG	n64Modify;		// Last Modify time
		LONGLONG	n64Modfil;		// Last modify of record
		LONGLONG	n64Access;		// Last Access time
		DWORD		dwAttributes;	// file attribute
		LONGLONG	n64Size;		// no of cluseters used
		bool 		bDeleted;		// if true then its deleted file
	};

	int GetFileDetail(DWORD nFileSeq, ST_FILEINFO& stFileInfo);
	int Read_File(DWORD nFileSeq, BYTE*& puchFileData, DWORD& dwFileDataLen);

	void SetDriveHandle(HANDLE hDrive);
	void  SetStartSector(DWORD dwStartSector, DWORD dwBytesPerSector);

	int Initialize();
	CNTFSDrive();
	virtual ~CNTFSDrive();

};

// NTFSDrive.cpp: implementation of the CNTFSDrive class.
//
//////////////////////////////////////////////////////////////////////


#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#define new DEBUG_NEW
#endif

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CNTFSDrive::CNTFSDrive() {
	m_bInitialized = false;
	m_hDrive = 0;
	m_dwBytesPerCluster = 0;
	m_dwBytesPerSector = 0;
	m_puchMFTRecord = 0;
	m_dwMFTRecordSz = 0;
	m_puchMFT = 0;
	m_dwMFTLen = 0;
	m_dwStartSector = 0;
}

CNTFSDrive::~CNTFSDrive() {
	if (m_puchMFT)		delete m_puchMFT;
	m_puchMFT = 0;
	m_dwMFTLen = 0;
}

void CNTFSDrive::SetDriveHandle(HANDLE hDrive) {
	m_hDrive = hDrive;
	m_bInitialized = false;
}

// this is necessary to start reading a logical drive 
void CNTFSDrive::SetStartSector(DWORD dwStartSector, DWORD dwBytesPerSector) {
	m_dwStartSector = dwStartSector;
	m_dwBytesPerSector = dwBytesPerSector;
}

// initialize will read the MFT record
///   and passes to the LoadMFT to load the entire MFT in to the memory
int CNTFSDrive::Initialize() {
	NTFS_PART_BOOT_SEC ntfsBS;
	DWORD dwBytes;
	LARGE_INTEGER n84StartPos;
	//опред. сектор начала MFT
	n84StartPos.QuadPart = (LONGLONG)m_dwBytesPerSector * m_dwStartSector;
	//позиционируем на начало MFT
	DWORD dwCur = SetFilePointer(m_hDrive, n84StartPos.LowPart, &n84StartPos.HighPart, FILE_BEGIN);
	//читаем загрузочный сектор из инф. MFT
	int nRet = ReadFile(m_hDrive, &ntfsBS, sizeof(NTFS_PART_BOOT_SEC), &dwBytes, NULL);
	if (!nRet) return GetLastError();
	if (memcmp(ntfsBS.chOemID, "NTFS", 4)) //действительно ли NTFS?
		return ERROR_INVALID_DRIVE;
	/// Cluster is the logical entity
	///  which is made up of several sectors (a physical entity) 
	m_dwBytesPerCluster = ntfsBS.bpb.uchSecPerClust * ntfsBS.bpb.wBytesPerSec;
	if (m_puchMFTRecord)	delete m_puchMFTRecord;
	m_dwMFTRecordSz = 0x01 << ((-1) * ((char)ntfsBS.bpb.nClustPerMFTRecord));
	m_puchMFTRecord = new BYTE[m_dwMFTRecordSz];
	m_bInitialized = true;
	// MFTRecord of MFT is available in the MFTRecord variable
	//   load the entire MFT using it
	nRet = LoadMFT(ntfsBS.bpb.n64MFTLogicalClustNum);
	if (nRet) {
		m_bInitialized = false;
		return nRet;
	}
	return ERROR_SUCCESS;
}

//// nStartCluster is the MFT table starting cluster
///    the first entry of record in MFT table will always have the MFT record of itself
int CNTFSDrive::LoadMFT(LONGLONG nStartCluster) {
	DWORD dwBytes;
	int nRet;
	LARGE_INTEGER n64Pos;
	if (!m_bInitialized)	return ERROR_INVALID_ACCESS;
	CMFTRecord cMFTRec;
	wchar_t uszMFTName[10];
	mbstowcs(uszMFTName, "$MFT", 10);

	// NTFS starting point
	n64Pos.QuadPart = (LONGLONG)m_dwBytesPerSector * m_dwStartSector;
	// MFT starting point
	n64Pos.QuadPart += (LONGLONG)nStartCluster * m_dwBytesPerCluster;
	//  set the pointer to the MFT start
	nRet = SetFilePointer(m_hDrive, n64Pos.LowPart, &n64Pos.HighPart, FILE_BEGIN);
	if (nRet == 0xFFFFFFFF)	return GetLastError();
	/// reading the first record in the NTFS table.
	//  the first record in the NTFS is always MFT record
	nRet = ReadFile(m_hDrive, m_puchMFTRecord, m_dwMFTRecordSz, &dwBytes, NULL);
	if (!nRet) return GetLastError();
	// now extract the MFT record just like the other MFT table records
	cMFTRec.SetDriveHandle(m_hDrive);
	cMFTRec.SetRecordInfo((LONGLONG)m_dwStartSector * m_dwBytesPerSector, m_dwMFTRecordSz, m_dwBytesPerCluster);
	nRet = cMFTRec.ExtractFile(m_puchMFTRecord, dwBytes);
	if (nRet)return nRet;
	if (memcmp(cMFTRec.m_attrFilename.wFilename, uszMFTName, 8))
		return ERROR_BAD_DEVICE; // no MFT file available
	if (m_puchMFT)delete m_puchMFT;
	m_puchMFT = 0;
	m_dwMFTLen = 0;
	// this data(m_puchFileData) is special since it is the data of entire MFT file
	m_puchMFT = new BYTE[cMFTRec.m_dwFileDataSz];
	m_dwMFTLen = cMFTRec.m_dwFileDataSz;
	// store this file to read other files
	memcpy(m_puchMFT, cMFTRec.m_puchFileData, m_dwMFTLen);
	return ERROR_SUCCESS;
}


/// this function if suceeded it will allocate the buufer and passed to the caller
//    the caller's responsibility to free it
int CNTFSDrive::Read_File(DWORD nFileSeq, BYTE*& puchFileData, DWORD& dwFileDataLen)
{
	int nRet;

	if (!m_bInitialized)
		return ERROR_INVALID_ACCESS;

	MFT_Record cFile;

	// point the record of the file in the MFT table
	memcpy(m_puchMFTRecord, &m_puchMFT[nFileSeq * m_dwMFTRecordSz], m_dwMFTRecordSz);

	// Then extract that file from the drive
	cFile.SetDriveHandle(m_hDrive);
	cFile.SetRecordInfo((LONGLONG)m_dwStartSector * m_dwBytesPerSector, m_dwMFTRecordSz, m_dwBytesPerCluster);
	nRet = cFile.ExtractFile(m_puchMFTRecord, m_dwMFTRecordSz);
	if (nRet)	return nRet;
	puchFileData = new BYTE[cFile.m_dwFileDataSz];
	dwFileDataLen = cFile.m_dwFileDataSz;
	// pass the file data, It should be deallocated by the caller
	memcpy(puchFileData, cFile.m_puchFileData, dwFileDataLen);
	return ERROR_SUCCESS;
}


int CNTFSDrive::GetFileDetail(DWORD nFileSeq, ST_FILEINFO& stFileInfo)
{
	int nRet;
	if (!m_bInitialized)		return ERROR_INVALID_ACCESS;
	if ((nFileSeq * m_dwMFTRecordSz + m_dwMFTRecordSz) >= m_dwMFTLen)
		return ERROR_NO_MORE_FILES;
	MFT_Record cFile;
	// point the record of the file in the MFT table
	memcpy(m_puchMFTRecord, &m_puchMFT[nFileSeq * m_dwMFTRecordSz], m_dwMFTRecordSz);
	// read the only file detail not the file data
	cFile.SetDriveHandle(m_hDrive);
	cFile.SetRecordInfo((LONGLONG)m_dwStartSector * m_dwBytesPerSector, m_dwMFTRecordSz, m_dwBytesPerCluster);
	nRet = cFile.ExtractFile(m_puchMFTRecord, m_dwMFTRecordSz, true);
	if (nRet)	return nRet;
	// set the struct and pass the struct of file detail
	memset(&stFileInfo, 0, sizeof(ST_FILEINFO));
	wcstombs(stFileInfo.szFilename, cFile.m_attrFilename.wFilename, _MAX_PATH);
	stFileInfo.dwAttributes = cFile.m_attrFilename.dwFlags;
	stFileInfo.n64Create = cFile.m_attrStandard.n64Create;
	stFileInfo.n64Modify = cFile.m_attrStandard.n64Modify;
	stFileInfo.n64Access = cFile.m_attrStandard.n64Access;
	stFileInfo.n64Modfil = cFile.m_attrStandard.n64Modfil;
	stFileInfo.n64Size = cFile.m_attrFilename.n64Allocated;
	stFileInfo.n64Size /= m_dwBytesPerCluster;
	stFileInfo.n64Size = (!stFileInfo.n64Size) ? 1 : stFileInfo.n64Size;
	stFileInfo.bDeleted = !cFile.m_bInUse;
	return ERROR_SUCCESS;
}
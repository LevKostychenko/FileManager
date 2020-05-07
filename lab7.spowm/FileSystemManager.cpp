#include "FileSystemManager.h"

#include "stdafx.h"
#include "MFT.h"
#include "NTFS.h"
#include "NTFS_Structures.h"
#include <windows.h>
#include "iostream"

using namespace std;

#define DRIVE_MAX 4			//должен быть на один больше реального числа!!!!

DRIVEPACKET OurDrive;		//диск с которым мы работаем
BYTE* m_pMFTRecord;
BYTE* m_puchFileData;
DWORD m_dwCurPos;
ATTR_STANDARD m_attrStandard;
ATTR_FILENAME m_attrFilename;
//BYTE *uchTmpData;
DWORD m_dwFileDataSz;

void ScanPartition()	//ищем все разделы на диске
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

// читаем данные с диска
int ReadRaw(LONGLONG n64LCN, BYTE* chData, DWORD& dwLen, OUR_WORK_INFO* info)
{
	int nRet;
	LARGE_INTEGER n64Pos;	//физический адрес

	n64Pos.QuadPart = (n64LCN) * (info->dwBytesPerCluster);	//переводим кластеры в байты
	n64Pos.QuadPart += (LONGLONG)info->dwStartSector * info->dwBytesPerSector;	//с учетом смещения относителньо начало винчестера

	//устанавливаем позицию на диске
	nRet = SetFilePointer(info->hDrive, n64Pos.LowPart, &n64Pos.HighPart, FILE_BEGIN);
	if (nRet == 0xFFFFFFFF) return GetLastError();

	BYTE* pTmp = chData;
	DWORD dwBytesRead = 0;
	DWORD dwBytes = 0;
	DWORD dwTotRead = 0;

	//читаем данные в размере кластера
	while (dwTotRead < dwLen)
	{
		//размер чтения
		dwBytesRead = info->dwBytesPerCluster;
		//читаем
		nRet = ReadFile(info->hDrive, pTmp, dwBytesRead, &dwBytes, NULL);
		if (!nRet) return GetLastError();
		dwTotRead += dwBytes;
		pTmp += dwBytes;
	}
	//число прочитанных байт
	dwLen = dwTotRead;

	return ERROR_SUCCESS;
}

int ExtractData(NTFS_ATTRIBUTE ntfsAttr, BYTE*& puchData, DWORD& dwDataLen, DWORD m_dwCurPos, OUR_WORK_INFO* info, BYTE* m_pMFTRecord)
{
	DWORD dwCurPos = m_dwCurPos;
	if (!ntfsAttr.uchNonResFlag)	//резидентный блок? Если да, то просто копируем данные
	{
		puchData = new BYTE[ntfsAttr.Attr.Resident.dwLength];
		dwDataLen = ntfsAttr.Attr.Resident.dwLength;
		memcpy(puchData, &m_pMFTRecord[m_dwCurPos + ntfsAttr.Attr.Resident.wAttrOffset], dwDataLen);
	}
	else	//нерезидентный блок => будем собирать информацию по всему диску
	{

		//if(!ntfsAttr.Attr.NonResident.n64AllocSize) // i don't know Y, but fails when its zero
		//	ntfsAttr.Attr.NonResident.n64AllocSize = (ntfsAttr.Attr.NonResident.n64EndVCN - ntfsAttr.Attr.NonResident.n64StartVCN) + 1;

		dwDataLen = ntfsAttr.Attr.NonResident.n64RealSize;				//фактический размер содержимого атрибута
		puchData = new BYTE[ntfsAttr.Attr.NonResident.n64AllocSize];	//выделенный размер содержимого атрибута

		BYTE chLenOffSz;			//
		BYTE chLenSz;				//
		BYTE chOffsetSz;			//
		LONGLONG n64Len, n64Offset; //реальный размер и смещение
		LONGLONG n64LCN = 0;			//содержит реальный адрес файла
		BYTE* pTmpBuff = puchData;
		int nRet;

		dwCurPos += ntfsAttr.Attr.NonResident.wDatarunOffset;;			//берем смещение к данным (если = 64, то выходим ровно за блок NTFS_ATTRIBUTE)

		for (;;)
		{
			//берем первый байт данных после NTFS_ATTRIBUTE
			chLenOffSz = 0;
			memcpy(&chLenOffSz, &m_pMFTRecord[dwCurPos], sizeof(BYTE));

			//смещаемся на сл. байт
			dwCurPos += sizeof(BYTE);
			//пусто?
			if (!chLenOffSz) break;	//нулевой байт сигнализирует об конце списка
			/* разбиваем полученное 16-ричное число на 2. Например:			 *
			 *			Пусть прочитали число: 21h
			 * Тогда мы его разбиваем соответственно на 2 и 1
			 */
			chLenSz = chLenOffSz & 0x0F;		//описываем размер поля длины отрезка (мл. байт)
			chOffsetSz = (chLenOffSz & 0xF0) >> 4;	//размер поля начального кластера (ст. байт) - понадобится далее

			//вычисляем длину отрезка
			n64Len = 0;
			memcpy(&n64Len, &m_pMFTRecord[dwCurPos], chLenSz);
			dwCurPos += chLenSz;

			//считываем номер начального кластера отрезка
			n64Offset = 0;
			memcpy(&n64Offset, &m_pMFTRecord[dwCurPos], chOffsetSz);
			dwCurPos += chOffsetSz;

			/*
			 * На данном этапе получили следующее:
			 *		chLenSz		-	размер поля длины отрезка
			 *		chOffsetSz	-	размер поля начального кластера
			 *		n64Len		-	длина отрезка в кластерах
			 *		n64Offset	-	считываем номер начального кластера
			 *	Пример:
			 *		поле, расположенное после NTFS_ATTRIBUTE:
			 *				21 18 34 56 00
			 *		chLenSz    = 1h
			 *		chOffsetSz = 2h
			 *		n64Len     = 18h
			 *		n64Offset  = 34h 56h
			 *  Т.о. делаем вывод: начальный кластер - 5634h, а конечный кластер - 5634h+18h == 564Ch
			 */

			 ////// if the last bit of n64Offset is 1 then its -ve so u got to make it -ve /////
			 //if((((char*)&n64Offset)[chOffsetSz-1])&0x80)
			 //	for(int i=sizeof(LONGLONG)-1;i>(chOffsetSz-1);i--)
			 //		((char*)&n64Offset)[i] = 0xff;

			n64LCN += n64Offset;				//изм. указатель на файл
			n64Len *= info->dwBytesPerCluster;	//переводим длину из кластеров в байты
			//читаем данные и повторяем цикл
			nRet = ReadRaw(n64LCN, pTmpBuff, (DWORD&)n64Len, info);
			if (nRet) return nRet;
			pTmpBuff += n64Len;					//смещаемся на сл. фрагмент для анализа
		}
	}
	return ERROR_SUCCESS;
}

int ExtractFile(BYTE* puchMFTBuffer, DWORD dwLen, bool bExcludeData, OUR_WORK_INFO* info)
{
	//проверяем на выходы за пределы массива
	if (info->dwMFTRecordSize > dwLen) return ERROR_INVALID_PARAMETER;
	if (!puchMFTBuffer) return ERROR_INVALID_PARAMETER;

	NTFS_MFT_FILE	ntfsMFT;		//структура MFT-файла
	NTFS_ATTRIBUTE	ntfsAttr;		//атрибут  DATA
	BYTE* puchTmp = 0;
	BYTE* uchTmpData = 0;			//помещаем сюда информацию, кот. берется из ExtractData
	DWORD dwTmpDataLen;				//длина буфера
	int nRet;
	m_pMFTRecord = puchMFTBuffer;	//берем указатель на прочитанный блок
	m_dwCurPos = 0;					//текущая позиция в прочитанном буфере
	int Finish = 0;

	if (m_puchFileData) delete m_puchFileData;

	m_puchFileData = 0;
	m_dwFileDataSz = 0;

	//заполняем структуру ntfsMFT
	memcpy(&ntfsMFT, &m_pMFTRecord[m_dwCurPos], sizeof(NTFS_MFT_FILE));
	//проверяем: правильно ли вышли?
	if (memcmp(ntfsMFT.szSignature, "FILE", 4))
		return ERROR_INVALID_PARAMETER; // not the right signature
	//берем смещение до аттрибута
	m_dwCurPos = ntfsMFT.wAttribOffset;
	//максимальная длина обрабатываемого буфера
	DWORD Max_Pos = m_dwCurPos + dwLen;
	//организуем цикл просмтора данного блока
	do
	{	//извлекаем очередной заголовок аттрибута
		memcpy(&ntfsAttr, &m_pMFTRecord[m_dwCurPos], sizeof(NTFS_ATTRIBUTE));
		//анализируем его
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
		case 0xFFFFFFFF: // конец 
			if (uchTmpData) delete uchTmpData;
			uchTmpData = 0;
			dwTmpDataLen = 0;
			return ERROR_SUCCESS;
		default:
			break;
		};
		m_dwCurPos += ntfsAttr.dwFullLength; //смещаемся на следующий атрибут
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
	//нач. точка раздела диска
	m64.QuadPart = (LONGLONG)info->dwBytesPerSector * info->dwStartSector;
	//смещение на MFT запись
	m64.QuadPart += (LONGLONG)info->dwBytesPerCluster * nStartCluster;
	//смещаемся...
	SetFilePointer(info->hDrive, m64.LowPart, &m64.HighPart, FILE_BEGIN);
	//читаем первую запись в таблице (в теории должно быть FILE0)	//?????????????????????7
	//int First = 0;
	ReadFile(info->hDrive, buff, info->dwMFTRecordSize, &dwBytes, NULL);
	if (ExtractFile(buff, dwBytes, false, info) != ERROR_SUCCESS) {
		cout << "Error\n";
		return;
	}
	//на этом этапе:
	//		m_puchFileData - содержит копию MFT в памяти
	//		m_dwFileDataSz - размер MFT
	int i = 30;

	//буфер под данные
	BYTE* buffer = new BYTE[m_dwFileDataSz];
	memcpy(buffer, m_puchFileData, m_dwFileDataSz);

	//запомнили размер данных в буфере
	DWORD dwLength = m_dwFileDataSz;

	//организуем пропуск с проверкой на выход за границы MFT
	while (((i + 1) * info->dwMFTRecordSize) <= dwLength)
	{
		//извлекаем данные из памяти
		if (ExtractFile(&buffer[i * info->dwMFTRecordSize], info->dwMFTRecordSize, true, info) != ERROR_SUCCESS) {
			cout << "Error\n";
			return;
		}
		//сравниваем: нашли или нет?
		if (wmemcmp((WCHAR*)m_attrFilename.wFilename, TEXT("wcx_ftp.ini"), 10) == 0)
			break;
		i++;
	}
	//проверка на наличие файла
	if (((i + 1) * info->dwMFTRecordSize) >= dwLength) {
		cout << "Error: File not found\n";
		return;
	}

	//Найден требуемый файл.

	//извлекаем сам файл
	if (ExtractFile(&buffer[i * info->dwMFTRecordSize], info->dwMFTRecordSize, false, info) != ERROR_SUCCESS) {
		cout << "Error\n";
		return;
	}
	//удаляем левую информацию
	m_puchFileData[m_dwFileDataSz] = 0;
	cout << "\n----------------------------------------------------\n";
	cout << "\n         Was found information:\n\n";
	//выводим сам файл
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
	//получаем список дисков
	ScanPartition();
	//открываем HDD!!!
	info.hDrive = CreateFile(TEXT("\\\\.\\PhysicalDrive0"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (info.hDrive == INVALID_HANDLE_VALUE)
		return -1;
	//заполняем нашу структуру
	info.dwStartSector = OurDrive.dwNTRelativeSector;
	info.dwBytesPerSector = 512;
	//вычисляем смещение относительно начала диска
	StartPos.QuadPart = (LONGLONG)info.dwBytesPerSector * info.dwStartSector;
	DWORD dwCur = SetFilePointer(info.hDrive, StartPos.LowPart, &StartPos.HighPart, FILE_BEGIN);	//позиционируем на начало MFT (содержит младшую часть нового адреса)
	//читаем базовую информацию в начале диска и проверяем на правильность
	if (!ReadFile(info.hDrive, &ntfsBS, sizeof(NTFS_PART_BOOT_SEC), &dwBytes, NULL))
		return -1;
	if (memcmp(ntfsBS.chOemID, "NTFS", 4)) {
		cout << "Error: this partition is not NTFS\n";
		return -1;
	}
	//дозаполняем структуру
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
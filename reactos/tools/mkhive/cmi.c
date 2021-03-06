/*
 *  ReactOS kernel
 *  Copyright (C) 2006 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/* COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS hive maker
 * FILE:            tools/mkhive/cmi.c
 * PURPOSE:         Registry file manipulation routines
 * PROGRAMMER:      Herv� Poussineau
 */

#define NDEBUG
#include "mkhive.h"

PVOID
NTAPI
CmpAllocate(
	IN SIZE_T Size,
	IN BOOLEAN Paged,
    IN ULONG Tag)
{
	return (PVOID) malloc((size_t)Size);
}

VOID
NTAPI
CmpFree(
	IN PVOID Ptr,
    IN ULONG Quota)
{
	free(Ptr);
}

static BOOLEAN
NTAPI
CmpFileRead(
	IN PHHIVE RegistryHive,
	IN ULONG FileType,
	IN PULONG FileOffset,
	OUT PVOID Buffer,
	IN SIZE_T BufferLength)
{
	DPRINT1("CmpFileRead() unimplemented\n");
	return FALSE;
}

static BOOLEAN
NTAPI
CmpFileWrite(
	IN PHHIVE RegistryHive,
	IN ULONG FileType,
	IN PULONG FileOffset,
	IN PVOID Buffer,
	IN SIZE_T BufferLength)
{
	PCMHIVE CmHive = (PCMHIVE)RegistryHive;
	FILE *File = CmHive->FileHandles[HFILE_TYPE_PRIMARY];
	if (0 != fseek (File, *FileOffset, SEEK_SET))
		return FALSE;
	return BufferLength == fwrite (Buffer, 1, BufferLength, File);
}

static BOOLEAN
NTAPI
CmpFileSetSize(
	IN PHHIVE RegistryHive,
	IN ULONG FileType,
	IN ULONG FileSize,
    IN ULONG OldFileSize)
{
	DPRINT1("CmpFileSetSize() unimplemented\n");
	return FALSE;
}

static BOOLEAN
NTAPI
CmpFileFlush(
	IN PHHIVE RegistryHive,
	IN ULONG FileType,
    PLARGE_INTEGER FileOffset,
    ULONG Length)
{
	PCMHIVE CmHive = (PCMHIVE)RegistryHive;
	FILE *File = CmHive->FileHandles[HFILE_TYPE_PRIMARY];
	return 0 == fflush (File);
}

NTSTATUS
CmiInitializeTempHive(
	IN OUT PCMHIVE Hive)
{
	NTSTATUS Status;

	RtlZeroMemory (
		Hive,
		sizeof(CMHIVE));

	DPRINT("Hive 0x%p\n", Hive);

	Status = HvInitialize(&Hive->Hive,
                          HINIT_CREATE,
                          0,
                          0,
                          0,
                          CmpAllocate,
                          CmpFree,
                          CmpFileSetSize,
                          CmpFileWrite,
                          CmpFileRead,
                          CmpFileFlush,
                          1,
                          NULL);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	if (!CmCreateRootNode (&Hive->Hive, L""))
	{
		HvFree (&Hive->Hive);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	Hive->Flags = HIVE_NO_FILE;

	/* Add the new hive to the hive list */
	InsertTailList (
		&CmiHiveListHead,
		&Hive->HiveList);

	VERIFY_REGISTRY_HIVE (Hive);

	return STATUS_SUCCESS;
}

static NTSTATUS
CmiAddKeyToHashTable(
	IN PCMHIVE RegistryHive,
	IN OUT PCM_KEY_FAST_INDEX HashCell,
   IN HCELL_INDEX HashCellIndex,
	IN PCM_KEY_NODE NewKeyCell,
	IN HCELL_INDEX NKBOffset)
{
	ULONG i;
	ULONG HashKey = 0;

	if (NewKeyCell->Flags & KEY_COMP_NAME)
	{
		RtlCopyMemory(
			&HashKey,
			NewKeyCell->Name,
			min(NewKeyCell->NameLength, sizeof(ULONG)));
	}

	for (i = 0; i < HashCell->Count; i++)
	{
		if (HashCell->List[i].HashKey > HashKey)
			break;
	}

	if (i < HashCell->Count)
	{
		RtlMoveMemory(HashCell->List + i + 1,
		              HashCell->List + i,
		              (HashCell->Count - i) *
		              sizeof(HashCell->List[0]));
	}

	HashCell->List[i].Cell = NKBOffset;
	HashCell->List[i].HashKey = HashKey;
   HashCell->Count++;
	HvMarkCellDirty(&RegistryHive->Hive, HashCellIndex, FALSE);
	return STATUS_SUCCESS;
}

static NTSTATUS
CmiAllocateHashTableCell (
	IN PCMHIVE RegistryHive,
	OUT PCM_KEY_FAST_INDEX *HashBlock,
	OUT HCELL_INDEX *HBOffset,
	IN USHORT SubKeyCount,
	IN HSTORAGE_TYPE Storage)
{
	PCM_KEY_FAST_INDEX NewHashBlock;
	ULONG NewHashSize;
	NTSTATUS Status;

	Status = STATUS_SUCCESS;
	*HashBlock = NULL;
	NewHashSize = FIELD_OFFSET(CM_KEY_FAST_INDEX, List) +
		(SubKeyCount * sizeof(CM_INDEX));
	*HBOffset = HvAllocateCell(&RegistryHive->Hive, NewHashSize, Storage, HCELL_NIL);

	if (*HBOffset == HCELL_NIL)
	{
		Status = STATUS_INSUFFICIENT_RESOURCES;
	}
	else
	{
		NewHashBlock = (PCM_KEY_FAST_INDEX)HvGetCell (&RegistryHive->Hive, *HBOffset);
		NewHashBlock->Signature = CM_KEY_FAST_LEAF;
		NewHashBlock->Count = 0;
		*HashBlock = NewHashBlock;
	}

	return Status;
}

NTSTATUS
CmiCreateSubKey(
	IN PCMHIVE RegistryHive,
	IN HCELL_INDEX ParentKeyCellOffset,
	IN PCUNICODE_STRING SubKeyName,
	IN ULONG CreateOptions,
	OUT HCELL_INDEX* pNKBOffset)
{
	HCELL_INDEX NKBOffset;
	PCM_KEY_NODE NewKeyCell;
	ULONG NewBlockSize;
	NTSTATUS Status;
	USHORT NameLength;
	PWSTR NamePtr;
	BOOLEAN Packable;
	HSTORAGE_TYPE Storage;
	ULONG i;

	/* Skip leading backslash */
	if (SubKeyName->Buffer[0] == L'\\')
	{
		NamePtr = &SubKeyName->Buffer[1];
		NameLength = SubKeyName->Length - sizeof(WCHAR);
	}
	else
	{
		NamePtr = SubKeyName->Buffer;
		NameLength = SubKeyName->Length;
	}

	/* Check whether key name can be packed */
	Packable = TRUE;
	for (i = 0; i < NameLength / sizeof(WCHAR); i++)
	{
		if (NamePtr[i] & 0xFF00)
		{
			Packable = FALSE;
			break;
		}
	}

	/* Adjust name size */
	if (Packable)
	{
		NameLength = NameLength / sizeof(WCHAR);
	}

	Status = STATUS_SUCCESS;

	Storage = (CreateOptions & REG_OPTION_VOLATILE) ? Volatile : Stable;
	NewBlockSize = FIELD_OFFSET(CM_KEY_NODE, Name) + NameLength;
	NKBOffset = HvAllocateCell(&RegistryHive->Hive, NewBlockSize, Storage, HCELL_NIL);
	if (NKBOffset == HCELL_NIL)
	{
		Status = STATUS_INSUFFICIENT_RESOURCES;
	}
	else
	{
		NewKeyCell = (PCM_KEY_NODE)HvGetCell (&RegistryHive->Hive, NKBOffset);
		NewKeyCell->Signature = CM_KEY_NODE_SIGNATURE;
		if (CreateOptions & REG_OPTION_VOLATILE)
		{
			NewKeyCell->Flags = KEY_IS_VOLATILE;
		}
		else
		{
			NewKeyCell->Flags = 0;
		}
		KeQuerySystemTime(&NewKeyCell->LastWriteTime);
		NewKeyCell->Parent = ParentKeyCellOffset;
		NewKeyCell->SubKeyCounts[Stable] = 0;
		NewKeyCell->SubKeyCounts[Volatile] = 0;
		NewKeyCell->SubKeyLists[Stable] = HCELL_NIL;
		NewKeyCell->SubKeyLists[Volatile] = HCELL_NIL;
		NewKeyCell->ValueList.Count = 0;
		NewKeyCell->ValueList.List = HCELL_NIL;
		NewKeyCell->Security = HCELL_NIL;
		NewKeyCell->Class = HCELL_NIL;

		/* Pack the key name */
		NewKeyCell->NameLength = NameLength;
		if (Packable)
		{
			NewKeyCell->Flags |= KEY_COMP_NAME;
			for (i = 0; i < NameLength; i++)
			{
				((PCHAR)NewKeyCell->Name)[i] = (CHAR)(NamePtr[i] & 0x00FF);
			}
		}
		else
		{
			RtlCopyMemory(
				NewKeyCell->Name,
				NamePtr,
				NameLength);
		}

		VERIFY_KEY_CELL(NewKeyCell);
	}

	if (NT_SUCCESS(Status))
	{
		*pNKBOffset = NKBOffset;
	}
	return Status;
}

NTSTATUS
CmiAddSubKey(
	IN PCMHIVE RegistryHive,
	IN HCELL_INDEX ParentKeyCellOffset,
	IN PCUNICODE_STRING SubKeyName,
	IN ULONG CreateOptions,
	OUT PCM_KEY_NODE *pSubKeyCell,
	OUT HCELL_INDEX *pBlockOffset)
{
	PCM_KEY_NODE ParentKeyCell;
	HCELL_INDEX NKBOffset;
	NTSTATUS Status;

	ParentKeyCell = (PCM_KEY_NODE)HvGetCell(&RegistryHive->Hive, ParentKeyCellOffset);
	if (!ParentKeyCell)
		return STATUS_UNSUCCESSFUL;
	VERIFY_KEY_CELL(ParentKeyCell);

	/* Create the new key */
	Status = CmiCreateSubKey(RegistryHive, ParentKeyCellOffset, SubKeyName, CreateOptions, &NKBOffset);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	if (!CmpAddSubKey(&RegistryHive->Hive, ParentKeyCellOffset, NKBOffset))
	{
		/* FIXME: delete newly created cell */
		return STATUS_UNSUCCESSFUL;
	}

	KeQuerySystemTime(&ParentKeyCell->LastWriteTime);
	HvMarkCellDirty(&RegistryHive->Hive, ParentKeyCellOffset, FALSE);

	*pBlockOffset = NKBOffset;
	return STATUS_SUCCESS;
}

static BOOLEAN
CmiCompareHash(
	IN PCUNICODE_STRING KeyName,
	IN PCHAR HashString)
{
	CHAR Buffer[4];

	Buffer[0] = (KeyName->Length >= 2) ? (CHAR)KeyName->Buffer[0] : 0;
	Buffer[1] = (KeyName->Length >= 4) ? (CHAR)KeyName->Buffer[1] : 0;
	Buffer[2] = (KeyName->Length >= 6) ? (CHAR)KeyName->Buffer[2] : 0;
	Buffer[3] = (KeyName->Length >= 8) ? (CHAR)KeyName->Buffer[3] : 0;

	return (strncmp(Buffer, HashString, 4) == 0);
}


BOOLEAN
CmiCompareHashI(
	IN PCUNICODE_STRING KeyName,
	IN PCHAR HashString)
{
	CHAR Buffer[4];

	Buffer[0] = (KeyName->Length >= 2) ? (CHAR)KeyName->Buffer[0] : 0;
	Buffer[1] = (KeyName->Length >= 4) ? (CHAR)KeyName->Buffer[1] : 0;
	Buffer[2] = (KeyName->Length >= 6) ? (CHAR)KeyName->Buffer[2] : 0;
	Buffer[3] = (KeyName->Length >= 8) ? (CHAR)KeyName->Buffer[3] : 0;

	return (strncasecmp(Buffer, HashString, 4) == 0);
}

NTSTATUS
CmiScanForSubKey(
	IN PCMHIVE RegistryHive,
	IN HCELL_INDEX ParentKeyCellOffset,
	IN PCUNICODE_STRING SubKeyName,
	IN ULONG Attributes,
	OUT PCM_KEY_NODE *pSubKeyCell,
	OUT HCELL_INDEX *pBlockOffset)
{
	PCM_KEY_NODE KeyCell;
	PCM_KEY_FAST_INDEX HashBlock;
	PCM_KEY_NODE CurSubKeyCell;
	BOOLEAN CaseInsensitive;
	ULONG Storage;
	ULONG i;

	KeyCell = (PCM_KEY_NODE)HvGetCell(&RegistryHive->Hive, ParentKeyCellOffset);
	if (!KeyCell)
		return STATUS_UNSUCCESSFUL;
	VERIFY_KEY_CELL(KeyCell);

	ASSERT(RegistryHive);

	*pSubKeyCell = NULL;
    CaseInsensitive = (Attributes & OBJ_CASE_INSENSITIVE) != 0;

	for (Storage = Stable; Storage < HTYPE_COUNT; Storage++)
	{
		if (KeyCell->SubKeyLists[Storage] == HCELL_NIL)
		{
			/* The key does not have any subkeys */
			continue;
		}

		/* Get hash table */
		HashBlock = (PCM_KEY_FAST_INDEX)HvGetCell (&RegistryHive->Hive, KeyCell->SubKeyLists[Storage]);
		if (!HashBlock || HashBlock->Signature != CM_KEY_FAST_LEAF)
			return STATUS_UNSUCCESSFUL;

		for (i = 0; i < KeyCell->SubKeyCounts[Storage]; i++)
		{
            if ((HashBlock->List[i].HashKey == 0) ||
                (CmCompareHash(SubKeyName, (PCHAR)&HashBlock->List[i].HashKey, CaseInsensitive)))
            {
                CurSubKeyCell = (PCM_KEY_NODE)HvGetCell (
                    &RegistryHive->Hive,
                    HashBlock->List[i].Cell);

                if (CmCompareKeyName(CurSubKeyCell, SubKeyName, CaseInsensitive))
                {
                    *pSubKeyCell = CurSubKeyCell;
                    *pBlockOffset = HashBlock->List[i].Cell;
                    return STATUS_SUCCESS;
                }
            }
		}
	}

	return STATUS_OBJECT_NAME_NOT_FOUND;
}

static USHORT
CmiGetPackedNameLength(
	IN PCUNICODE_STRING Name,
	OUT PBOOLEAN pPackable)
{
	USHORT i;

	*pPackable = TRUE;

	for (i = 0; i < Name->Length / sizeof(WCHAR); i++)
	{
		if (Name->Buffer[i] & 0xFF00)
		{
			*pPackable = FALSE;
			return Name->Length;
		}
	}

	return (Name->Length / sizeof(WCHAR));
}

static NTSTATUS
CmiAllocateValueCell(
	IN PCMHIVE RegistryHive,
	OUT PCM_KEY_VALUE *ValueCell,
	OUT HCELL_INDEX *VBOffset,
	IN PCUNICODE_STRING ValueName,
	IN HSTORAGE_TYPE Storage)
{
	PCM_KEY_VALUE NewValueCell;
	BOOLEAN Packable;
	USHORT NameLength, i;
	NTSTATUS Status;

	Status = STATUS_SUCCESS;

	NameLength = CmiGetPackedNameLength(ValueName, &Packable);

	DPRINT("ValueName->Length %u  NameLength %u\n", ValueName->Length, NameLength);

	*VBOffset = HvAllocateCell(&RegistryHive->Hive, sizeof(CM_KEY_VALUE) + NameLength, Storage, HCELL_NIL);
	if (*VBOffset == HCELL_NIL)
	{
		Status = STATUS_INSUFFICIENT_RESOURCES;
	}
	else
	{
		NewValueCell = (PCM_KEY_VALUE)HvGetCell (&RegistryHive->Hive, *VBOffset);
		NewValueCell->Signature = CM_KEY_VALUE_SIGNATURE;
		NewValueCell->NameLength = (USHORT)NameLength;
		if (Packable)
		{
			/* Pack the value name */
			for (i = 0; i < NameLength; i++)
				((PCHAR)NewValueCell->Name)[i] = (CHAR)ValueName->Buffer[i];
			NewValueCell->Flags |= VALUE_COMP_NAME;
		}
		else
		{
			/* Copy the value name */
			RtlCopyMemory(
				NewValueCell->Name,
				ValueName->Buffer,
				NameLength);
			NewValueCell->Flags = 0;
		}
		NewValueCell->Type = 0;
		NewValueCell->DataLength = 0;
		NewValueCell->Data = HCELL_NIL;
		*ValueCell = NewValueCell;
	}

	return Status;
}

NTSTATUS
CmiAddValueKey(
	IN PCMHIVE RegistryHive,
	IN HCELL_INDEX KeyCellOffset,
	IN PCUNICODE_STRING ValueName,
	OUT PCM_KEY_VALUE *pValueCell,
	OUT HCELL_INDEX *pValueCellOffset)
{
	PVALUE_LIST_CELL ValueListCell;
	PCM_KEY_NODE KeyCell;
	PCM_KEY_VALUE NewValueCell;
	HCELL_INDEX ValueListCellOffset;
	HCELL_INDEX NewValueCellOffset;
	ULONG CellSize;
	HSTORAGE_TYPE Storage;
	NTSTATUS Status;

	KeyCell = HvGetCell(&RegistryHive->Hive, KeyCellOffset);
	if (!KeyCell)
		return STATUS_UNSUCCESSFUL;
	Storage = (KeyCell->Flags & KEY_IS_VOLATILE) ? Volatile : Stable;
	if (KeyCell->ValueList.List == HCELL_NIL)
	{
		/* Allocate some room for the value list */
		CellSize = sizeof(VALUE_LIST_CELL) + (3 * sizeof(HCELL_INDEX));
		ValueListCellOffset = HvAllocateCell(&RegistryHive->Hive, CellSize, Storage, HCELL_NIL);
		if (ValueListCellOffset == HCELL_NIL)
			return STATUS_INSUFFICIENT_RESOURCES;

		ValueListCell = (PVALUE_LIST_CELL)HvGetCell(&RegistryHive->Hive, ValueListCellOffset);
		if (!ValueListCell)
			return STATUS_UNSUCCESSFUL;
		KeyCell->ValueList.List = ValueListCellOffset;
		HvMarkCellDirty(&RegistryHive->Hive, KeyCellOffset, FALSE);
	}
	else
	{
		ValueListCell = (PVALUE_LIST_CELL)HvGetCell(&RegistryHive->Hive, KeyCell->ValueList.List);
		if (!ValueListCell)
			return STATUS_UNSUCCESSFUL;
		CellSize = ABS_VALUE(HvGetCellSize(&RegistryHive->Hive, ValueListCell));

		if (KeyCell->ValueList.Count >= CellSize / sizeof(HCELL_INDEX))
		{
			CellSize *= 2;
			ValueListCellOffset = HvReallocateCell(&RegistryHive->Hive, KeyCell->ValueList.List, CellSize);
			if (ValueListCellOffset == HCELL_NIL)
				return STATUS_INSUFFICIENT_RESOURCES;

			ValueListCell = (PVALUE_LIST_CELL)HvGetCell(&RegistryHive->Hive, ValueListCellOffset);
			if (!ValueListCell)
				return STATUS_UNSUCCESSFUL;
			KeyCell->ValueList.List = ValueListCellOffset;
			HvMarkCellDirty(&RegistryHive->Hive, KeyCellOffset, FALSE);
        }
	}

	Status = CmiAllocateValueCell(
		RegistryHive,
		&NewValueCell,
		&NewValueCellOffset,
		ValueName,
		Storage);
	if (!NT_SUCCESS(Status))
		return Status;

	ValueListCell->ValueOffset[KeyCell->ValueList.Count] = NewValueCellOffset;
	KeyCell->ValueList.Count++;
	if (NewValueCell->Flags & VALUE_COMP_NAME)
	{
	    if (NewValueCell->NameLength*sizeof(WCHAR) > KeyCell->MaxValueNameLen)
            KeyCell->MaxValueNameLen = NewValueCell->NameLength*sizeof(WCHAR);
	}
	else
	{
	    if (NewValueCell->NameLength > KeyCell->MaxValueNameLen)
            KeyCell->MaxValueNameLen = NewValueCell->NameLength;
	}

	HvMarkCellDirty(&RegistryHive->Hive, KeyCellOffset, FALSE);
	HvMarkCellDirty(&RegistryHive->Hive, KeyCell->ValueList.List, FALSE);
	HvMarkCellDirty(&RegistryHive->Hive, NewValueCellOffset, FALSE);

	*pValueCell = NewValueCell;
	*pValueCellOffset = NewValueCellOffset;

	return STATUS_SUCCESS;
}

NTSTATUS
CmiScanForValueKey(
	IN PCMHIVE RegistryHive,
	IN HCELL_INDEX KeyCellOffset,
	IN PCUNICODE_STRING ValueName,
	OUT PCM_KEY_VALUE *pValueCell,
	OUT HCELL_INDEX *pValueCellOffset)
{
	PCM_KEY_NODE KeyCell;
	PVALUE_LIST_CELL ValueListCell;
	PCM_KEY_VALUE CurValueCell;
	ULONG i;

	KeyCell = (PCM_KEY_NODE)HvGetCell(&RegistryHive->Hive, KeyCellOffset);
	if (!KeyCell)
		return STATUS_UNSUCCESSFUL;
	*pValueCell = NULL;
	*pValueCellOffset = HCELL_NIL;

	/* The key does not have any values */
	if (KeyCell->ValueList.List == HCELL_NIL)
    {
		return STATUS_OBJECT_NAME_NOT_FOUND;
    }

	ValueListCell = (PVALUE_LIST_CELL)HvGetCell(&RegistryHive->Hive, KeyCell->ValueList.List);

	VERIFY_VALUE_LIST_CELL(ValueListCell);

	for (i = 0; i < KeyCell->ValueList.Count; i++)
	{
		CurValueCell = (PCM_KEY_VALUE)HvGetCell(
			&RegistryHive->Hive,
			ValueListCell->ValueOffset[i]);

		if (CmComparePackedNames(ValueName,
                                 (PUCHAR)CurValueCell->Name,
                                 CurValueCell->NameLength,
                                 (CurValueCell->Flags & VALUE_COMP_NAME) ? TRUE : FALSE,
                                 TRUE))
		{
			*pValueCell = CurValueCell;
			*pValueCellOffset = ValueListCell->ValueOffset[i];
			return STATUS_SUCCESS;
		}
	}

	return STATUS_OBJECT_NAME_NOT_FOUND;
}

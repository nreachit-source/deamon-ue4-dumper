/*
 * ue4_offsets.h — UE4 reflection struct layout constants for ARM64.
 *
 * All byte offsets for reading UE4 reflection structures from a remote
 * process.  Targeting UE 4.25–4.27, ARM64 (iOS).  If the exact engine
 * version differs, edit only this file.
 *
 * These offsets assume a standard Shipping/Development build without
 * WITH_EDITORONLY_DATA or other editor-only fields compiled in.
 */

#ifndef UE4_OFFSETS_H
#define UE4_OFFSETS_H

/* ------------------------------------------------------------------ */
/*  FName  (8 bytes total)                                            */
/* ------------------------------------------------------------------ */
#define OFF_FNAME_INDEX          0x00   /* int32  ComparisonIndex      */
#define OFF_FNAME_NUMBER         0x04   /* int32  Number               */
#define FNAME_SIZE               8

/* ------------------------------------------------------------------ */
/*  FNameEntry  (FNamePool block entries, UE 4.23+)                   */
/*  Header is a uint16:                                               */
/*    bit  0     : bIsWide                                            */
/*    bits 1–5   : unused / hash probe bits                           */
/*    bits 6–15  : Len  (up to 1024 characters)                       */
/* ------------------------------------------------------------------ */
#define FNAMEENTRY_HEADER_SIZE   2
#define FNAMEENTRY_HEADER_WIDE_MASK   0x0001
#define FNAMEENTRY_HEADER_LEN_SHIFT   6

/* FNamePool block-based name resolution.                             */
/* ComparisonIndex encodes:                                           */
/*   Block  = ComparisonIndex >> FNAME_BLOCK_OFFSET_BITS              */
/*   Offset = ComparisonIndex & ((1 << FNAME_BLOCK_OFFSET_BITS) - 1)  */
#define FNAME_BLOCK_OFFSET_BITS  16

/* Stride between FNameEntries within a block.  Each entry is         */
/* header (2 bytes) + string data, rounded to 2-byte alignment.       */
/* We compute stride per-entry at read time.                          */

/* ------------------------------------------------------------------ */
/*  UObject  (base of all UE4 objects)                                */
/*  ARM64 size: 0x28 (40 bytes)                                       */
/* ------------------------------------------------------------------ */
#define OFF_UOBJECT_VFTABLE      0x00   /* void*     VTable           */
#define OFF_UOBJECT_FLAGS        0x08   /* int32     ObjectFlags      */
#define OFF_UOBJECT_INDEX        0x0C   /* int32     InternalIndex    */
#define OFF_UOBJECT_CLASS        0x10   /* UClass*   ClassPrivate     */
#define OFF_UOBJECT_NAME         0x18   /* FName     NamePrivate      */
#define OFF_UOBJECT_OUTER        0x20   /* UObject*  OuterPrivate     */
#define UOBJECT_SIZE             0x28

/* ------------------------------------------------------------------ */
/*  UField  (inherits UObject)                                        */
/*  ARM64 size: 0x30 (48 bytes)                                       */
/* ------------------------------------------------------------------ */
#define OFF_UFIELD_NEXT          0x28   /* UField*   Next             */
#define UFIELD_SIZE              0x30

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/*  UStruct  (inherits UField)                                        */
/*  Offsets for UE 4.18 (with 4.25+ fallback support)                 */
/* ------------------------------------------------------------------ */
#define OFF_USTRUCT_SUPER_418    0x30   /* UStruct*  SuperStruct (UE 4.18) */
#define OFF_USTRUCT_CHILDREN_418 0x38   /* UField*   Children (UE 4.18)    */
#define OFF_USTRUCT_PROPS_SIZE_418 0x40 /* int32     PropertiesSize (4.18) */

#define OFF_USTRUCT_SUPER_425    0x40   /* UStruct*  SuperStruct (UE 4.25) */
#define OFF_USTRUCT_CHILDREN_425 0x48   /* UField*   Children (UE 4.25)    */
#define OFF_USTRUCT_CHILD_PROPS  0x50   /* FField*   ChildProperties (4.25+) */
#define OFF_USTRUCT_PROPS_SIZE_425 0x58 /* int32     PropertiesSize (4.25) */

/* Defaults pointing to 4.18 */
#define OFF_USTRUCT_SUPER        OFF_USTRUCT_SUPER_418
#define OFF_USTRUCT_CHILDREN     OFF_USTRUCT_CHILDREN_418
#define OFF_USTRUCT_PROPS_SIZE   OFF_USTRUCT_PROPS_SIZE_418

/* ------------------------------------------------------------------ */
/*  UClass  (inherits UStruct)                                        */
/*  We only use it as a tag — if an UObject's class name ends with    */
/*  "Class", it's a UClass.  No additional offsets needed beyond       */
/*  those inherited from UStruct.                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  UFunction  (inherits UStruct)                                     */
/* ------------------------------------------------------------------ */
#define OFF_UFUNC_FLAGS_418      0x88   /* uint32  FunctionFlags (UE 4.18) */
#define OFF_UFUNC_PARMS_SIZE_418 0x8E   /* uint16  ParmsSize (UE 4.18)     */

#define OFF_UFUNC_FLAGS_425      0xB0   /* uint32  FunctionFlags (UE 4.25) */
#define OFF_UFUNC_PARMS_SIZE_425 0xB6   /* uint16  ParmsSize (UE 4.25)     */

#define OFF_UFUNC_FLAGS          OFF_UFUNC_FLAGS_418
#define OFF_UFUNC_NUM_PARMS      0x8C   /* uint8   NumParms           */
#define OFF_UFUNC_PARMS_SIZE     OFF_UFUNC_PARMS_SIZE_418
#define OFF_UFUNC_RET_OFFSET     0x90   /* uint16  ReturnValueOffset  */

/* ------------------------------------------------------------------ */
/*  FField  (base of property chain in UE 4.25+)                      */
/* ------------------------------------------------------------------ */
#define OFF_FFIELD_CLASS         0x08   /* FFieldClass*  ClassPrivate  */
#define OFF_FFIELD_NEXT          0x20   /* FField*       Next         */
#define OFF_FFIELD_NAME          0x28   /* FName         NamePrivate  */

/* ------------------------------------------------------------------ */
/*  FFieldClass  (identifies the type of an FField / FProperty)       */
/* ------------------------------------------------------------------ */
#define OFF_FFIELDCLASS_NAME     0x00   /* FName    Name              */

/* ------------------------------------------------------------------ */
/*  FProperty  (inherits FField, UE 4.25+)                            */
/* ------------------------------------------------------------------ */
#define OFF_FPROP_ARRAY_DIM      0x38   /* int32    ArrayDim          */
#define OFF_FPROP_ELEMENT_SIZE   0x3C   /* int32    ElementSize       */
#define OFF_FPROP_PROP_FLAGS     0x40   /* uint64   PropertyFlags     */
#define OFF_FPROP_OFFSET         0x4C   /* int32    Offset_Internal   */

/* ------------------------------------------------------------------ */
/*  GUObjectArray / FUObjectArray                                     */
/* ------------------------------------------------------------------ */
/* FUObjectArray contains FChunkedFixedUObjectArray at some offset.   */
/* The chunked array is the first meaningful field in FUObjectArray    */
/* after a critical-section lock.                                     */
#define OFF_GUOBJ_CHUNKED        0x10   /* FChunkedFixedUObjectArray  */

#define OFF_CHUNKED_OBJECTS      0x00   /* FUObjectItem**  Objects    */
#define OFF_CHUNKED_MAX_ELEMS    0x10   /* int32  MaxElements         */
#define OFF_CHUNKED_NUM_ELEMS    0x14   /* int32  NumElements         */
#define OFF_CHUNKED_MAX_CHUNKS   0x18   /* int32  MaxChunks           */
#define OFF_CHUNKED_NUM_CHUNKS   0x1C   /* int32  NumChunks           */

#define FUOBJECTITEM_OBJECT      0x00   /* UObject*                   */
#define FUOBJECTITEM_SIZE        0x18   /* sizeof(FUObjectItem)       */
#define ELEMENTS_PER_CHUNK       (64 * 1024)

/* ------------------------------------------------------------------ */
/*  Known class-name FName strings  (for identifying UClass objects)  */
/* ------------------------------------------------------------------ */
/* We identify UClass objects by checking if their Class pointer's    */
/* name resolves to "Class".                                          */

#endif /* UE4_OFFSETS_H */

/*
 * NDR data marshalling
 *
 * Copyright 2002 Greg Turner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * TODO:
 *  - figure out whether we *really* got this right
 *  - check for errors and throw exceptions
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winreg.h"

#include "ndr_misc.h"
#include "rpcndr.h"

#include "wine/unicode.h"
#include "wine/rpcfc.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ole);

#define BUFFER_PARANOIA 20

#if defined(__i386__)
# define LITTLE_ENDIAN_UINT32_WRITE(pchar, uint32) \
    (*((UINT32 *)(pchar)) = (uint32))

# define LITTLE_ENDIAN_UINT32_READ(pchar) \
    (*((UINT32 *)(pchar)))
#else
  /* these would work for i386 too, but less efficient */
# define LITTLE_ENDIAN_UINT32_WRITE(pchar, uint32) \
    (*(pchar)     = LOBYTE(LOWORD(uint32)), \
     *((pchar)+1) = HIBYTE(LOWORD(uint32)), \
     *((pchar)+2) = LOBYTE(HIWORD(uint32)), \
     *((pchar)+3) = HIBYTE(HIWORD(uint32)), \
     (uint32)) /* allow as r-value */

# define LITTLE_ENDIAN_UINT32_READ(pchar) \
    (MAKELONG( \
      MAKEWORD(*(pchar), *((pchar)+1)), \
      MAKEWORD(*((pchar)+2), *((pchar)+3))))
#endif

#define BIG_ENDIAN_UINT32_WRITE(pchar, uint32) \
  (*((pchar)+3) = LOBYTE(LOWORD(uint32)), \
   *((pchar)+2) = HIBYTE(LOWORD(uint32)), \
   *((pchar)+1) = LOBYTE(HIWORD(uint32)), \
   *(pchar)     = HIBYTE(HIWORD(uint32)), \
   (uint32)) /* allow as r-value */

#define BIG_ENDIAN_UINT32_READ(pchar) \
  (MAKELONG( \
    MAKEWORD(*((pchar)+3), *((pchar)+2)), \
    MAKEWORD(*((pchar)+1), *(pchar))))

#ifdef NDR_LOCAL_IS_BIG_ENDIAN
# define NDR_LOCAL_UINT32_WRITE(pchar, uint32) \
    BIG_ENDIAN_UINT32_WRITE(pchar, uint32)
# define NDR_LOCAL_UINT32_READ(pchar) \
    BIG_ENDIAN_UINT32_READ(pchar)
#else
# define NDR_LOCAL_UINT32_WRITE(pchar, uint32) \
    LITTLE_ENDIAN_UINT32_WRITE(pchar, uint32)
# define NDR_LOCAL_UINT32_READ(pchar) \
    LITTLE_ENDIAN_UINT32_READ(pchar)
#endif

/* _Align must be the desired alignment,
 * e.g. ALIGN_LENGTH(len, 4) to align on a dword boundary. */
#define ALIGNED_LENGTH(_Len, _Align) (((_Len)+(_Align)-1)&~((_Align)-1))
#define ALIGNED_POINTER(_Ptr, _Align) ((LPVOID)ALIGNED_LENGTH((ULONG_PTR)(_Ptr), _Align))
#define ALIGN_LENGTH(_Len, _Align) _Len = ALIGNED_LENGTH(_Len, _Align)
#define ALIGN_POINTER(_Ptr, _Align) _Ptr = ALIGNED_POINTER(_Ptr, _Align)

#define STD_OVERFLOW_CHECK(_Msg) do { \
    TRACE("buffer=%d/%ld\n", _Msg->Buffer - (unsigned char *)_Msg->RpcMsg->Buffer, _Msg->BufferLength); \
    if (_Msg->Buffer > (unsigned char *)_Msg->RpcMsg->Buffer + _Msg->BufferLength) \
        ERR("buffer overflow %d bytes\n", _Msg->Buffer - ((unsigned char *)_Msg->RpcMsg->Buffer + _Msg->BufferLength)); \
  } while (0)

#define NDR_TABLE_SIZE 128
#define NDR_TABLE_MASK 127

static unsigned char *WINAPI NdrBaseTypeMarshall(PMIDL_STUB_MESSAGE, unsigned char *, PFORMAT_STRING);
static unsigned char *WINAPI NdrBaseTypeUnmarshall(PMIDL_STUB_MESSAGE, unsigned char **, PFORMAT_STRING, unsigned char);
static void WINAPI NdrBaseTypeBufferSize(PMIDL_STUB_MESSAGE, unsigned char *, PFORMAT_STRING);
static void WINAPI NdrBaseTypeFree(PMIDL_STUB_MESSAGE, unsigned char *, PFORMAT_STRING);
static unsigned long WINAPI NdrBaseTypeMemorySize(PMIDL_STUB_MESSAGE, PFORMAT_STRING);

const NDR_MARSHALL NdrMarshaller[NDR_TABLE_SIZE] = {
  0,
  NdrBaseTypeMarshall, NdrBaseTypeMarshall, NdrBaseTypeMarshall,
  NdrBaseTypeMarshall, NdrBaseTypeMarshall, NdrBaseTypeMarshall, NdrBaseTypeMarshall,
  NdrBaseTypeMarshall, NdrBaseTypeMarshall, NdrBaseTypeMarshall, NdrBaseTypeMarshall,
  NdrBaseTypeMarshall, NdrBaseTypeMarshall, NdrBaseTypeMarshall, NdrBaseTypeMarshall,
  /* 0x10 */
  NdrBaseTypeMarshall,
  /* 0x11 */
  NdrPointerMarshall, NdrPointerMarshall,
  NdrPointerMarshall, NdrPointerMarshall,
  /* 0x15 */
  NdrSimpleStructMarshall, NdrSimpleStructMarshall,
  NdrConformantStructMarshall, NdrConformantStructMarshall,
  NdrConformantVaryingStructMarshall,
  NdrComplexStructMarshall,
  /* 0x1b */
  NdrConformantArrayMarshall, 
  NdrConformantVaryingArrayMarshall,
  NdrFixedArrayMarshall, NdrFixedArrayMarshall,
  NdrVaryingArrayMarshall, NdrVaryingArrayMarshall,
  NdrComplexArrayMarshall,
  /* 0x22 */
  NdrConformantStringMarshall, 0, 0,
  NdrConformantStringMarshall,
  NdrNonConformantStringMarshall, 0, 0, 0,
  /* 0x2a */
  NdrEncapsulatedUnionMarshall,
  NdrNonEncapsulatedUnionMarshall,
  0,
  NdrXmitOrRepAsMarshall, NdrXmitOrRepAsMarshall,
  /* 0x2f */
  NdrInterfacePointerMarshall,
  /* 0xb0 */
  0, 0, 0, 0,
  NdrUserMarshalMarshall
};
const NDR_UNMARSHALL NdrUnmarshaller[NDR_TABLE_SIZE] = {
  0,
  NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall,
  NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall,
  NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall,
  NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall, NdrBaseTypeUnmarshall,
  /* 0x10 */
  NdrBaseTypeUnmarshall,
  /* 0x11 */
  NdrPointerUnmarshall, NdrPointerUnmarshall,
  NdrPointerUnmarshall, NdrPointerUnmarshall,
  /* 0x15 */
  NdrSimpleStructUnmarshall, NdrSimpleStructUnmarshall,
  NdrConformantStructUnmarshall, NdrConformantStructUnmarshall,
  NdrConformantVaryingStructUnmarshall,
  NdrComplexStructUnmarshall,
  /* 0x1b */
  NdrConformantArrayUnmarshall, 
  NdrConformantVaryingArrayUnmarshall,
  NdrFixedArrayUnmarshall, NdrFixedArrayUnmarshall,
  NdrVaryingArrayUnmarshall, NdrVaryingArrayUnmarshall,
  NdrComplexArrayUnmarshall,
  /* 0x22 */
  NdrConformantStringUnmarshall, 0, 0,
  NdrConformantStringUnmarshall,
  NdrNonConformantStringUnmarshall, 0, 0, 0,
  /* 0x2a */
  NdrEncapsulatedUnionUnmarshall,
  NdrNonEncapsulatedUnionUnmarshall,
  0,
  NdrXmitOrRepAsUnmarshall, NdrXmitOrRepAsUnmarshall,
  /* 0x2f */
  NdrInterfacePointerUnmarshall,
  /* 0xb0 */
  0, 0, 0, 0,
  NdrUserMarshalUnmarshall
};
const NDR_BUFFERSIZE NdrBufferSizer[NDR_TABLE_SIZE] = {
  0,
  NdrBaseTypeBufferSize, NdrBaseTypeBufferSize, NdrBaseTypeBufferSize,
  NdrBaseTypeBufferSize, NdrBaseTypeBufferSize, NdrBaseTypeBufferSize, NdrBaseTypeBufferSize,
  NdrBaseTypeBufferSize, NdrBaseTypeBufferSize, NdrBaseTypeBufferSize, NdrBaseTypeBufferSize,
  NdrBaseTypeBufferSize, NdrBaseTypeBufferSize, NdrBaseTypeBufferSize, NdrBaseTypeBufferSize,
  /* 0x10 */
  NdrBaseTypeBufferSize,
  /* 0x11 */
  NdrPointerBufferSize, NdrPointerBufferSize,
  NdrPointerBufferSize, NdrPointerBufferSize,
  /* 0x15 */
  NdrSimpleStructBufferSize, NdrSimpleStructBufferSize,
  NdrConformantStructBufferSize, NdrConformantStructBufferSize,
  NdrConformantVaryingStructBufferSize,
  NdrComplexStructBufferSize,
  /* 0x1b */
  NdrConformantArrayBufferSize, 
  NdrConformantVaryingArrayBufferSize,
  NdrFixedArrayBufferSize, NdrFixedArrayBufferSize,
  NdrVaryingArrayBufferSize, NdrVaryingArrayBufferSize,
  NdrComplexArrayBufferSize,
  /* 0x22 */
  NdrConformantStringBufferSize, 0, 0,
  NdrConformantStringBufferSize,
  NdrNonConformantStringBufferSize, 0, 0, 0,
  /* 0x2a */
  NdrEncapsulatedUnionBufferSize,
  NdrNonEncapsulatedUnionBufferSize,
  0,
  NdrXmitOrRepAsBufferSize, NdrXmitOrRepAsBufferSize,
  /* 0x2f */
  NdrInterfacePointerBufferSize,
  /* 0xb0 */
  0, 0, 0, 0,
  NdrUserMarshalBufferSize
};
const NDR_MEMORYSIZE NdrMemorySizer[NDR_TABLE_SIZE] = {
  0,
  NdrBaseTypeMemorySize, NdrBaseTypeMemorySize, NdrBaseTypeMemorySize,
  NdrBaseTypeMemorySize, NdrBaseTypeMemorySize, NdrBaseTypeMemorySize, NdrBaseTypeMemorySize,
  NdrBaseTypeMemorySize, NdrBaseTypeMemorySize, NdrBaseTypeMemorySize, NdrBaseTypeMemorySize,
  NdrBaseTypeMemorySize, NdrBaseTypeMemorySize, NdrBaseTypeMemorySize, NdrBaseTypeMemorySize,
  /* 0x10 */
  NdrBaseTypeMemorySize,
  /* 0x11 */
  NdrPointerMemorySize, NdrPointerMemorySize,
  NdrPointerMemorySize, NdrPointerMemorySize,
  /* 0x15 */
  NdrSimpleStructMemorySize, NdrSimpleStructMemorySize,
  0, 0, 0,
  NdrComplexStructMemorySize,
  /* 0x1b */
  NdrConformantArrayMemorySize, 0, 0, 0, 0, 0,
  NdrComplexArrayMemorySize,
  /* 0x22 */
  NdrConformantStringMemorySize, 0, 0,
  NdrConformantStringMemorySize,
  NdrNonConformantStringMemorySize, 0, 0, 0,
  /* 0x2a */
  0, 0, 0, 0, 0,
  /* 0x2f */
  NdrInterfacePointerMemorySize,
  /* 0xb0 */
  0, 0, 0, 0,
  NdrUserMarshalMemorySize
};
const NDR_FREE NdrFreer[NDR_TABLE_SIZE] = {
  0,
  NdrBaseTypeFree, NdrBaseTypeFree, NdrBaseTypeFree,
  NdrBaseTypeFree, NdrBaseTypeFree, NdrBaseTypeFree, NdrBaseTypeFree,
  NdrBaseTypeFree, NdrBaseTypeFree, NdrBaseTypeFree, NdrBaseTypeFree,
  NdrBaseTypeFree, NdrBaseTypeFree, NdrBaseTypeFree, NdrBaseTypeFree,
  /* 0x10 */
  NdrBaseTypeFree,
  /* 0x11 */
  NdrPointerFree, NdrPointerFree,
  NdrPointerFree, NdrPointerFree,
  /* 0x15 */
  NdrSimpleStructFree, NdrSimpleStructFree,
  NdrConformantStructFree, NdrConformantStructFree,
  NdrConformantVaryingStructFree,
  NdrComplexStructFree,
  /* 0x1b */
  NdrConformantArrayFree, 
  NdrConformantVaryingArrayFree,
  NdrFixedArrayFree, NdrFixedArrayFree,
  NdrVaryingArrayFree, NdrVaryingArrayFree,
  NdrComplexArrayFree,
  /* 0x22 */
  0, 0, 0,
  0, 0, 0, 0, 0,
  /* 0x2a */
  NdrEncapsulatedUnionFree,
  NdrNonEncapsulatedUnionFree,
  0,
  NdrXmitOrRepAsFree, NdrXmitOrRepAsFree,
  /* 0x2f */
  NdrInterfacePointerFree,
  /* 0xb0 */
  0, 0, 0, 0,
  NdrUserMarshalFree
};

void * WINAPI NdrAllocate(MIDL_STUB_MESSAGE *pStubMsg, size_t len)
{
  /* hmm, this is probably supposed to do more? */
  return pStubMsg->pfnAllocate(len);
}

static void WINAPI NdrFree(MIDL_STUB_MESSAGE *pStubMsg, unsigned char *Pointer)
{
  pStubMsg->pfnFree(Pointer);
}

static inline BOOL IsConformanceOrVariancePresent(PFORMAT_STRING pFormat)
{
    return (*(const ULONG *)pFormat != -1);
}

PFORMAT_STRING ReadConformance(MIDL_STUB_MESSAGE *pStubMsg, PFORMAT_STRING pFormat)
{
  ALIGN_POINTER(pStubMsg->Buffer, 4);
  pStubMsg->MaxCount = NDR_LOCAL_UINT32_READ(pStubMsg->Buffer);
  pStubMsg->Buffer += 4;
  TRACE("unmarshalled conformance is %ld\n", pStubMsg->MaxCount);
  if (pStubMsg->fHasNewCorrDesc)
    return pFormat+6;
  else
    return pFormat+4;
}

static inline PFORMAT_STRING ReadVariance(MIDL_STUB_MESSAGE *pStubMsg, PFORMAT_STRING pFormat)
{
  if (pFormat && !IsConformanceOrVariancePresent(pFormat))
  {
    pStubMsg->Offset = 0;
    pStubMsg->ActualCount = pStubMsg->MaxCount;
    goto done;
  }

  ALIGN_POINTER(pStubMsg->Buffer, 4);
  pStubMsg->Offset      = NDR_LOCAL_UINT32_READ(pStubMsg->Buffer);
  pStubMsg->Buffer += 4;
  TRACE("offset is %ld\n", pStubMsg->Offset);
  pStubMsg->ActualCount = NDR_LOCAL_UINT32_READ(pStubMsg->Buffer);
  pStubMsg->Buffer += 4;
  TRACE("variance is %ld\n", pStubMsg->ActualCount);

done:
  if (pStubMsg->fHasNewCorrDesc)
    return pFormat+6;
  else
    return pFormat+4;
}

/* writes the conformance value to the buffer */
static inline void WriteConformance(MIDL_STUB_MESSAGE *pStubMsg)
{
    ALIGN_POINTER(pStubMsg->Buffer, 4);
    NDR_LOCAL_UINT32_WRITE(pStubMsg->Buffer, pStubMsg->MaxCount);
    pStubMsg->Buffer += 4;
}

/* writes the variance values to the buffer */
static inline void WriteVariance(MIDL_STUB_MESSAGE *pStubMsg)
{
    ALIGN_POINTER(pStubMsg->Buffer, 4);
    NDR_LOCAL_UINT32_WRITE(pStubMsg->Buffer, pStubMsg->Offset);
    pStubMsg->Buffer += 4;
    NDR_LOCAL_UINT32_WRITE(pStubMsg->Buffer, pStubMsg->ActualCount);
    pStubMsg->Buffer += 4;
}

/* requests buffer space for the conformance value */
static inline void SizeConformance(MIDL_STUB_MESSAGE *pStubMsg)
{
    ALIGN_LENGTH(pStubMsg->BufferLength, 4);
    pStubMsg->BufferLength += 4;
}

/* requests buffer space for the variance values */
static inline void SizeVariance(MIDL_STUB_MESSAGE *pStubMsg)
{
    ALIGN_LENGTH(pStubMsg->BufferLength, 4);
    pStubMsg->BufferLength += 8;
}

PFORMAT_STRING ComputeConformanceOrVariance(
    MIDL_STUB_MESSAGE *pStubMsg, unsigned char *pMemory,
    PFORMAT_STRING pFormat, ULONG_PTR def, ULONG *pCount)
{
  BYTE dtype = pFormat[0] & 0xf;
  short ofs = *(short *)&pFormat[2];
  LPVOID ptr = NULL;
  DWORD data = 0;

  if (!IsConformanceOrVariancePresent(pFormat)) {
    /* null descriptor */
    *pCount = def;
    goto finish_conf;
  }

  switch (pFormat[0] & 0xf0) {
  case RPC_FC_NORMAL_CONFORMANCE:
    TRACE("normal conformance, ofs=%d\n", ofs);
    ptr = pMemory;
    break;
  case RPC_FC_POINTER_CONFORMANCE:
    TRACE("pointer conformance, ofs=%d\n", ofs);
    ptr = pStubMsg->Memory;
    break;
  case RPC_FC_TOP_LEVEL_CONFORMANCE:
    TRACE("toplevel conformance, ofs=%d\n", ofs);
    if (pStubMsg->StackTop) {
      ptr = pStubMsg->StackTop;
    }
    else {
      /* -Os mode, *pCount is already set */
      goto finish_conf;
    }
    break;
  case RPC_FC_CONSTANT_CONFORMANCE:
    data = ofs | ((DWORD)pFormat[1] << 16);
    TRACE("constant conformance, val=%ld\n", data);
    *pCount = data;
    goto finish_conf;
  case RPC_FC_TOP_LEVEL_MULTID_CONFORMANCE:
    FIXME("toplevel multidimensional conformance, ofs=%d\n", ofs);
    if (pStubMsg->StackTop) {
      ptr = pStubMsg->StackTop;
    }
    else {
      /* ? */
      goto done_conf_grab;
    }
    break;
  default:
    FIXME("unknown conformance type %x\n", pFormat[0] & 0xf0);
  }

  switch (pFormat[1]) {
  case RPC_FC_DEREFERENCE:
    ptr = *(LPVOID*)((char *)ptr + ofs);
    break;
  case RPC_FC_CALLBACK:
  {
    unsigned char *old_stack_top = pStubMsg->StackTop;
    pStubMsg->StackTop = ptr;

    /* ofs is index into StubDesc->apfnExprEval */
    TRACE("callback conformance into apfnExprEval[%d]\n", ofs);
    pStubMsg->StubDesc->apfnExprEval[ofs](pStubMsg);

    pStubMsg->StackTop = old_stack_top;
    goto finish_conf;
  }
  default:
    ptr = (char *)ptr + ofs;
    break;
  }

  switch (dtype) {
  case RPC_FC_LONG:
  case RPC_FC_ULONG:
    data = *(DWORD*)ptr;
    break;
  case RPC_FC_SHORT:
    data = *(SHORT*)ptr;
    break;
  case RPC_FC_USHORT:
    data = *(USHORT*)ptr;
    break;
  case RPC_FC_SMALL:
    data = *(CHAR*)ptr;
    break;
  case RPC_FC_USMALL:
    data = *(UCHAR*)ptr;
    break;
  default:
    FIXME("unknown conformance data type %x\n", dtype);
    goto done_conf_grab;
  }
  TRACE("dereferenced data type %x at %p, got %ld\n", dtype, ptr, data);

done_conf_grab:
  switch (pFormat[1]) {
  case 0: /* no op */
    *pCount = data;
    break;
  case RPC_FC_DEREFERENCE:
    /* already handled */
    break;
  default:
    FIXME("unknown conformance op %d\n", pFormat[1]);
    goto finish_conf;
  }

finish_conf:
  TRACE("resulting conformance is %ld\n", *pCount);
  if (pStubMsg->fHasNewCorrDesc)
    return pFormat+6;
  else
    return pFormat+4;
}


/*
 * NdrConformantString:
 * 
 * What MS calls a ConformantString is, in DCE terminology,
 * a Varying-Conformant String.
 * [
 *   maxlen: DWORD (max # of CHARTYPE characters, inclusive of '\0')
 *   offset: DWORD (actual string data begins at (offset) CHARTYPE's
 *           into unmarshalled string) 
 *   length: DWORD (# of CHARTYPE characters, inclusive of '\0')
 *   [ 
 *     data: CHARTYPE[maxlen]
 *   ] 
 * ], where CHARTYPE is the appropriate character type (specified externally)
 *
 */

/***********************************************************************
 *            NdrConformantStringMarshall [RPCRT4.@]
 */
unsigned char *WINAPI NdrConformantStringMarshall(MIDL_STUB_MESSAGE *pStubMsg,
  unsigned char *pszMessage, PFORMAT_STRING pFormat)
{ 
  unsigned long len, esize;

  TRACE("(pStubMsg == ^%p, pszMessage == ^%p, pFormat == ^%p)\n", pStubMsg, pszMessage, pFormat);
  
  assert(pFormat);
  if (*pFormat == RPC_FC_C_CSTRING) {
    TRACE("string=%s\n", debugstr_a((char*)pszMessage));
    len = strlen((char*)pszMessage)+1;
    esize = 1;
  }
  else if (*pFormat == RPC_FC_C_WSTRING) {
    TRACE("string=%s\n", debugstr_w((LPWSTR)pszMessage));
    len = strlenW((LPWSTR)pszMessage)+1;
    esize = 2;
  }
  else {
    ERR("Unhandled string type: %#x\n", *pFormat); 
    /* FIXME: raise an exception. */
    return NULL;
  }

  if (pFormat[1] != RPC_FC_PAD) {
    FIXME("sized string format=%d\n", pFormat[1]);
  }

  assert( (pStubMsg->BufferLength >= (len*esize + 13)) && (pStubMsg->Buffer != NULL) );

  pStubMsg->MaxCount = pStubMsg->ActualCount = len;
  pStubMsg->Offset = 0;
  WriteConformance(pStubMsg);
  WriteVariance(pStubMsg);

  memcpy(pStubMsg->Buffer, pszMessage, len*esize); /* the string itself */
  pStubMsg->Buffer += len*esize;

  STD_OVERFLOW_CHECK(pStubMsg);

  /* success */
  return NULL; /* is this always right? */
}

/***********************************************************************
 *           NdrConformantStringBufferSize [RPCRT4.@]
 */
void WINAPI NdrConformantStringBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
  unsigned char* pMemory, PFORMAT_STRING pFormat)
{
  TRACE("(pStubMsg == ^%p, pMemory == ^%p, pFormat == ^%p)\n", pStubMsg, pMemory, pFormat);

  SizeConformance(pStubMsg);
  SizeVariance(pStubMsg);

  assert(pFormat);
  if (*pFormat == RPC_FC_C_CSTRING) {
    /* we need + 1 octet for '\0' */
    TRACE("string=%s\n", debugstr_a((char*)pMemory));
    pStubMsg->BufferLength += strlen((char*)pMemory) + 1 + BUFFER_PARANOIA;
  }
  else if (*pFormat == RPC_FC_C_WSTRING) {
    /* we need + 2 octets for L'\0' */
    TRACE("string=%s\n", debugstr_w((LPWSTR)pMemory));
    pStubMsg->BufferLength += strlenW((LPWSTR)pMemory)*2 + 2 + BUFFER_PARANOIA;
  }
  else {
    ERR("Unhandled string type: %#x\n", *pFormat); 
    /* FIXME: raise an exception */
  }

  if (pFormat[1] != RPC_FC_PAD) {
    FIXME("sized string format=%d\n", pFormat[1]);
  }
}

/************************************************************************
 *            NdrConformantStringMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrConformantStringMemorySize( PMIDL_STUB_MESSAGE pStubMsg,
  PFORMAT_STRING pFormat )
{
  unsigned long rslt = 0;

  TRACE("(pStubMsg == ^%p, pFormat == ^%p)\n", pStubMsg, pFormat);
   
  assert(pStubMsg && pFormat);

  if (*pFormat == RPC_FC_C_CSTRING) {
    rslt = NDR_LOCAL_UINT32_READ(pStubMsg->Buffer); /* maxlen */
  }
  else if (*pFormat == RPC_FC_C_WSTRING) {
    rslt = NDR_LOCAL_UINT32_READ(pStubMsg->Buffer)*2; /* maxlen */
  }
  else {
    ERR("Unhandled string type: %#x\n", *pFormat);
    /* FIXME: raise an exception */
  }

  if (pFormat[1] != RPC_FC_PAD) {
    FIXME("sized string format=%d\n", pFormat[1]);
  }

  TRACE("  --> %lu\n", rslt);
  return rslt;
}

/************************************************************************
 *           NdrConformantStringUnmarshall [RPCRT4.@]
 */
unsigned char *WINAPI NdrConformantStringUnmarshall( PMIDL_STUB_MESSAGE pStubMsg,
  unsigned char** ppMemory, PFORMAT_STRING pFormat, unsigned char fMustAlloc )
{
  unsigned long len, esize;

  TRACE("(pStubMsg == ^%p, *pMemory == ^%p, pFormat == ^%p, fMustAlloc == %u)\n",
    pStubMsg, *ppMemory, pFormat, fMustAlloc);

  assert(pFormat && ppMemory && pStubMsg);

  ReadConformance(pStubMsg, NULL);
  ReadVariance(pStubMsg, NULL);

  if (*pFormat == RPC_FC_C_CSTRING) esize = 1;
  else if (*pFormat == RPC_FC_C_WSTRING) esize = 2;
  else {
    ERR("Unhandled string type: %#x\n", *pFormat);
    /* FIXME: raise an exception */
    esize = 0;
  }

  if (pFormat[1] != RPC_FC_PAD) {
    FIXME("sized string format=%d\n", pFormat[1]);
  }

  len = pStubMsg->ActualCount;

  if (fMustAlloc || !*ppMemory)
    *ppMemory = NdrAllocate(pStubMsg, len*esize + BUFFER_PARANOIA);

  memcpy(*ppMemory, pStubMsg->Buffer, len*esize);

  pStubMsg->Buffer += len*esize;

  if (*pFormat == RPC_FC_C_CSTRING) {
    TRACE("string=%s\n", debugstr_a((char*)*ppMemory));
  }
  else if (*pFormat == RPC_FC_C_WSTRING) {
    TRACE("string=%s\n", debugstr_w((LPWSTR)*ppMemory));
  }

  return NULL; /* FIXME: is this always right? */
}

/***********************************************************************
 *           NdrNonConformantStringMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrNonConformantStringMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrNonConformantStringUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrNonConformantStringUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrNonConformantStringBufferSize [RPCRT4.@]
 */
void WINAPI NdrNonConformantStringBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrNonConformantStringMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrNonConformantStringMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

static inline void dump_pointer_attr(unsigned char attr)
{
    if (attr & RPC_FC_P_ALLOCALLNODES)
        TRACE(" RPC_FC_P_ALLOCALLNODES");
    if (attr & RPC_FC_P_DONTFREE)
        TRACE(" RPC_FC_P_DONTFREE");
    if (attr & RPC_FC_P_ONSTACK)
        TRACE(" RPC_FC_P_ONSTACK");
    if (attr & RPC_FC_P_SIMPLEPOINTER)
        TRACE(" RPC_FC_P_SIMPLEPOINTER");
    if (attr & RPC_FC_P_DEREF)
        TRACE(" RPC_FC_P_DEREF");
    TRACE("\n");
}

/***********************************************************************
 *           PointerMarshall
 */
void WINAPI PointerMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                            unsigned char *Buffer,
                            unsigned char *Pointer,
                            PFORMAT_STRING pFormat)
{
  unsigned type = pFormat[0], attr = pFormat[1];
  PFORMAT_STRING desc;
  NDR_MARSHALL m;

  TRACE("(%p,%p,%p,%p)\n", pStubMsg, Buffer, Pointer, pFormat);
  TRACE("type=0x%x, attr=", type); dump_pointer_attr(attr);
  pFormat += 2;
  if (attr & RPC_FC_P_SIMPLEPOINTER) desc = pFormat;
  else desc = pFormat + *(const SHORT*)pFormat;

  switch (type) {
  case RPC_FC_RP: /* ref pointer (always non-null) */
#if 0 /* this causes problems for InstallShield so is disabled - we need more tests */
    if (!Pointer)
      RpcRaiseException(RPC_X_NULL_REF_POINTER);
#endif
    break;
  case RPC_FC_UP: /* unique pointer */
  case RPC_FC_OP: /* object pointer - same as unique here */
    TRACE("writing %p to buffer\n", Pointer);
    NDR_LOCAL_UINT32_WRITE(Buffer, (unsigned long)Pointer);
    break;
  case RPC_FC_FP:
  default:
    FIXME("unhandled ptr type=%02x\n", type);
    RpcRaiseException(RPC_X_BAD_STUB_DATA);
  }

  TRACE("calling marshaller for type 0x%x\n", (int)*desc);

  if (Pointer) {
    if (attr & RPC_FC_P_DEREF) {
      Pointer = *(unsigned char**)Pointer;
      TRACE("deref => %p\n", Pointer);
    }
    m = NdrMarshaller[*desc & NDR_TABLE_MASK];
    if (m) m(pStubMsg, Pointer, desc);
    else FIXME("no marshaller for data type=%02x\n", *desc);
  }

  STD_OVERFLOW_CHECK(pStubMsg);
}

/***********************************************************************
 *           PointerUnmarshall
 */
void WINAPI PointerUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                              unsigned char *Buffer,
                              unsigned char **pPointer,
                              PFORMAT_STRING pFormat,
                              unsigned char fMustAlloc)
{
  unsigned type = pFormat[0], attr = pFormat[1];
  PFORMAT_STRING desc;
  NDR_UNMARSHALL m;
  DWORD pointer_id = 0;

  TRACE("(%p,%p,%p,%p,%d)\n", pStubMsg, Buffer, pPointer, pFormat, fMustAlloc);
  TRACE("type=0x%x, attr=", type); dump_pointer_attr(attr);
  pFormat += 2;
  if (attr & RPC_FC_P_SIMPLEPOINTER) desc = pFormat;
  else desc = pFormat + *(const SHORT*)pFormat;

  switch (type) {
  case RPC_FC_RP: /* ref pointer (always non-null) */
    pointer_id = ~0UL;
    break;
  case RPC_FC_UP: /* unique pointer */
    pointer_id = NDR_LOCAL_UINT32_READ(Buffer);
    TRACE("pointer_id is 0x%08lx\n", pointer_id);
    break;
  case RPC_FC_OP: /* object pointer - we must free data before overwriting it */
    pointer_id = NDR_LOCAL_UINT32_READ(Buffer);
    TRACE("pointer_id is 0x%08lx\n", pointer_id);
    if (*pPointer)
        FIXME("free object pointer %p\n", *pPointer);
    break;
  case RPC_FC_FP:
  default:
    FIXME("unhandled ptr type=%02x\n", type);
    RpcRaiseException(RPC_X_BAD_STUB_DATA);
  }

  if (pointer_id) {
    if (attr & RPC_FC_P_DEREF) {
      if (!*pPointer || fMustAlloc)
        *pPointer = NdrAllocate(pStubMsg, sizeof(void *));
      pPointer = *(unsigned char***)pPointer;
      TRACE("deref => %p\n", pPointer);
    }
    m = NdrUnmarshaller[*desc & NDR_TABLE_MASK];
    if (m) m(pStubMsg, pPointer, desc, fMustAlloc);
    else FIXME("no unmarshaller for data type=%02x\n", *desc);
  }

  TRACE("pointer=%p\n", *pPointer);
}

/***********************************************************************
 *           PointerBufferSize
 */
void WINAPI PointerBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                              unsigned char *Pointer,
                              PFORMAT_STRING pFormat)
{
  unsigned type = pFormat[0], attr = pFormat[1];
  PFORMAT_STRING desc;
  NDR_BUFFERSIZE m;

  TRACE("(%p,%p,%p)\n", pStubMsg, Pointer, pFormat);
  TRACE("type=0x%x, attr=", type); dump_pointer_attr(attr);
  pFormat += 2;
  if (attr & RPC_FC_P_SIMPLEPOINTER) desc = pFormat;
  else desc = pFormat + *(const SHORT*)pFormat;

  switch (type) {
  case RPC_FC_RP: /* ref pointer (always non-null) */
    break;
  case RPC_FC_OP:
  case RPC_FC_UP:
    /* NULL pointer has no further representation */
    if (!Pointer)
        return;
    break;
  case RPC_FC_FP:
  default:
    FIXME("unhandled ptr type=%02x\n", type);
    RpcRaiseException(RPC_X_BAD_STUB_DATA);
  }

  if (attr & RPC_FC_P_DEREF) {
    Pointer = *(unsigned char**)Pointer;
    TRACE("deref => %p\n", Pointer);
  }

  m = NdrBufferSizer[*desc & NDR_TABLE_MASK];
  if (m) m(pStubMsg, Pointer, desc);
  else FIXME("no buffersizer for data type=%02x\n", *desc);
}

/***********************************************************************
 *           PointerMemorySize [RPCRT4.@]
 */
unsigned long WINAPI PointerMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                       unsigned char *Buffer,
                                       PFORMAT_STRING pFormat)
{
  unsigned type = pFormat[0], attr = pFormat[1];
  PFORMAT_STRING desc;
  NDR_MEMORYSIZE m;

  FIXME("(%p,%p,%p): stub\n", pStubMsg, Buffer, pFormat);
  TRACE("type=0x%x, attr=", type); dump_pointer_attr(attr);
  pFormat += 2;
  if (attr & RPC_FC_P_SIMPLEPOINTER) desc = pFormat;
  else desc = pFormat + *(const SHORT*)pFormat;

  switch (type) {
  case RPC_FC_RP: /* ref pointer (always non-null) */
    break;
  default:
    FIXME("unhandled ptr type=%02x\n", type);
    RpcRaiseException(RPC_X_BAD_STUB_DATA);
  }

  if (attr & RPC_FC_P_DEREF) {
    TRACE("deref\n");
  }

  m = NdrMemorySizer[*desc & NDR_TABLE_MASK];
  if (m) m(pStubMsg, desc);
  else FIXME("no memorysizer for data type=%02x\n", *desc);

  return 0;
}

/***********************************************************************
 *           PointerFree [RPCRT4.@]
 */
void WINAPI PointerFree(PMIDL_STUB_MESSAGE pStubMsg,
                        unsigned char *Pointer,
                        PFORMAT_STRING pFormat)
{
  unsigned type = pFormat[0], attr = pFormat[1];
  PFORMAT_STRING desc;
  NDR_FREE m;

  TRACE("(%p,%p,%p)\n", pStubMsg, Pointer, pFormat);
  TRACE("type=0x%x, attr=", type); dump_pointer_attr(attr);
  if (attr & RPC_FC_P_DONTFREE) return;
  pFormat += 2;
  if (attr & RPC_FC_P_SIMPLEPOINTER) desc = pFormat;
  else desc = pFormat + *(const SHORT*)pFormat;

  if (!Pointer) return;

  if (attr & RPC_FC_P_DEREF) {
    Pointer = *(unsigned char**)Pointer;
    TRACE("deref => %p\n", Pointer);
  }

  m = NdrFreer[*desc & NDR_TABLE_MASK];
  if (m) m(pStubMsg, Pointer, desc);

  /* hmm... is this sensible?
   * perhaps we should check if the memory comes from NdrAllocate,
   * and deallocate only if so - checking if the pointer is between
   * BufferStart and BufferEnd is probably no good since the buffer
   * may be reallocated when the server wants to marshal the reply */
  switch (*desc) {
  case RPC_FC_BOGUS_STRUCT:
  case RPC_FC_BOGUS_ARRAY:
  case RPC_FC_USER_MARSHAL:
  case RPC_FC_CARRAY:
  case RPC_FC_CVARRAY:
    break;
  default:
    FIXME("unhandled data type=%02x\n", *desc);
    break;
  case RPC_FC_C_CSTRING:
  case RPC_FC_C_WSTRING:
    if (pStubMsg->ReuseBuffer) goto notfree;
    break;
  case RPC_FC_IP:
    goto notfree;
  }

  if (attr & RPC_FC_P_ONSTACK) {
    TRACE("not freeing stack ptr %p\n", Pointer);
    return;
  }
  TRACE("freeing %p\n", Pointer);
  NdrFree(pStubMsg, Pointer);
  return;
notfree:
  TRACE("not freeing %p\n", Pointer);
}

/***********************************************************************
 *           EmbeddedPointerMarshall
 */
unsigned char * WINAPI EmbeddedPointerMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                               unsigned char *pMemory,
                                               PFORMAT_STRING pFormat)
{
  unsigned char *Mark = pStubMsg->BufferMark;
  unsigned long Offset = pStubMsg->Offset;
  unsigned ofs, rep, count, stride, xofs;
  unsigned i;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  if (*pFormat != RPC_FC_PP) return NULL;
  pFormat += 2;

  while (pFormat[0] != RPC_FC_END) {
    switch (pFormat[0]) {
    default:
      FIXME("unknown repeat type %d\n", pFormat[0]);
    case RPC_FC_NO_REPEAT:
      rep = 1;
      stride = 0;
      ofs = 0;
      count = 1;
      xofs = 0;
      pFormat += 2;
      break;
    case RPC_FC_FIXED_REPEAT:
      rep = *(const WORD*)&pFormat[2];
      stride = *(const WORD*)&pFormat[4];
      ofs = *(const WORD*)&pFormat[6];
      count = *(const WORD*)&pFormat[8];
      xofs = 0;
      pFormat += 10;
      break;
    case RPC_FC_VARIABLE_REPEAT:
      rep = pStubMsg->MaxCount;
      stride = *(const WORD*)&pFormat[2];
      ofs = *(const WORD*)&pFormat[4];
      count = *(const WORD*)&pFormat[6];
      xofs = (pFormat[1] == RPC_FC_VARIABLE_OFFSET) ? Offset * stride : 0;
      pFormat += 8;
      break;
    }
    for (i = 0; i < rep; i++) {
      PFORMAT_STRING info = pFormat;
      unsigned char *membase = pMemory + (i * stride);
      unsigned char *bufbase = Mark + (i * stride);
      unsigned u;
      /* ofs doesn't seem to matter in this context */
      for (u=0; u<count; u++,info+=8) {
        unsigned char *memptr = membase + *(const SHORT*)&info[0];
        unsigned char *bufptr = bufbase + *(const SHORT*)&info[2];
        PointerMarshall(pStubMsg, bufptr, *(unsigned char**)memptr, info+4);
      }
    }
    pFormat += 8 * count;
  }

  STD_OVERFLOW_CHECK(pStubMsg);

  return NULL;
}

/***********************************************************************
 *           EmbeddedPointerUnmarshall
 */
unsigned char * WINAPI EmbeddedPointerUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                 unsigned char **ppMemory,
                                                 PFORMAT_STRING pFormat,
                                                 unsigned char fMustAlloc)
{
  unsigned char *Mark = pStubMsg->BufferMark;
  unsigned long Offset = pStubMsg->Offset;
  unsigned ofs, rep, count, stride, xofs;
  unsigned i;

  TRACE("(%p,%p,%p,%d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);

  if (*pFormat != RPC_FC_PP) return NULL;
  pFormat += 2;

  while (pFormat[0] != RPC_FC_END) {
    TRACE("pFormat[0] = 0x%x\n", pFormat[0]);
    switch (pFormat[0]) {
    default:
      FIXME("unknown repeat type %d\n", pFormat[0]);
    case RPC_FC_NO_REPEAT:
      rep = 1;
      stride = 0;
      ofs = 0;
      count = 1;
      xofs = 0;
      pFormat += 2;
      break;
    case RPC_FC_FIXED_REPEAT:
      rep = *(const WORD*)&pFormat[2];
      stride = *(const WORD*)&pFormat[4];
      ofs = *(const WORD*)&pFormat[6];
      count = *(const WORD*)&pFormat[8];
      xofs = 0;
      pFormat += 10;
      break;
    case RPC_FC_VARIABLE_REPEAT:
      rep = pStubMsg->MaxCount;
      stride = *(const WORD*)&pFormat[2];
      ofs = *(const WORD*)&pFormat[4];
      count = *(const WORD*)&pFormat[6];
      xofs = (pFormat[1] == RPC_FC_VARIABLE_OFFSET) ? Offset * stride : 0;
      pFormat += 8;
      break;
    }
    /* ofs doesn't seem to matter in this context */
    for (i = 0; i < rep; i++) {
      PFORMAT_STRING info = pFormat;
      unsigned char *membase = *ppMemory + (i * stride);
      unsigned char *bufbase = Mark + (i * stride);
      unsigned u;
      for (u=0; u<count; u++,info+=8) {
        unsigned char *memptr = membase + *(const SHORT*)&info[0];
        unsigned char *bufptr = bufbase + *(const SHORT*)&info[2];
        *(void **)memptr = NULL;
        PointerUnmarshall(pStubMsg, bufptr, (unsigned char**)memptr, info+4, fMustAlloc);
      }
    }
    pFormat += 8 * count;
  }

  return NULL;
}

/***********************************************************************
 *           EmbeddedPointerBufferSize
 */
void WINAPI EmbeddedPointerBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                      unsigned char *pMemory,
                                      PFORMAT_STRING pFormat)
{
  unsigned long Offset = pStubMsg->Offset;
  unsigned ofs, rep, count, stride, xofs;
  unsigned i;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  if (pStubMsg->IgnoreEmbeddedPointers) return;

  if (*pFormat != RPC_FC_PP) return;
  pFormat += 2;

  while (pFormat[0] != RPC_FC_END) {
    switch (pFormat[0]) {
    default:
      FIXME("unknown repeat type %d\n", pFormat[0]);
    case RPC_FC_NO_REPEAT:
      rep = 1;
      stride = 0;
      ofs = 0;
      count = 1;
      xofs = 0;
      pFormat += 2;
      break;
    case RPC_FC_FIXED_REPEAT:
      rep = *(const WORD*)&pFormat[2];
      stride = *(const WORD*)&pFormat[4];
      ofs = *(const WORD*)&pFormat[6];
      count = *(const WORD*)&pFormat[8];
      xofs = 0;
      pFormat += 10;
      break;
    case RPC_FC_VARIABLE_REPEAT:
      rep = pStubMsg->MaxCount;
      stride = *(const WORD*)&pFormat[2];
      ofs = *(const WORD*)&pFormat[4];
      count = *(const WORD*)&pFormat[6];
      xofs = (pFormat[1] == RPC_FC_VARIABLE_OFFSET) ? Offset * stride : 0;
      pFormat += 8;
      break;
    }
    /* ofs doesn't seem to matter in this context */
    for (i = 0; i < rep; i++) {
      PFORMAT_STRING info = pFormat;
      unsigned char *membase = pMemory + (i * stride);
      unsigned u;
      for (u=0; u<count; u++,info+=8) {
        unsigned char *memptr = membase + *(const SHORT*)&info[0];
        PointerBufferSize(pStubMsg, *(unsigned char**)memptr, info+4);
      }
    }
    pFormat += 8 * count;
  }
}

/***********************************************************************
 *           EmbeddedPointerMemorySize
 */
unsigned long WINAPI EmbeddedPointerMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                               PFORMAT_STRING pFormat)
{
  unsigned long Offset = pStubMsg->Offset;
  unsigned char *Mark = pStubMsg->BufferMark;
  unsigned ofs, rep, count, stride, xofs;
  unsigned i;

  FIXME("(%p,%p): stub\n", pStubMsg, pFormat);

  if (*pFormat != RPC_FC_PP) return 0;
  pFormat += 2;

  while (pFormat[0] != RPC_FC_END) {
    switch (pFormat[0]) {
    default:
      FIXME("unknown repeat type %d\n", pFormat[0]);
    case RPC_FC_NO_REPEAT:
      rep = 1;
      stride = 0;
      ofs = 0;
      count = 1;
      xofs = 0;
      pFormat += 2;
      break;
    case RPC_FC_FIXED_REPEAT:
      rep = *(const WORD*)&pFormat[2];
      stride = *(const WORD*)&pFormat[4];
      ofs = *(const WORD*)&pFormat[6];
      count = *(const WORD*)&pFormat[8];
      xofs = 0;
      pFormat += 10;
      break;
    case RPC_FC_VARIABLE_REPEAT:
      rep = pStubMsg->MaxCount;
      stride = *(const WORD*)&pFormat[2];
      ofs = *(const WORD*)&pFormat[4];
      count = *(const WORD*)&pFormat[6];
      xofs = (pFormat[1] == RPC_FC_VARIABLE_OFFSET) ? Offset * stride : 0;
      pFormat += 8;
      break;
    }
    /* ofs doesn't seem to matter in this context */
    for (i = 0; i < rep; i++) {
      PFORMAT_STRING info = pFormat;
      unsigned char *bufbase = Mark + (i * stride);
      unsigned u;
      for (u=0; u<count; u++,info+=8) {
        unsigned char *bufptr = bufbase + *(const SHORT*)&info[2];
        PointerMemorySize(pStubMsg, bufptr, info+4);
      }
    }
    pFormat += 8 * count;
  }

  return 0;
}

/***********************************************************************
 *           EmbeddedPointerFree
 */
void WINAPI EmbeddedPointerFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
  unsigned long Offset = pStubMsg->Offset;
  unsigned ofs, rep, count, stride, xofs;
  unsigned i;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  if (*pFormat != RPC_FC_PP) return;
  pFormat += 2;

  while (pFormat[0] != RPC_FC_END) {
    switch (pFormat[0]) {
    default:
      FIXME("unknown repeat type %d\n", pFormat[0]);
    case RPC_FC_NO_REPEAT:
      rep = 1;
      stride = 0;
      ofs = 0;
      count = 1;
      xofs = 0;
      pFormat += 2;
      break;
    case RPC_FC_FIXED_REPEAT:
      rep = *(const WORD*)&pFormat[2];
      stride = *(const WORD*)&pFormat[4];
      ofs = *(const WORD*)&pFormat[6];
      count = *(const WORD*)&pFormat[8];
      xofs = 0;
      pFormat += 10;
      break;
    case RPC_FC_VARIABLE_REPEAT:
      rep = pStubMsg->MaxCount;
      stride = *(const WORD*)&pFormat[2];
      ofs = *(const WORD*)&pFormat[4];
      count = *(const WORD*)&pFormat[6];
      xofs = (pFormat[1] == RPC_FC_VARIABLE_OFFSET) ? Offset * stride : 0;
      pFormat += 8;
      break;
    }
    /* ofs doesn't seem to matter in this context */
    for (i = 0; i < rep; i++) {
      PFORMAT_STRING info = pFormat;
      unsigned char *membase = pMemory + (i * stride);
      unsigned u;
      for (u=0; u<count; u++,info+=8) {
        unsigned char *memptr = membase + *(const SHORT*)&info[0];
        PointerFree(pStubMsg, *(unsigned char**)memptr, info+4);
      }
    }
    pFormat += 8 * count;
  }
}

/***********************************************************************
 *           NdrPointerMarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrPointerMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                          unsigned char *pMemory,
                                          PFORMAT_STRING pFormat)
{
  unsigned char *Buffer;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  /* incremement the buffer here instead of in PointerMarshall,
   * as that is used by embedded pointers which already handle the incrementing
   * the buffer, and shouldn't write any additional pointer data to the wire */
  if (*pFormat != RPC_FC_RP)
  {
    ALIGN_POINTER(pStubMsg->Buffer, 4);
    Buffer = pStubMsg->Buffer;
    pStubMsg->Buffer += 4;
  }
  else
    Buffer = pStubMsg->Buffer;

  PointerMarshall(pStubMsg, Buffer, pMemory, pFormat);

  STD_OVERFLOW_CHECK(pStubMsg);

  return NULL;
}

/***********************************************************************
 *           NdrPointerUnmarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrPointerUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                            unsigned char **ppMemory,
                                            PFORMAT_STRING pFormat,
                                            unsigned char fMustAlloc)
{
  unsigned char *Buffer;

  TRACE("(%p,%p,%p,%d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);

  /* incremement the buffer here instead of in PointerUnmarshall,
   * as that is used by embedded pointers which already handle the incrementing
   * the buffer, and shouldn't read any additional pointer data from the
   * buffer */
  if (*pFormat != RPC_FC_RP)
  {
    ALIGN_POINTER(pStubMsg->Buffer, 4);
    Buffer = pStubMsg->Buffer;
    pStubMsg->Buffer += 4;
  }
  else
    Buffer = pStubMsg->Buffer;

  PointerUnmarshall(pStubMsg, Buffer, ppMemory, pFormat, fMustAlloc);

  return NULL;
}

/***********************************************************************
 *           NdrPointerBufferSize [RPCRT4.@]
 */
void WINAPI NdrPointerBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                      unsigned char *pMemory,
                                      PFORMAT_STRING pFormat)
{
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  /* incremement the buffer length here instead of in PointerBufferSize,
   * as that is used by embedded pointers which already handle the buffer
   * length, and shouldn't write anything more to the wire */
  if (*pFormat != RPC_FC_RP)
  {
    ALIGN_LENGTH(pStubMsg->BufferLength, 4);
    pStubMsg->BufferLength += 4;
  }

  PointerBufferSize(pStubMsg, pMemory, pFormat);
}

/***********************************************************************
 *           NdrPointerMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrPointerMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                          PFORMAT_STRING pFormat)
{
  /* unsigned size = *(LPWORD)(pFormat+2); */
  FIXME("(%p,%p): stub\n", pStubMsg, pFormat);
  PointerMemorySize(pStubMsg, pStubMsg->Buffer, pFormat);
  return 0;
}

/***********************************************************************
 *           NdrPointerFree [RPCRT4.@]
 */
void WINAPI NdrPointerFree(PMIDL_STUB_MESSAGE pStubMsg,
                           unsigned char *pMemory,
                           PFORMAT_STRING pFormat)
{
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  PointerFree(pStubMsg, pMemory, pFormat);
}

/***********************************************************************
 *           NdrSimpleStructMarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrSimpleStructMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                               unsigned char *pMemory,
                                               PFORMAT_STRING pFormat)
{
  unsigned size = *(const WORD*)(pFormat+2);
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  ALIGN_POINTER(pStubMsg->Buffer, pFormat[1] + 1);

  memcpy(pStubMsg->Buffer, pMemory, size);
  pStubMsg->BufferMark = pStubMsg->Buffer;
  pStubMsg->Buffer += size;

  if (pFormat[0] != RPC_FC_STRUCT)
    EmbeddedPointerMarshall(pStubMsg, pMemory, pFormat+4);

  STD_OVERFLOW_CHECK(pStubMsg);

  return NULL;
}

/***********************************************************************
 *           NdrSimpleStructUnmarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrSimpleStructUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                 unsigned char **ppMemory,
                                                 PFORMAT_STRING pFormat,
                                                 unsigned char fMustAlloc)
{
  unsigned size = *(const WORD*)(pFormat+2);
  TRACE("(%p,%p,%p,%d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);

  ALIGN_POINTER(pStubMsg->Buffer, pFormat[1] + 1);

  if (fMustAlloc) {
    *ppMemory = NdrAllocate(pStubMsg, size);
    memcpy(*ppMemory, pStubMsg->Buffer, size);
  } else {
    if (pStubMsg->ReuseBuffer && !*ppMemory)
      /* for servers, we may just point straight into the RPC buffer, I think
       * (I guess that's what MS does since MIDL code doesn't try to free) */
      *ppMemory = pStubMsg->Buffer;
    else
      /* for clients, memory should be provided by caller */
      memcpy(*ppMemory, pStubMsg->Buffer, size);
  }

  pStubMsg->BufferMark = pStubMsg->Buffer;
  pStubMsg->Buffer += size;

  if (pFormat[0] != RPC_FC_STRUCT)
    EmbeddedPointerUnmarshall(pStubMsg, ppMemory, pFormat+4, fMustAlloc);

  return NULL;
}


/***********************************************************************
 *           NdrSimpleStructUnmarshall [RPCRT4.@]
 */
void WINAPI NdrSimpleTypeMarshall( PMIDL_STUB_MESSAGE pStubMsg, unsigned char* pMemory,
                                   unsigned char FormatChar )
{
    FIXME("stub\n");
}


/***********************************************************************
 *           NdrSimpleStructUnmarshall [RPCRT4.@]
 */
void WINAPI NdrSimpleTypeUnmarshall( PMIDL_STUB_MESSAGE pStubMsg, unsigned char* pMemory,
                                     unsigned char FormatChar )
{
    FIXME("stub\n");
}


/***********************************************************************
 *           NdrSimpleStructBufferSize [RPCRT4.@]
 */
void WINAPI NdrSimpleStructBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                      unsigned char *pMemory,
                                      PFORMAT_STRING pFormat)
{
  unsigned size = *(const WORD*)(pFormat+2);
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  ALIGN_LENGTH(pStubMsg->BufferLength, pFormat[1] + 1);

  pStubMsg->BufferLength += size;
  if (pFormat[0] != RPC_FC_STRUCT)
    EmbeddedPointerBufferSize(pStubMsg, pMemory, pFormat+4);
}

/***********************************************************************
 *           NdrSimpleStructMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrSimpleStructMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                               PFORMAT_STRING pFormat)
{
  /* unsigned size = *(LPWORD)(pFormat+2); */
  FIXME("(%p,%p): stub\n", pStubMsg, pFormat);
  if (pFormat[0] != RPC_FC_STRUCT)
    EmbeddedPointerMemorySize(pStubMsg, pFormat+4);
  return 0;
}

/***********************************************************************
 *           NdrSimpleStructFree [RPCRT4.@]
 */
void WINAPI NdrSimpleStructFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  if (pFormat[0] != RPC_FC_STRUCT)
    EmbeddedPointerFree(pStubMsg, pMemory, pFormat+4);
}


unsigned long WINAPI EmbeddedComplexSize(PMIDL_STUB_MESSAGE pStubMsg,
                                         PFORMAT_STRING pFormat)
{
  switch (*pFormat) {
  case RPC_FC_STRUCT:
  case RPC_FC_PSTRUCT:
  case RPC_FC_CSTRUCT:
  case RPC_FC_BOGUS_STRUCT:
    return *(const WORD*)&pFormat[2];
  case RPC_FC_USER_MARSHAL:
    return *(const WORD*)&pFormat[4];
  case RPC_FC_NON_ENCAPSULATED_UNION:
    pFormat += 2;
    if (pStubMsg->fHasNewCorrDesc)
        pFormat += 6;
    else
        pFormat += 4;

    pFormat += *(const SHORT*)pFormat;
    return *(const SHORT*)pFormat;
  default:
    FIXME("unhandled embedded type %02x\n", *pFormat);
  }
  return 0;
}


unsigned long WINAPI EmbeddedComplexMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                               PFORMAT_STRING pFormat)
{
  NDR_MEMORYSIZE m = NdrMemorySizer[*pFormat & NDR_TABLE_MASK];

  if (!m)
  {
    FIXME("no memorysizer for data type=%02x\n", *pFormat);
    return 0;
  }

  return m(pStubMsg, pFormat);
}


unsigned char * WINAPI ComplexMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                       unsigned char *pMemory,
                                       PFORMAT_STRING pFormat,
                                       PFORMAT_STRING pPointer)
{
  PFORMAT_STRING desc;
  NDR_MARSHALL m;
  unsigned long size;

  while (*pFormat != RPC_FC_END) {
    switch (*pFormat) {
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
      TRACE("short=%d <= %p\n", *(WORD*)pMemory, pMemory);
      memcpy(pStubMsg->Buffer, pMemory, 2);
      pStubMsg->Buffer += 2;
      pMemory += 2;
      break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
    case RPC_FC_ENUM32:
      TRACE("long=%ld <= %p\n", *(DWORD*)pMemory, pMemory);
      memcpy(pStubMsg->Buffer, pMemory, 4);
      pStubMsg->Buffer += 4;
      pMemory += 4;
      break;
    case RPC_FC_POINTER:
      TRACE("pointer=%p <= %p\n", *(unsigned char**)pMemory, pMemory);
      NdrPointerMarshall(pStubMsg, *(unsigned char**)pMemory, pPointer);
      pPointer += 4;
      pMemory += 4;
      break;
    case RPC_FC_ALIGNM4:
      ALIGN_POINTER(pMemory, 4);
      break;
    case RPC_FC_ALIGNM8:
      ALIGN_POINTER(pMemory, 8);
      break;
    case RPC_FC_STRUCTPAD2:
      pMemory += 2;
      break;
    case RPC_FC_EMBEDDED_COMPLEX:
      pMemory += pFormat[1];
      pFormat += 2;
      desc = pFormat + *(const SHORT*)pFormat;
      size = EmbeddedComplexSize(pStubMsg, desc);
      TRACE("embedded complex (size=%ld) <= %p\n", size, pMemory);
      m = NdrMarshaller[*desc & NDR_TABLE_MASK];
      if (m) m(pStubMsg, pMemory, desc);
      else FIXME("no marshaller for embedded type %02x\n", *desc);
      pMemory += size;
      pFormat += 2;
      continue;
    case RPC_FC_PAD:
      break;
    default:
      FIXME("unhandled format %02x\n", *pFormat);
    }
    pFormat++;
  }

  return pMemory;
}

unsigned char * WINAPI ComplexUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                         unsigned char *pMemory,
                                         PFORMAT_STRING pFormat,
                                         PFORMAT_STRING pPointer,
                                         unsigned char fMustAlloc)
{
  PFORMAT_STRING desc;
  NDR_UNMARSHALL m;
  unsigned long size;

  while (*pFormat != RPC_FC_END) {
    switch (*pFormat) {
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
      memcpy(pMemory, pStubMsg->Buffer, 2);
      TRACE("short=%d => %p\n", *(WORD*)pMemory, pMemory);
      pStubMsg->Buffer += 2;
      pMemory += 2;
      break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
    case RPC_FC_ENUM32:
      memcpy(pMemory, pStubMsg->Buffer, 4);
      TRACE("long=%ld => %p\n", *(DWORD*)pMemory, pMemory);
      pStubMsg->Buffer += 4;
      pMemory += 4;
      break;
    case RPC_FC_POINTER:
      *(unsigned char**)pMemory = NULL;
      TRACE("pointer => %p\n", pMemory);
      NdrPointerUnmarshall(pStubMsg, (unsigned char**)pMemory, pPointer, fMustAlloc);
      pPointer += 4;
      pMemory += 4;
      break;
    case RPC_FC_ALIGNM4:
      ALIGN_POINTER(pMemory, 4);
      break;
    case RPC_FC_ALIGNM8:
      ALIGN_POINTER(pMemory, 8);
      break;
    case RPC_FC_STRUCTPAD2:
      pMemory += 2;
      break;
    case RPC_FC_EMBEDDED_COMPLEX:
      pMemory += pFormat[1];
      pFormat += 2;
      desc = pFormat + *(const SHORT*)pFormat;
      size = EmbeddedComplexSize(pStubMsg, desc);
      TRACE("embedded complex (size=%ld) => %p\n", size, pMemory);
      m = NdrUnmarshaller[*desc & NDR_TABLE_MASK];
      memset(pMemory, 0, size); /* just in case */
      if (m) m(pStubMsg, &pMemory, desc, fMustAlloc);
      else FIXME("no unmarshaller for embedded type %02x\n", *desc);
      pMemory += size;
      pFormat += 2;
      continue;
    case RPC_FC_PAD:
      break;
    default:
      FIXME("unhandled format %d\n", *pFormat);
    }
    pFormat++;
  }

  return pMemory;
}

unsigned char * WINAPI ComplexBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                         unsigned char *pMemory,
                                         PFORMAT_STRING pFormat,
                                         PFORMAT_STRING pPointer)
{
  PFORMAT_STRING desc;
  NDR_BUFFERSIZE m;
  unsigned long size;

  while (*pFormat != RPC_FC_END) {
    switch (*pFormat) {
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
      pStubMsg->BufferLength += 2;
      pMemory += 2;
      break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
    case RPC_FC_ENUM32:
      pStubMsg->BufferLength += 4;
      pMemory += 4;
      break;
    case RPC_FC_POINTER:
      NdrPointerBufferSize(pStubMsg, *(unsigned char**)pMemory, pPointer);
      pPointer += 4;
      pMemory += 4;
      break;
    case RPC_FC_ALIGNM4:
      ALIGN_POINTER(pMemory, 4);
      break;
    case RPC_FC_ALIGNM8:
      ALIGN_POINTER(pMemory, 8);
      break;
    case RPC_FC_STRUCTPAD2:
      pMemory += 2;
      break;
    case RPC_FC_EMBEDDED_COMPLEX:
      pMemory += pFormat[1];
      pFormat += 2;
      desc = pFormat + *(const SHORT*)pFormat;
      size = EmbeddedComplexSize(pStubMsg, desc);
      m = NdrBufferSizer[*desc & NDR_TABLE_MASK];
      if (m) m(pStubMsg, pMemory, desc);
      else FIXME("no buffersizer for embedded type %02x\n", *desc);
      pMemory += size;
      pFormat += 2;
      continue;
    case RPC_FC_PAD:
      break;
    default:
      FIXME("unhandled format %d\n", *pFormat);
    }
    pFormat++;
  }

  return pMemory;
}

unsigned char * WINAPI ComplexFree(PMIDL_STUB_MESSAGE pStubMsg,
                                   unsigned char *pMemory,
                                   PFORMAT_STRING pFormat,
                                   PFORMAT_STRING pPointer)
{
  PFORMAT_STRING desc;
  NDR_FREE m;
  unsigned long size;

  while (*pFormat != RPC_FC_END) {
    switch (*pFormat) {
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
      pMemory += 2;
      break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
    case RPC_FC_ENUM32:
      pMemory += 4;
      break;
    case RPC_FC_POINTER:
      NdrPointerFree(pStubMsg, *(unsigned char**)pMemory, pPointer);
      pPointer += 4;
      pMemory += 4;
      break;
    case RPC_FC_ALIGNM4:
      ALIGN_POINTER(pMemory, 4);
      break;
    case RPC_FC_ALIGNM8:
      ALIGN_POINTER(pMemory, 8);
      break;
    case RPC_FC_STRUCTPAD2:
      pMemory += 2;
      break;
    case RPC_FC_EMBEDDED_COMPLEX:
      pMemory += pFormat[1];
      pFormat += 2;
      desc = pFormat + *(const SHORT*)pFormat;
      size = EmbeddedComplexSize(pStubMsg, desc);
      m = NdrFreer[*desc & NDR_TABLE_MASK];
      if (m) m(pStubMsg, pMemory, desc);
      else FIXME("no freer for embedded type %02x\n", *desc);
      pMemory += size;
      pFormat += 2;
      continue;
    case RPC_FC_PAD:
      break;
    default:
      FIXME("unhandled format %d\n", *pFormat);
    }
    pFormat++;
  }

  return pMemory;
}

unsigned long WINAPI ComplexStructMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                       PFORMAT_STRING pFormat)
{
  PFORMAT_STRING desc;
  unsigned long size = 0;

  while (*pFormat != RPC_FC_END) {
    switch (*pFormat) {
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
      size += 2;
      pStubMsg->Buffer += 2;
      break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
      size += 4;
      pStubMsg->Buffer += 4;
      break;
    case RPC_FC_POINTER:
      size += 4;
      pStubMsg->Buffer += 4;
      break;
    case RPC_FC_ALIGNM4:
      ALIGN_LENGTH(size, 4);
      ALIGN_POINTER(pStubMsg->Buffer, 4);
      break;
    case RPC_FC_ALIGNM8:
      ALIGN_LENGTH(size, 8);
      ALIGN_POINTER(pStubMsg->Buffer, 8);
      break;
    case RPC_FC_STRUCTPAD2:
      size += 2;
      pStubMsg->Buffer += 2;
      break;
    case RPC_FC_EMBEDDED_COMPLEX:
      size += pFormat[1];
      pFormat += 2;
      desc = pFormat + *(const SHORT*)pFormat;
      size += EmbeddedComplexMemorySize(pStubMsg, desc);
      pFormat += 2;
      continue;
    case RPC_FC_PAD:
      break;
    default:
      FIXME("unhandled format %d\n", *pFormat);
    }
    pFormat++;
  }

  return size;
}

/***********************************************************************
 *           NdrComplexStructMarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrComplexStructMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                unsigned char *pMemory,
                                                PFORMAT_STRING pFormat)
{
  PFORMAT_STRING conf_array = NULL;
  PFORMAT_STRING pointer_desc = NULL;
  unsigned char *OldMemory = pStubMsg->Memory;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  ALIGN_POINTER(pStubMsg->Buffer, pFormat[1] + 1);

  pFormat += 4;
  if (*(const WORD*)pFormat) conf_array = pFormat + *(const WORD*)pFormat;
  pFormat += 2;
  if (*(const WORD*)pFormat) pointer_desc = pFormat + *(const WORD*)pFormat;
  pFormat += 2;

  pStubMsg->Memory = pMemory;

  ComplexMarshall(pStubMsg, pMemory, pFormat, pointer_desc);

  if (conf_array)
    NdrConformantArrayMarshall(pStubMsg, pMemory, conf_array);

  pStubMsg->Memory = OldMemory;

  STD_OVERFLOW_CHECK(pStubMsg);

  return NULL;
}

/***********************************************************************
 *           NdrComplexStructUnmarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrComplexStructUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                  unsigned char **ppMemory,
                                                  PFORMAT_STRING pFormat,
                                                  unsigned char fMustAlloc)
{
  unsigned size = *(const WORD*)(pFormat+2);
  PFORMAT_STRING conf_array = NULL;
  PFORMAT_STRING pointer_desc = NULL;
  unsigned char *pMemory;

  TRACE("(%p,%p,%p,%d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);

  ALIGN_POINTER(pStubMsg->Buffer, pFormat[1] + 1);

  if (fMustAlloc || !*ppMemory)
  {
    *ppMemory = NdrAllocate(pStubMsg, size);
    memset(*ppMemory, 0, size);
  }

  pFormat += 4;
  if (*(const WORD*)pFormat) conf_array = pFormat + *(const WORD*)pFormat;
  pFormat += 2;
  if (*(const WORD*)pFormat) pointer_desc = pFormat + *(const WORD*)pFormat;
  pFormat += 2;

  pMemory = ComplexUnmarshall(pStubMsg, *ppMemory, pFormat, pointer_desc, fMustAlloc);

  if (conf_array)
    NdrConformantArrayUnmarshall(pStubMsg, &pMemory, conf_array, fMustAlloc);

  return NULL;
}

/***********************************************************************
 *           NdrComplexStructBufferSize [RPCRT4.@]
 */
void WINAPI NdrComplexStructBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                       unsigned char *pMemory,
                                       PFORMAT_STRING pFormat)
{
  PFORMAT_STRING conf_array = NULL;
  PFORMAT_STRING pointer_desc = NULL;
  unsigned char *OldMemory = pStubMsg->Memory;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  ALIGN_LENGTH(pStubMsg->BufferLength, pFormat[1] + 1);

  pFormat += 4;
  if (*(const WORD*)pFormat) conf_array = pFormat + *(const WORD*)pFormat;
  pFormat += 2;
  if (*(const WORD*)pFormat) pointer_desc = pFormat + *(const WORD*)pFormat;
  pFormat += 2;

  pStubMsg->Memory = pMemory;

  pMemory = ComplexBufferSize(pStubMsg, pMemory, pFormat, pointer_desc);

  if (conf_array)
    NdrConformantArrayBufferSize(pStubMsg, pMemory, conf_array);

  pStubMsg->Memory = OldMemory;
}

/***********************************************************************
 *           NdrComplexStructMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrComplexStructMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                                PFORMAT_STRING pFormat)
{
  unsigned size = *(const WORD*)(pFormat+2);
  PFORMAT_STRING conf_array = NULL;
  PFORMAT_STRING pointer_desc = NULL;

  TRACE("(%p,%p)\n", pStubMsg, pFormat);

  ALIGN_POINTER(pStubMsg->Buffer, pFormat[1] + 1);

  pFormat += 4;
  if (*(const WORD*)pFormat) conf_array = pFormat + *(const WORD*)pFormat;
  pFormat += 2;
  if (*(const WORD*)pFormat) pointer_desc = pFormat + *(const WORD*)pFormat;
  pFormat += 2;

  ComplexStructMemorySize(pStubMsg, pFormat);

  if (conf_array)
    NdrConformantArrayMemorySize(pStubMsg, conf_array);

  return size;
}

/***********************************************************************
 *           NdrComplexStructFree [RPCRT4.@]
 */
void WINAPI NdrComplexStructFree(PMIDL_STUB_MESSAGE pStubMsg,
                                 unsigned char *pMemory,
                                 PFORMAT_STRING pFormat)
{
  PFORMAT_STRING conf_array = NULL;
  PFORMAT_STRING pointer_desc = NULL;
  unsigned char *OldMemory = pStubMsg->Memory;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  pFormat += 4;
  if (*(const WORD*)pFormat) conf_array = pFormat + *(const WORD*)pFormat;
  pFormat += 2;
  if (*(const WORD*)pFormat) pointer_desc = pFormat + *(const WORD*)pFormat;
  pFormat += 2;

  pStubMsg->Memory = pMemory;

  pMemory = ComplexFree(pStubMsg, pMemory, pFormat, pointer_desc);

  if (conf_array)
    NdrConformantArrayFree(pStubMsg, pMemory, conf_array);

  pStubMsg->Memory = OldMemory;
}

/***********************************************************************
 *           NdrConformantArrayMarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrConformantArrayMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                  unsigned char *pMemory,
                                                  PFORMAT_STRING pFormat)
{
  DWORD size = 0, esize = *(const WORD*)(pFormat+2);
  unsigned char alignment = pFormat[1] + 1;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  if (pFormat[0] != RPC_FC_CARRAY) FIXME("format=%d\n", pFormat[0]);

  pFormat = ComputeConformance(pStubMsg, pMemory, pFormat+4, 0);
  size = pStubMsg->MaxCount;

  WriteConformance(pStubMsg);

  ALIGN_POINTER(pStubMsg->Buffer, alignment);

  memcpy(pStubMsg->Buffer, pMemory, size*esize);
  pStubMsg->BufferMark = pStubMsg->Buffer;
  pStubMsg->Buffer += size*esize;

  EmbeddedPointerMarshall(pStubMsg, pMemory, pFormat);

  STD_OVERFLOW_CHECK(pStubMsg);

  return NULL;
}

/***********************************************************************
 *           NdrConformantArrayUnmarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrConformantArrayUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                    unsigned char **ppMemory,
                                                    PFORMAT_STRING pFormat,
                                                    unsigned char fMustAlloc)
{
  DWORD size = 0, esize = *(const WORD*)(pFormat+2);
  unsigned char alignment = pFormat[1] + 1;

  TRACE("(%p,%p,%p,%d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);
  if (pFormat[0] != RPC_FC_CARRAY) FIXME("format=%d\n", pFormat[0]);

  pFormat = ReadConformance(pStubMsg, pFormat+4);
  size = pStubMsg->MaxCount;

  if (fMustAlloc || !*ppMemory)
    *ppMemory = NdrAllocate(pStubMsg, size*esize);

  ALIGN_POINTER(pStubMsg->Buffer, alignment);

  memcpy(*ppMemory, pStubMsg->Buffer, size*esize);

  pStubMsg->BufferMark = pStubMsg->Buffer;
  pStubMsg->Buffer += size*esize;

  EmbeddedPointerUnmarshall(pStubMsg, ppMemory, pFormat, fMustAlloc);

  return NULL;
}

/***********************************************************************
 *           NdrConformantArrayBufferSize [RPCRT4.@]
 */
void WINAPI NdrConformantArrayBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                         unsigned char *pMemory,
                                         PFORMAT_STRING pFormat)
{
  DWORD size = 0, esize = *(const WORD*)(pFormat+2);
  unsigned char alignment = pFormat[1] + 1;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  if (pFormat[0] != RPC_FC_CARRAY) FIXME("format=%d\n", pFormat[0]);

  pFormat = ComputeConformance(pStubMsg, pMemory, pFormat+4, 0);
  size = pStubMsg->MaxCount;

  SizeConformance(pStubMsg);

  ALIGN_LENGTH(pStubMsg->BufferLength, alignment);

  /* conformance value plus array */
  pStubMsg->BufferLength += size*esize;

  EmbeddedPointerBufferSize(pStubMsg, pMemory, pFormat);
}

/***********************************************************************
 *           NdrConformantArrayMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrConformantArrayMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                                  PFORMAT_STRING pFormat)
{
  DWORD size = 0, esize = *(const WORD*)(pFormat+2);
  unsigned char *buffer;

  TRACE("(%p,%p)\n", pStubMsg, pFormat);
  if (pFormat[0] != RPC_FC_CARRAY) FIXME("format=%d\n", pFormat[0]);

  buffer = pStubMsg->Buffer;
  pFormat = ReadConformance(pStubMsg, pFormat+4);
  pStubMsg->Buffer = buffer;
  size = pStubMsg->MaxCount;

  return size*esize;
}

/***********************************************************************
 *           NdrConformantArrayFree [RPCRT4.@]
 */
void WINAPI NdrConformantArrayFree(PMIDL_STUB_MESSAGE pStubMsg,
                                   unsigned char *pMemory,
                                   PFORMAT_STRING pFormat)
{
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  if (pFormat[0] != RPC_FC_CARRAY) FIXME("format=%d\n", pFormat[0]);

  EmbeddedPointerFree(pStubMsg, pMemory, pFormat);
}


/***********************************************************************
 *           NdrConformantVaryingArrayMarshall  [RPCRT4.@]
 */
unsigned char* WINAPI NdrConformantVaryingArrayMarshall( PMIDL_STUB_MESSAGE pStubMsg,
                                                         unsigned char* pMemory,
                                                         PFORMAT_STRING pFormat )
{
    unsigned char alignment = pFormat[1] + 1;
    DWORD esize = *(const WORD*)(pFormat+2);

    TRACE("(%p, %p, %p)\n", pStubMsg, pMemory, pFormat);

    if (pFormat[0] != RPC_FC_CVARRAY)
    {
        ERR("invalid format type %x\n", pFormat[0]);
        RpcRaiseException(RPC_S_INTERNAL_ERROR);
        return NULL;
    }

    pFormat = ComputeConformance(pStubMsg, pMemory, pFormat+4, 0);
    pFormat = ComputeVariance(pStubMsg, pMemory, pFormat, 0);

    WriteConformance(pStubMsg);
    WriteVariance(pStubMsg);

    ALIGN_POINTER(pStubMsg->Buffer, alignment);

    memcpy(pStubMsg->Buffer, pMemory + pStubMsg->Offset, pStubMsg->ActualCount*esize);
    pStubMsg->BufferMark = pStubMsg->Buffer;
    pStubMsg->Buffer += pStubMsg->ActualCount*esize;

    EmbeddedPointerMarshall(pStubMsg, pMemory, pFormat);

    STD_OVERFLOW_CHECK(pStubMsg);

    return NULL;
}


/***********************************************************************
 *           NdrConformantVaryingArrayUnmarshall  [RPCRT4.@]
 */
unsigned char* WINAPI NdrConformantVaryingArrayUnmarshall( PMIDL_STUB_MESSAGE pStubMsg,
                                                           unsigned char** ppMemory,
                                                           PFORMAT_STRING pFormat,
                                                           unsigned char fMustAlloc )
{
    unsigned char alignment = pFormat[1] + 1;
    DWORD esize = *(const WORD*)(pFormat+2);

    TRACE("(%p, %p, %p, %d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);

    if (pFormat[0] != RPC_FC_CVARRAY)
    {
        ERR("invalid format type %x\n", pFormat[0]);
        RpcRaiseException(RPC_S_INTERNAL_ERROR);
        return NULL;
    }

    pFormat = ReadConformance(pStubMsg, pFormat);
    pFormat = ReadVariance(pStubMsg, pFormat);

    ALIGN_POINTER(pStubMsg->Buffer, alignment);

    if (!*ppMemory || fMustAlloc)
        *ppMemory = NdrAllocate(pStubMsg, pStubMsg->MaxCount * esize);
    memcpy(*ppMemory + pStubMsg->Offset, pStubMsg->Buffer, pStubMsg->ActualCount * esize);
    pStubMsg->Buffer += pStubMsg->ActualCount * esize;

    EmbeddedPointerUnmarshall(pStubMsg, ppMemory, pFormat, fMustAlloc);

    return NULL;
}


/***********************************************************************
 *           NdrConformantVaryingArrayFree  [RPCRT4.@]
 */
void WINAPI NdrConformantVaryingArrayFree( PMIDL_STUB_MESSAGE pStubMsg,
                                           unsigned char* pMemory,
                                           PFORMAT_STRING pFormat )
{
    TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

    if (pFormat[0] != RPC_FC_CVARRAY)
    {
        ERR("invalid format type %x\n", pFormat[0]);
        RpcRaiseException(RPC_S_INTERNAL_ERROR);
        return;
    }

    pFormat = ComputeConformance(pStubMsg, pMemory, pFormat+4, 0);
    pFormat = ComputeVariance(pStubMsg, pMemory, pFormat, 0);

    EmbeddedPointerFree(pStubMsg, pMemory, pFormat);
}


/***********************************************************************
 *           NdrConformantVaryingArrayBufferSize  [RPCRT4.@]
 */
void WINAPI NdrConformantVaryingArrayBufferSize( PMIDL_STUB_MESSAGE pStubMsg,
                                                 unsigned char* pMemory, PFORMAT_STRING pFormat )
{
    unsigned char alignment = pFormat[1] + 1;
    DWORD esize = *(const WORD*)(pFormat+2);

    TRACE("(%p, %p, %p)\n", pStubMsg, pMemory, pFormat);

    if (pFormat[0] != RPC_FC_CVARRAY)
    {
        ERR("invalid format type %x\n", pFormat[0]);
        RpcRaiseException(RPC_S_INTERNAL_ERROR);
        return;
    }

    /* compute size */
    pFormat = ComputeConformance(pStubMsg, pMemory, pFormat+4, 0);
    /* compute length */
    pFormat = ComputeVariance(pStubMsg, pMemory, pFormat, 0);

    SizeConformance(pStubMsg);
    SizeVariance(pStubMsg);

    ALIGN_LENGTH(pStubMsg->BufferLength, alignment);

    pStubMsg->BufferLength += pStubMsg->ActualCount*esize;

    EmbeddedPointerBufferSize(pStubMsg, pMemory, pFormat);
}


/***********************************************************************
 *           NdrConformantVaryingArrayMemorySize  [RPCRT4.@]
 */
unsigned long WINAPI NdrConformantVaryingArrayMemorySize( PMIDL_STUB_MESSAGE pStubMsg,
                                                          PFORMAT_STRING pFormat )
{
    FIXME( "stub\n" );
    return 0;
}


/***********************************************************************
 *           NdrComplexArrayMarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrComplexArrayMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                               unsigned char *pMemory,
                                               PFORMAT_STRING pFormat)
{
  ULONG i, count, def;
  BOOL variance_present;
  unsigned char alignment;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  if (pFormat[0] != RPC_FC_BOGUS_ARRAY)
  {
      ERR("invalid format type %x\n", pFormat[0]);
      RpcRaiseException(RPC_S_INTERNAL_ERROR);
      return NULL;
  }

  alignment = pFormat[1] + 1;

  def = *(const WORD*)&pFormat[2];
  pFormat += 4;

  pFormat = ComputeConformance(pStubMsg, pMemory, pFormat, def);
  TRACE("conformance = %ld\n", pStubMsg->MaxCount);

  variance_present = IsConformanceOrVariancePresent(pFormat);
  pFormat = ComputeVariance(pStubMsg, pMemory, pFormat, pStubMsg->MaxCount);
  TRACE("variance = %ld\n", pStubMsg->ActualCount);

  WriteConformance(pStubMsg);
  if (variance_present)
    WriteVariance(pStubMsg);

  ALIGN_POINTER(pStubMsg->Buffer, alignment);

  count = pStubMsg->ActualCount;
  for (i = 0; i < count; i++)
    pMemory = ComplexMarshall(pStubMsg, pMemory, pFormat, NULL);

  STD_OVERFLOW_CHECK(pStubMsg);

  return NULL;
}

/***********************************************************************
 *           NdrComplexArrayUnmarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrComplexArrayUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                 unsigned char **ppMemory,
                                                 PFORMAT_STRING pFormat,
                                                 unsigned char fMustAlloc)
{
  ULONG i, count, esize;
  unsigned char alignment;
  unsigned char *pMemory;
  unsigned char *Buffer;

  TRACE("(%p,%p,%p,%d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);

  if (pFormat[0] != RPC_FC_BOGUS_ARRAY)
  {
      ERR("invalid format type %x\n", pFormat[0]);
      RpcRaiseException(RPC_S_INTERNAL_ERROR);
      return NULL;
  }

  alignment = pFormat[1] + 1;

  pFormat += 4;

  pFormat = ReadConformance(pStubMsg, pFormat);
  pFormat = ReadVariance(pStubMsg, pFormat);

  Buffer = pStubMsg->Buffer;
  esize = ComplexStructMemorySize(pStubMsg, pFormat);
  pStubMsg->Buffer = Buffer;

  if (fMustAlloc || !*ppMemory)
  {
    *ppMemory = NdrAllocate(pStubMsg, pStubMsg->MaxCount * esize);
    memset(*ppMemory, 0, pStubMsg->MaxCount * esize);
  }

  ALIGN_POINTER(pStubMsg->Buffer, alignment);

  pMemory = *ppMemory;
  count = pStubMsg->ActualCount;
  for (i = 0; i < count; i++)
    pMemory = ComplexUnmarshall(pStubMsg, pMemory, pFormat, NULL, fMustAlloc);

  return NULL;
}

/***********************************************************************
 *           NdrComplexArrayBufferSize [RPCRT4.@]
 */
void WINAPI NdrComplexArrayBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                      unsigned char *pMemory,
                                      PFORMAT_STRING pFormat)
{
  ULONG i, count, def;
  unsigned char alignment;
  BOOL variance_present;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  if (pFormat[0] != RPC_FC_BOGUS_ARRAY)
  {
      ERR("invalid format type %x\n", pFormat[0]);
      RpcRaiseException(RPC_S_INTERNAL_ERROR);
      return;
  }

  alignment = pFormat[1] + 1;

  def = *(const WORD*)&pFormat[2];
  pFormat += 4;

  pFormat = ComputeConformance(pStubMsg, pMemory, pFormat, def);
  TRACE("conformance = %ld\n", pStubMsg->MaxCount);
  SizeConformance(pStubMsg);

  variance_present = IsConformanceOrVariancePresent(pFormat);
  pFormat = ComputeVariance(pStubMsg, pMemory, pFormat, pStubMsg->MaxCount);
  TRACE("variance = %ld\n", pStubMsg->ActualCount);

  if (variance_present)
    SizeVariance(pStubMsg);

  ALIGN_LENGTH(pStubMsg->BufferLength, alignment);

  count = pStubMsg->ActualCount;
  for (i = 0; i < count; i++)
    pMemory = ComplexBufferSize(pStubMsg, pMemory, pFormat, NULL);
}

/***********************************************************************
 *           NdrComplexArrayMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrComplexArrayMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                               PFORMAT_STRING pFormat)
{
  ULONG i, count, esize;
  unsigned char alignment;
  unsigned char *Buffer;
  unsigned long SavedMemorySize;
  unsigned long MemorySize;

  TRACE("(%p,%p)\n", pStubMsg, pFormat);

  if (pFormat[0] != RPC_FC_BOGUS_ARRAY)
  {
      ERR("invalid format type %x\n", pFormat[0]);
      RpcRaiseException(RPC_S_INTERNAL_ERROR);
      return 0;
  }

  alignment = pFormat[1] + 1;

  pFormat += 4;

  pFormat = ReadConformance(pStubMsg, pFormat);
  pFormat = ReadVariance(pStubMsg, pFormat);

  ALIGN_POINTER(pStubMsg->Buffer, alignment);

  SavedMemorySize = pStubMsg->MemorySize;

  Buffer = pStubMsg->Buffer;
  esize = ComplexStructMemorySize(pStubMsg, pFormat);
  pStubMsg->Buffer = Buffer;

  MemorySize = esize * pStubMsg->MaxCount;

  count = pStubMsg->ActualCount;
  for (i = 0; i < count; i++)
    ComplexStructMemorySize(pStubMsg, pFormat);

  pStubMsg->MemorySize = SavedMemorySize;

  pStubMsg->MemorySize += MemorySize;
  return MemorySize;
}

/***********************************************************************
 *           NdrComplexArrayFree [RPCRT4.@]
 */
void WINAPI NdrComplexArrayFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
  ULONG i, count, def;

  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);

  if (pFormat[0] != RPC_FC_BOGUS_ARRAY)
  {
      ERR("invalid format type %x\n", pFormat[0]);
      RpcRaiseException(RPC_S_INTERNAL_ERROR);
      return;
  }

  def = *(const WORD*)&pFormat[2];
  pFormat += 4;

  pFormat = ComputeConformance(pStubMsg, pMemory, pFormat, def);
  TRACE("conformance = %ld\n", pStubMsg->MaxCount);

  pFormat = ComputeVariance(pStubMsg, pMemory, pFormat, pStubMsg->MaxCount);
  TRACE("variance = %ld\n", pStubMsg->ActualCount);

  count = pStubMsg->ActualCount;
  for (i = 0; i < count; i++)
    pMemory = ComplexFree(pStubMsg, pMemory, pFormat, NULL);
}

unsigned long UserMarshalFlags(PMIDL_STUB_MESSAGE pStubMsg)
{
  return MAKELONG(pStubMsg->dwDestContext,
                  pStubMsg->RpcMsg->DataRepresentation);
}

/***********************************************************************
 *           NdrUserMarshalMarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrUserMarshalMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                              unsigned char *pMemory,
                                              PFORMAT_STRING pFormat)
{
/*  unsigned flags = pFormat[1]; */
  unsigned index = *(const WORD*)&pFormat[2];
  unsigned long uflag = UserMarshalFlags(pStubMsg);
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  TRACE("index=%d\n", index);

  pStubMsg->Buffer =
    pStubMsg->StubDesc->aUserMarshalQuadruple[index].pfnMarshall(
      &uflag, pStubMsg->Buffer, pMemory);

  STD_OVERFLOW_CHECK(pStubMsg);

  return NULL;
}

/***********************************************************************
 *           NdrUserMarshalUnmarshall [RPCRT4.@]
 */
unsigned char * WINAPI NdrUserMarshalUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                 unsigned char **ppMemory,
                                                 PFORMAT_STRING pFormat,
                                                 unsigned char fMustAlloc)
{
/*  unsigned flags = pFormat[1];*/
  unsigned index = *(const WORD*)&pFormat[2];
  DWORD memsize = *(const WORD*)&pFormat[4];
  unsigned long uflag = UserMarshalFlags(pStubMsg);
  TRACE("(%p,%p,%p,%d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);
  TRACE("index=%d\n", index);

  if (fMustAlloc || !*ppMemory)
    *ppMemory = NdrAllocate(pStubMsg, memsize);

  pStubMsg->Buffer =
    pStubMsg->StubDesc->aUserMarshalQuadruple[index].pfnUnmarshall(
      &uflag, pStubMsg->Buffer, *ppMemory);

  return NULL;
}

/***********************************************************************
 *           NdrUserMarshalBufferSize [RPCRT4.@]
 */
void WINAPI NdrUserMarshalBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                      unsigned char *pMemory,
                                      PFORMAT_STRING pFormat)
{
/*  unsigned flags = pFormat[1];*/
  unsigned index = *(const WORD*)&pFormat[2];
  DWORD bufsize = *(const WORD*)&pFormat[6];
  unsigned long uflag = UserMarshalFlags(pStubMsg);
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  TRACE("index=%d\n", index);

  if (bufsize) {
    TRACE("size=%ld\n", bufsize);
    pStubMsg->BufferLength += bufsize;
    return;
  }

  pStubMsg->BufferLength =
    pStubMsg->StubDesc->aUserMarshalQuadruple[index].pfnBufferSize(
      &uflag, pStubMsg->BufferLength, pMemory);
}

/***********************************************************************
 *           NdrUserMarshalMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrUserMarshalMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                              PFORMAT_STRING pFormat)
{
  unsigned index = *(const WORD*)&pFormat[2];
/*  DWORD memsize = *(const WORD*)&pFormat[4]; */
  FIXME("(%p,%p): stub\n", pStubMsg, pFormat);
  TRACE("index=%d\n", index);

  return 0;
}

/***********************************************************************
 *           NdrUserMarshalFree [RPCRT4.@]
 */
void WINAPI NdrUserMarshalFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
/*  unsigned flags = pFormat[1]; */
  unsigned index = *(const WORD*)&pFormat[2];
  unsigned long uflag = UserMarshalFlags(pStubMsg);
  TRACE("(%p,%p,%p)\n", pStubMsg, pMemory, pFormat);
  TRACE("index=%d\n", index);

  pStubMsg->StubDesc->aUserMarshalQuadruple[index].pfnFree(
    &uflag, pMemory);
}

/***********************************************************************
 *           NdrClearOutParameters [RPCRT4.@]
 */
void WINAPI NdrClearOutParameters(PMIDL_STUB_MESSAGE pStubMsg,
                                  PFORMAT_STRING pFormat,
                                  void *ArgAddr)
{
  FIXME("(%p,%p,%p): stub\n", pStubMsg, pFormat, ArgAddr);
}

/***********************************************************************
 *           NdrConvert [RPCRT4.@]
 */
void WINAPI NdrConvert( PMIDL_STUB_MESSAGE pStubMsg, PFORMAT_STRING pFormat )
{
  FIXME("(pStubMsg == ^%p, pFormat == ^%p): stub.\n", pStubMsg, pFormat);
  /* FIXME: since this stub doesn't do any converting, the proper behavior
     is to raise an exception */
}

/***********************************************************************
 *           NdrConvert2 [RPCRT4.@]
 */
void WINAPI NdrConvert2( PMIDL_STUB_MESSAGE pStubMsg, PFORMAT_STRING pFormat, long NumberParams )
{
  FIXME("(pStubMsg == ^%p, pFormat == ^%p, NumberParams == %ld): stub.\n",
    pStubMsg, pFormat, NumberParams);
  /* FIXME: since this stub doesn't do any converting, the proper behavior
     is to raise an exception */
}

typedef struct _NDR_CSTRUCT_FORMAT
{
    unsigned char type;
    unsigned char alignment;
    unsigned short memory_size;
    short offset_to_array_description;
} NDR_CSTRUCT_FORMAT;

/***********************************************************************
 *           NdrConformantStructMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrConformantStructMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    const NDR_CSTRUCT_FORMAT * pCStructFormat = (NDR_CSTRUCT_FORMAT*)pFormat;
    pFormat += sizeof(NDR_CSTRUCT_FORMAT);

    TRACE("(%p, %p, %p)\n", pStubMsg, pMemory, pFormat);

    if ((pCStructFormat->type != RPC_FC_CPSTRUCT) && (pCStructFormat->type != RPC_FC_CSTRUCT))
    {
        ERR("invalid format type %x\n", pCStructFormat->type);
        RpcRaiseException(RPC_S_INTERNAL_ERROR);
        return NULL;
    }

    ALIGN_POINTER(pStubMsg->Buffer, pCStructFormat->alignment + 1);

    TRACE("memory_size = %d\n", pCStructFormat->memory_size);

    /* copy constant sized part of struct */
    memcpy(pStubMsg->Buffer, pMemory, pCStructFormat->memory_size);
    pStubMsg->Buffer += pCStructFormat->memory_size;

    if (pCStructFormat->offset_to_array_description)
    {
        PFORMAT_STRING pArrayFormat = (unsigned char*)&pCStructFormat->offset_to_array_description +
            pCStructFormat->offset_to_array_description;
        NdrConformantArrayMarshall(pStubMsg, pMemory + pCStructFormat->memory_size, pArrayFormat);
    }
    if (pCStructFormat->type == RPC_FC_CPSTRUCT)
        EmbeddedPointerMarshall(pStubMsg, pMemory, pFormat);
    return NULL;
}

/***********************************************************************
 *           NdrConformantStructUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrConformantStructUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    const NDR_CSTRUCT_FORMAT * pCStructFormat = (NDR_CSTRUCT_FORMAT*)pFormat;
    pFormat += sizeof(NDR_CSTRUCT_FORMAT);

    TRACE("(%p, %p, %p, %d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);

    if ((pCStructFormat->type != RPC_FC_CPSTRUCT) && (pCStructFormat->type != RPC_FC_CSTRUCT))
    {
        ERR("invalid format type %x\n", pCStructFormat->type);
        RpcRaiseException(RPC_S_INTERNAL_ERROR);
        return NULL;
    }

    ALIGN_POINTER(pStubMsg->Buffer, pCStructFormat->alignment + 1);

    TRACE("memory_size = %d\n", pCStructFormat->memory_size);

    /* work out how much memory to allocate if we need to do so */
    if (!*ppMemory || fMustAlloc)
    {
        SIZE_T size = pCStructFormat->memory_size;
    
        if (pCStructFormat->offset_to_array_description)
        {
            unsigned char *buffer;
            PFORMAT_STRING pArrayFormat = (unsigned char*)&pCStructFormat->offset_to_array_description +
                pCStructFormat->offset_to_array_description;
            buffer = pStubMsg->Buffer;
            pStubMsg->Buffer += pCStructFormat->memory_size;
            size += NdrConformantArrayMemorySize(pStubMsg, pArrayFormat);
            pStubMsg->Buffer = buffer;
        }
        *ppMemory = NdrAllocate(pStubMsg, size);
    }

    /* now copy the data */
    memcpy(*ppMemory, pStubMsg->Buffer, pCStructFormat->memory_size);
    pStubMsg->Buffer += pCStructFormat->memory_size;
    if (pCStructFormat->offset_to_array_description)
    {
        PFORMAT_STRING pArrayFormat = (unsigned char*)&pCStructFormat->offset_to_array_description +
            pCStructFormat->offset_to_array_description;
        unsigned char *pMemoryArray = *ppMemory + pCStructFormat->memory_size;
        /* note that we pass fMustAlloc as 0 as we have already allocated the
         * memory */
        NdrConformantArrayUnmarshall(pStubMsg, &pMemoryArray, pArrayFormat, 0);
    }
    if (pCStructFormat->type == RPC_FC_CPSTRUCT)
        EmbeddedPointerUnmarshall(pStubMsg, ppMemory, pFormat, fMustAlloc);
    return NULL;
}

/***********************************************************************
 *           NdrConformantStructBufferSize [RPCRT4.@]
 */
void WINAPI NdrConformantStructBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    const NDR_CSTRUCT_FORMAT * pCStructFormat = (NDR_CSTRUCT_FORMAT*)pFormat;
    pFormat += sizeof(NDR_CSTRUCT_FORMAT);
    TRACE("(%p, %p, %p)\n", pStubMsg, pMemory, pFormat);

    if ((pCStructFormat->type != RPC_FC_CPSTRUCT) && (pCStructFormat->type != RPC_FC_CSTRUCT))
    {
        ERR("invalid format type %x\n", pCStructFormat->type);
        RpcRaiseException(RPC_S_INTERNAL_ERROR);
        return;
    }

    ALIGN_LENGTH(pStubMsg->BufferLength, pCStructFormat->alignment + 1);

    TRACE("memory_size = %d\n", pCStructFormat->memory_size);

    /* add constant sized part of struct to buffer size */
    pStubMsg->BufferLength += pCStructFormat->memory_size;

    if (pCStructFormat->offset_to_array_description)
    {
        PFORMAT_STRING pArrayFormat = (unsigned char*)&pCStructFormat->offset_to_array_description +
            pCStructFormat->offset_to_array_description;
        NdrConformantArrayBufferSize(pStubMsg, pMemory + pCStructFormat->memory_size, pArrayFormat);
    }
    if (pCStructFormat->type == RPC_FC_CPSTRUCT)
        EmbeddedPointerBufferSize(pStubMsg, pMemory, pFormat);
}

/***********************************************************************
 *           NdrConformantStructMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrConformantStructMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

/***********************************************************************
 *           NdrConformantStructFree [RPCRT4.@]
 */
void WINAPI NdrConformantStructFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrConformantVaryingStructMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrConformantVaryingStructMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrConformantVaryingStructUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrConformantVaryingStructUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrConformantVaryingStructBufferSize [RPCRT4.@]
 */
void WINAPI NdrConformantVaryingStructBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrConformantVaryingStructMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrConformantVaryingStructMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

/***********************************************************************
 *           NdrConformantVaryingStructFree [RPCRT4.@]
 */
void WINAPI NdrConformantVaryingStructFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrFixedArrayMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrFixedArrayMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrFixedArrayUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrFixedArrayUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrFixedArrayBufferSize [RPCRT4.@]
 */
void WINAPI NdrFixedArrayBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrFixedArrayMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrFixedArrayMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

/***********************************************************************
 *           NdrFixedArrayFree [RPCRT4.@]
 */
void WINAPI NdrFixedArrayFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrVaryingArrayMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrVaryingArrayMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrVaryingArrayUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrVaryingArrayUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrVaryingArrayBufferSize [RPCRT4.@]
 */
void WINAPI NdrVaryingArrayBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrVaryingArrayMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrVaryingArrayMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

/***********************************************************************
 *           NdrVaryingArrayFree [RPCRT4.@]
 */
void WINAPI NdrVaryingArrayFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrEncapsulatedUnionMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrEncapsulatedUnionMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrEncapsulatedUnionUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrEncapsulatedUnionUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrEncapsulatedUnionBufferSize [RPCRT4.@]
 */
void WINAPI NdrEncapsulatedUnionBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrEncapsulatedUnionMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrEncapsulatedUnionMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

/***********************************************************************
 *           NdrEncapsulatedUnionFree [RPCRT4.@]
 */
void WINAPI NdrEncapsulatedUnionFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

static PFORMAT_STRING get_arm_offset_from_union_arm_selector(PMIDL_STUB_MESSAGE pStubMsg,
                                                             unsigned long discriminant,
                                                             PFORMAT_STRING pFormat)
{
    unsigned short num_arms, arm, type;

    num_arms = *(const SHORT*)pFormat & 0x0fff;
    pFormat += 2;
    for(arm = 0; arm < num_arms; arm++)
    {
        if(discriminant == *(const ULONG*)pFormat)
        {
            pFormat += 4;
            break;
        }
        pFormat += 6;
    }

    type = *(const unsigned short*)pFormat;
    TRACE("type %04x\n", type);
    if(arm == num_arms) /* default arm extras */
    {
        if(type == 0xffff)
        {
            FIXME("should raise an exception here\n");
            return NULL;
        }
        if(type == 0)
        {
            /* Don't marshall any type. FIXME is this correct? */
            return NULL;
        }
    }
    return pFormat;
}

static PFORMAT_STRING get_non_encapsulated_union_arm(PMIDL_STUB_MESSAGE pStubMsg,
                                                     unsigned char *pMemory,
                                                     PFORMAT_STRING pFormat)
{
    ULONG value;

    pFormat = ComputeConformanceOrVariance(pStubMsg, pMemory, pFormat,
                                           0, &value);
    TRACE("got switch value %lx\n", value);
    pFormat += *(const SHORT*)pFormat;
    pFormat += 2;

    return get_arm_offset_from_union_arm_selector(pStubMsg, value, pFormat);
}

static unsigned char *get_conformance_address(PMIDL_STUB_MESSAGE pStubMsg,
                                              unsigned char *pMemory,
                                              PFORMAT_STRING pFormat)
{
    short ofs = *(short *)&pFormat[2];
    LPVOID ptr = NULL;

    switch (pFormat[0] & 0xf0)
    {
    case RPC_FC_NORMAL_CONFORMANCE:
        ptr = pMemory;
        break;
    default:
        FIXME("Conformance type %x\n", pFormat[0]);
        return NULL;
    }

    if(pFormat[1])
    {
        FIXME("Conformance op %x\n", pFormat[1]);
        return NULL;
    }

    return (unsigned char *)ptr + ofs;
}

/***********************************************************************
 *           NdrNonEncapsulatedUnionMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrNonEncapsulatedUnionMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    unsigned char *discriminant;
    unsigned short type;

    TRACE("(%p, %p, %p)\n", pStubMsg, pMemory, pFormat);
    pFormat++;

    /* Marshall discriminant */
    discriminant = get_conformance_address(pStubMsg, pMemory, pFormat + 1);
    NdrBaseTypeMarshall(pStubMsg, discriminant, pFormat);
    pFormat++;

    pFormat = get_non_encapsulated_union_arm(pStubMsg, pMemory, pFormat);
    if(!pFormat)
        return NULL;

    type = *(const unsigned short*)pFormat;
    if((type & 0xff00) == 0x8000)
    {
        unsigned char basetype = LOBYTE(type);
        return NdrBaseTypeMarshall(pStubMsg, pMemory, &basetype);
    }
    else
    {
        PFORMAT_STRING desc = pFormat + *(const SHORT*)pFormat;
        NDR_MARSHALL m = NdrMarshaller[*desc & NDR_TABLE_MASK];
        if (m)
        {
            unsigned char *saved_buffer = NULL;
            switch(*desc)
            {
            case RPC_FC_RP:
            case RPC_FC_UP:
            case RPC_FC_OP:
            case RPC_FC_FP:
                saved_buffer = pStubMsg->Buffer;
                pStubMsg->Buffer += 4; /* for pointer ID */
                PointerMarshall(pStubMsg, saved_buffer, *(unsigned char **)pMemory, desc);
                break;
            default:
                m(pStubMsg, pMemory, desc);
            }
        }
        else FIXME("no marshaller for embedded type %02x\n", *desc);
    }
    return NULL;
}

static long unmarshall_discriminant(PMIDL_STUB_MESSAGE pStubMsg,
                                    PFORMAT_STRING *ppFormat)
{
    long discriminant = 0;

    switch(**ppFormat)
    {
    case RPC_FC_BYTE:
    case RPC_FC_CHAR:
    case RPC_FC_SMALL:
    case RPC_FC_USMALL:
        discriminant = *(UCHAR *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(UCHAR);
        break;
    case RPC_FC_WCHAR:
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(USHORT));
        discriminant = *(USHORT *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(USHORT);
        break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(ULONG));
        discriminant = *(ULONG *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(ULONG);
        break;
    default:
        FIXME("Unhandled base type: 0x%02x\n", **ppFormat);
    }
    (*ppFormat)++;

    if (pStubMsg->fHasNewCorrDesc)
        *ppFormat += 6;
    else
        *ppFormat += 4;
    return discriminant;
}

/**********************************************************************
 *           NdrNonEncapsulatedUnionUnmarshall[RPCRT4.@]
 */
unsigned char *  WINAPI NdrNonEncapsulatedUnionUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    long discriminant;
    unsigned short type, size;

    TRACE("(%p, %p, %p, %d)\n", pStubMsg, ppMemory, pFormat, fMustAlloc);
    pFormat++;

    /* Unmarshall discriminant */
    discriminant = unmarshall_discriminant(pStubMsg, &pFormat);
    TRACE("unmarshalled discriminant %lx\n", discriminant);

    pFormat += *(const SHORT*)pFormat;

    size = *(const unsigned short*)pFormat;
    pFormat += 2;

    pFormat = get_arm_offset_from_union_arm_selector(pStubMsg, discriminant, pFormat);
    if(!pFormat)
        return NULL;

    if(!*ppMemory || fMustAlloc)
        *ppMemory = NdrAllocate(pStubMsg, size);

    type = *(const unsigned short*)pFormat;
    if((type & 0xff00) == 0x8000)
    {
        unsigned char basetype = LOBYTE(type);
        return NdrBaseTypeUnmarshall(pStubMsg, ppMemory, &basetype, fMustAlloc);
    }
    else
    {
        PFORMAT_STRING desc = pFormat + *(const SHORT*)pFormat;
        NDR_UNMARSHALL m = NdrUnmarshaller[*desc & NDR_TABLE_MASK];
        if (m)
        {
            unsigned char *saved_buffer = NULL;
            switch(*desc)
            {
            case RPC_FC_RP:
            case RPC_FC_UP:
            case RPC_FC_OP:
            case RPC_FC_FP:
                **(void***)ppMemory = NULL;
                ALIGN_POINTER(pStubMsg->Buffer, 4);
                saved_buffer = pStubMsg->Buffer;
                pStubMsg->Buffer += 4; /* for pointer ID */
                PointerUnmarshall(pStubMsg, saved_buffer, *(unsigned char ***)ppMemory, desc, fMustAlloc);
                break;
            default:
                m(pStubMsg, ppMemory, desc, fMustAlloc);
            }
        }
        else FIXME("no marshaller for embedded type %02x\n", *desc);
    }
    return NULL;
}

/***********************************************************************
 *           NdrNonEncapsulatedUnionBufferSize [RPCRT4.@]
 */
void WINAPI NdrNonEncapsulatedUnionBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    unsigned short type;
    TRACE("(%p, %p, %p)\n", pStubMsg, pMemory, pFormat);

    pFormat++;
    /* Add discriminant size */
    NdrBaseTypeBufferSize(pStubMsg, pMemory, pFormat);
    pFormat++;

    pFormat = get_non_encapsulated_union_arm(pStubMsg, pMemory, pFormat);
    if(!pFormat)
        return;

    type = *(const unsigned short*)pFormat;
    if((type & 0xff00) == 0x8000)
    {
        unsigned char basetype = LOBYTE(type);
        NdrBaseTypeBufferSize(pStubMsg, pMemory, &basetype);
    }
    else
    {
        PFORMAT_STRING desc = pFormat + *(const SHORT*)pFormat;
        NDR_BUFFERSIZE m = NdrBufferSizer[*desc & NDR_TABLE_MASK];
        if (m)
        {
            switch(*desc)
            {
            case RPC_FC_RP:
            case RPC_FC_UP:
            case RPC_FC_OP:
            case RPC_FC_FP:
                ALIGN_LENGTH(pStubMsg->BufferLength, 4);
                pStubMsg->BufferLength += 4; /* for pointer ID */
                PointerBufferSize(pStubMsg, *(unsigned char **)pMemory, desc);
                break;
            default:
                m(pStubMsg, pMemory, desc);
            }
        }
        else FIXME("no buffersizer for embedded type %02x\n", *desc);
    }
    return;
}

/***********************************************************************
 *           NdrNonEncapsulatedUnionMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrNonEncapsulatedUnionMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    pFormat += 2;
    if (pStubMsg->fHasNewCorrDesc)
        pFormat += 6;
    else
        pFormat += 4;

    pFormat += *(const SHORT*)pFormat;
    TRACE("size %d\n", *(const SHORT*)pFormat);
    return *(const SHORT*)pFormat;
}

/***********************************************************************
 *           NdrNonEncapsulatedUnionFree [RPCRT4.@]
 */
void WINAPI NdrNonEncapsulatedUnionFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrByteCountPointerMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrByteCountPointerMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrByteCountPointerUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrByteCountPointerUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrByteCountPointerBufferSize [RPCRT4.@]
 */
void WINAPI NdrByteCountPointerBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrByteCountPointerMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrByteCountPointerMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

/***********************************************************************
 *           NdrByteCountPointerFree [RPCRT4.@]
 */
void WINAPI NdrByteCountPointerFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrXmitOrRepAsMarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrXmitOrRepAsMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrXmitOrRepAsUnmarshall [RPCRT4.@]
 */
unsigned char *  WINAPI NdrXmitOrRepAsUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char **ppMemory,
                                PFORMAT_STRING pFormat,
                                unsigned char fMustAlloc)
{
    FIXME("stub\n");
    return NULL;
}

/***********************************************************************
 *           NdrXmitOrRepAsBufferSize [RPCRT4.@]
 */
void WINAPI NdrXmitOrRepAsBufferSize(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrXmitOrRepAsMemorySize [RPCRT4.@]
 */
unsigned long WINAPI NdrXmitOrRepAsMemorySize(PMIDL_STUB_MESSAGE pStubMsg,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
    return 0;
}

/***********************************************************************
 *           NdrXmitOrRepAsFree [RPCRT4.@]
 */
void WINAPI NdrXmitOrRepAsFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
    FIXME("stub\n");
}

/***********************************************************************
 *           NdrBaseTypeMarshall [internal]
 */
static unsigned char *WINAPI NdrBaseTypeMarshall(
    PMIDL_STUB_MESSAGE pStubMsg,
    unsigned char *pMemory,
    PFORMAT_STRING pFormat)
{
    TRACE("pStubMsg %p, pMemory %p, type 0x%02x\n", pStubMsg, pMemory, *pFormat);

    switch(*pFormat)
    {
    case RPC_FC_BYTE:
    case RPC_FC_CHAR:
    case RPC_FC_SMALL:
    case RPC_FC_USMALL:
        *(UCHAR *)pStubMsg->Buffer = *(UCHAR *)pMemory;
        pStubMsg->Buffer += sizeof(UCHAR);
        TRACE("value: 0x%02x\n", *(UCHAR *)pMemory);
        break;
    case RPC_FC_WCHAR:
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(USHORT));
        *(USHORT *)pStubMsg->Buffer = *(USHORT *)pMemory;
        pStubMsg->Buffer += sizeof(USHORT);
        TRACE("value: 0x%04x\n", *(USHORT *)pMemory);
        break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
    case RPC_FC_ERROR_STATUS_T:
    case RPC_FC_ENUM32:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(ULONG));
        *(ULONG *)pStubMsg->Buffer = *(ULONG *)pMemory;
        pStubMsg->Buffer += sizeof(ULONG);
        TRACE("value: 0x%08lx\n", *(ULONG *)pMemory);
        break;
    case RPC_FC_FLOAT:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(float));
        *(float *)pStubMsg->Buffer = *(float *)pMemory;
        pStubMsg->Buffer += sizeof(float);
        break;
    case RPC_FC_DOUBLE:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(double));
        *(double *)pStubMsg->Buffer = *(double *)pMemory;
        pStubMsg->Buffer += sizeof(double);
        break;
    case RPC_FC_HYPER:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(ULONGLONG));
        *(ULONGLONG *)pStubMsg->Buffer = *(ULONGLONG *)pMemory;
        pStubMsg->Buffer += sizeof(ULONGLONG);
        TRACE("value: %s\n", wine_dbgstr_longlong(*(ULONGLONG*)pMemory));
        break;
    case RPC_FC_ENUM16:
        /* only 16-bits on the wire, so do a sanity check */
        if (*(UINT *)pMemory > USHRT_MAX)
            RpcRaiseException(RPC_X_ENUM_VALUE_OUT_OF_RANGE);
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(USHORT));
        *(USHORT *)pStubMsg->Buffer = *(UINT *)pMemory;
        pStubMsg->Buffer += sizeof(USHORT);
        TRACE("value: 0x%04x\n", *(UINT *)pMemory);
        break;
    default:
        FIXME("Unhandled base type: 0x%02x\n", *pFormat);
    }

    STD_OVERFLOW_CHECK(pStubMsg);

    /* FIXME: what is the correct return value? */
    return NULL;
}

/***********************************************************************
 *           NdrBaseTypeUnmarshall [internal]
 */
static unsigned char *WINAPI NdrBaseTypeUnmarshall(
    PMIDL_STUB_MESSAGE pStubMsg,
    unsigned char **ppMemory,
    PFORMAT_STRING pFormat,
    unsigned char fMustAlloc)
{
    TRACE("pStubMsg: %p, ppMemory: %p, type: 0x%02x, fMustAlloc: %s\n", pStubMsg, ppMemory, *pFormat, fMustAlloc ? "true" : "false");

    if (fMustAlloc || !*ppMemory)
    {
        unsigned char *Buffer = pStubMsg->Buffer;
        *ppMemory = NdrAllocate(pStubMsg, NdrBaseTypeMemorySize(pStubMsg, pFormat));
        pStubMsg->Buffer = Buffer;
    }

    TRACE("*ppMemory: %p\n", *ppMemory);

    switch(*pFormat)
    {
    case RPC_FC_BYTE:
    case RPC_FC_CHAR:
    case RPC_FC_SMALL:
    case RPC_FC_USMALL:
        **(UCHAR **)ppMemory = *(UCHAR *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(UCHAR);
        TRACE("value: 0x%02x\n", **(UCHAR **)ppMemory);
        break;
    case RPC_FC_WCHAR:
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(USHORT));
        **(USHORT **)ppMemory = *(USHORT *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(USHORT);
        TRACE("value: 0x%04x\n", **(USHORT **)ppMemory);
        break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
    case RPC_FC_ERROR_STATUS_T:
    case RPC_FC_ENUM32:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(ULONG));
        **(ULONG **)ppMemory = *(ULONG *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(ULONG);
        TRACE("value: 0x%08lx\n", **(ULONG **)ppMemory);
        break;
   case RPC_FC_FLOAT:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(float));
        **(float **)ppMemory = *(float *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(float);
        TRACE("value: %f\n", **(float **)ppMemory);
        break;
    case RPC_FC_DOUBLE:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(double));
        **(double **)ppMemory = *(double*)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(double);
        TRACE("value: %f\n", **(double **)ppMemory);
        break;
    case RPC_FC_HYPER:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(ULONGLONG));
        **(ULONGLONG **)ppMemory = *(ULONGLONG *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(ULONGLONG);
        TRACE("value: %s\n", wine_dbgstr_longlong(**(ULONGLONG **)ppMemory));
        break;
    case RPC_FC_ENUM16:
        ALIGN_POINTER(pStubMsg->Buffer, sizeof(USHORT));
        /* 16-bits on the wire, but int in memory */
        **(UINT **)ppMemory = *(USHORT *)pStubMsg->Buffer;
        pStubMsg->Buffer += sizeof(USHORT);
        TRACE("value: 0x%08x\n", **(UINT **)ppMemory);
        break;
    default:
        FIXME("Unhandled base type: 0x%02x\n", *pFormat);
    }

    /* FIXME: what is the correct return value? */

    return NULL;
}

/***********************************************************************
 *           NdrBaseTypeBufferSize [internal]
 */
static void WINAPI NdrBaseTypeBufferSize(
    PMIDL_STUB_MESSAGE pStubMsg,
    unsigned char *pMemory,
    PFORMAT_STRING pFormat)
{
    TRACE("pStubMsg %p, pMemory %p, type 0x%02x\n", pStubMsg, pMemory, *pFormat);

    switch(*pFormat)
    {
    case RPC_FC_BYTE:
    case RPC_FC_CHAR:
    case RPC_FC_SMALL:
    case RPC_FC_USMALL:
        pStubMsg->BufferLength += sizeof(UCHAR);
        break;
    case RPC_FC_WCHAR:
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
    case RPC_FC_ENUM16:
        ALIGN_LENGTH(pStubMsg->BufferLength, sizeof(USHORT));
        pStubMsg->BufferLength += sizeof(USHORT);
        break;
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
    case RPC_FC_ENUM32:
        ALIGN_LENGTH(pStubMsg->BufferLength, sizeof(ULONG));
        pStubMsg->BufferLength += sizeof(ULONG);
        break;
    case RPC_FC_FLOAT:
        ALIGN_LENGTH(pStubMsg->BufferLength, sizeof(float));
        pStubMsg->BufferLength += sizeof(float);
        break;
    case RPC_FC_DOUBLE:
        ALIGN_LENGTH(pStubMsg->BufferLength, sizeof(double));
        pStubMsg->BufferLength += sizeof(double);
        break;
    case RPC_FC_HYPER:
        ALIGN_LENGTH(pStubMsg->BufferLength, sizeof(ULONGLONG));
        pStubMsg->BufferLength += sizeof(ULONGLONG);
        break;
    case RPC_FC_ERROR_STATUS_T:
        ALIGN_LENGTH(pStubMsg->BufferLength, sizeof(error_status_t));
        pStubMsg->BufferLength += sizeof(error_status_t);
        break;
    default:
        FIXME("Unhandled base type: 0x%02x\n", *pFormat);
    }
}

/***********************************************************************
 *           NdrBaseTypeMemorySize [internal]
 */
static unsigned long WINAPI NdrBaseTypeMemorySize(
    PMIDL_STUB_MESSAGE pStubMsg,
    PFORMAT_STRING pFormat)
{
    switch(*pFormat)
    {
    case RPC_FC_BYTE:
    case RPC_FC_CHAR:
    case RPC_FC_SMALL:
    case RPC_FC_USMALL:
        pStubMsg->Buffer += sizeof(UCHAR);
        pStubMsg->MemorySize += sizeof(UCHAR);
        return sizeof(UCHAR);
    case RPC_FC_WCHAR:
    case RPC_FC_SHORT:
    case RPC_FC_USHORT:
        pStubMsg->Buffer += sizeof(USHORT);
        pStubMsg->MemorySize += sizeof(USHORT);
        return sizeof(USHORT);
    case RPC_FC_LONG:
    case RPC_FC_ULONG:
        pStubMsg->Buffer += sizeof(ULONG);
        pStubMsg->MemorySize += sizeof(ULONG);
        return sizeof(ULONG);
    case RPC_FC_FLOAT:
        pStubMsg->Buffer += sizeof(float);
        pStubMsg->MemorySize += sizeof(float);
        return sizeof(float);
    case RPC_FC_DOUBLE:
        pStubMsg->Buffer += sizeof(double);
        pStubMsg->MemorySize += sizeof(double);
        return sizeof(double);
    case RPC_FC_HYPER:
        pStubMsg->Buffer += sizeof(ULONGLONG);
        pStubMsg->MemorySize += sizeof(ULONGLONG);
        return sizeof(ULONGLONG);
    case RPC_FC_ERROR_STATUS_T:
        pStubMsg->Buffer += sizeof(error_status_t);
        pStubMsg->MemorySize += sizeof(error_status_t);
        return sizeof(error_status_t);
    case RPC_FC_ENUM16:
    case RPC_FC_ENUM32:
        pStubMsg->Buffer += sizeof(INT);
        pStubMsg->MemorySize += sizeof(INT);
        return sizeof(INT);
    default:
        FIXME("Unhandled base type: 0x%02x\n", *pFormat);
       return 0;
    }
}

/***********************************************************************
 *           NdrBaseTypeFree [internal]
 */
static void WINAPI NdrBaseTypeFree(PMIDL_STUB_MESSAGE pStubMsg,
                                unsigned char *pMemory,
                                PFORMAT_STRING pFormat)
{
   TRACE("pStubMsg %p pMemory %p type 0x%02x\n", pStubMsg, pMemory, *pFormat);

   /* nothing to do */
}

/***********************************************************************
 *           NdrClientContextMarshall
 */
void WINAPI NdrClientContextMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                     NDR_CCONTEXT ContextHandle,
                                     int fCheck)
{
    FIXME("(%p, %p, %d): stub\n", pStubMsg, ContextHandle, fCheck);
}

/***********************************************************************
 *           NdrClientContextUnmarshall
 */
void WINAPI NdrClientContextUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                       NDR_CCONTEXT * pContextHandle,
                                       RPC_BINDING_HANDLE BindHandle)
{
    FIXME("(%p, %p, %p): stub\n", pStubMsg, pContextHandle, BindHandle);
}

void WINAPI NdrServerContextMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                     NDR_SCONTEXT ContextHandle,
                                     NDR_RUNDOWN RundownRoutine )
{
    FIXME("(%p, %p, %p): stub\n", pStubMsg, ContextHandle, RundownRoutine);
}

NDR_SCONTEXT WINAPI NdrServerContextUnmarshall(PMIDL_STUB_MESSAGE pStubMsg)
{
    FIXME("(%p): stub\n", pStubMsg);
    return NULL;
}

void WINAPI NdrContextHandleSize(PMIDL_STUB_MESSAGE pStubMsg,
                                 unsigned char* pMemory,
                                 PFORMAT_STRING pFormat)
{
    FIXME("(%p, %p, %p): stub\n", pStubMsg, pMemory, pFormat);
}

NDR_SCONTEXT WINAPI NdrContextHandleInitialize(PMIDL_STUB_MESSAGE pStubMsg,
                                               PFORMAT_STRING pFormat)
{
    FIXME("(%p, %p): stub\n", pStubMsg, pFormat);
    return NULL;
}

void WINAPI NdrServerContextNewMarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                        NDR_SCONTEXT ContextHandle,
                                        NDR_RUNDOWN RundownRoutine,
                                        PFORMAT_STRING pFormat)
{
    FIXME("(%p, %p, %p, %p): stub\n", pStubMsg, ContextHandle, RundownRoutine, pFormat);
}

NDR_SCONTEXT WINAPI NdrServerContextNewUnmarshall(PMIDL_STUB_MESSAGE pStubMsg,
                                                  PFORMAT_STRING pFormat)
{
    FIXME("(%p, %p): stub\n", pStubMsg, pFormat);
    return NULL;
}

RPC_BINDING_HANDLE WINAPI NDRCContextBinding(NDR_CCONTEXT CContext)
{
    FIXME("(%p): stub\n", CContext);
    return NULL;
}

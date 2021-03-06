// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_ENDIAN_H
#define MUDUO_NET_ENDIAN_H

#include <stdint.h>
#include <endian.h>

namespace muduo
{
namespace net
{
namespace sockets
{

// the inline assembler code makes type blur,
// so we disable warnings for a while.
#if defined(__clang__) || __GNUC_PREREQ (4,6)
#pragma GCC diagnostic push
#endif
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"

/* uint64_t 的整形数字由机器字节序转化为网络字节序 */
inline uint64_t hostToNetwork64(uint64_t host64)
{
  return htobe64(host64);
}

/* uint32_t 的整形数字由机器字节序转化为网络字节序 */
inline uint32_t hostToNetwork32(uint32_t host32)
{
  return htobe32(host32);
}

/* uint16_t 的整形数字由机器字节序转化为网络字节序 */
inline uint16_t hostToNetwork16(uint16_t host16)
{
  return htobe16(host16);
}

/* uint64_t 的整形数字由网络字节序转化为机器字节序 */
inline uint64_t networkToHost64(uint64_t net64)
{
  return be64toh(net64);
}

/* uint32_t 的整形数字由网络字节序转化为机器字节序 */
inline uint32_t networkToHost32(uint32_t net32)
{
  return be32toh(net32);
}

/* uint16_t 的整形数字由网络字节序转化为机器字节序 */
inline uint16_t networkToHost16(uint16_t net16)
{
  return be16toh(net16);
}
#if defined(__clang__) || __GNUC_PREREQ (4,6)
#pragma GCC diagnostic pop
#else
#pragma GCC diagnostic warning "-Wconversion"
#pragma GCC diagnostic warning "-Wold-style-cast"
#endif


}
}
}

#endif  // MUDUO_NET_ENDIAN_H

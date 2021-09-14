// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Copyright(c) 2010 - 2017,
//     The Regents of the University of California(Regents).All Rights Reserved.
//
//     Redistribution and use in source and binary forms,
//     with or without modification,
//     are permitted provided that the following
//     conditions are met : 1. Redistributions of source code must retain the
//     above copyright notice, this list of conditions and the following
//     disclaimer.2. Redistributions in binary form must reproduce the above
//     copyright notice, this list of conditions and the following disclaimer in
//     the
//             documentation and /
//         or
//         other materials provided with the distribution.3. Neither the name of
//         the Regents nor the names of its contributors may be used to endorse
//         or
//         promote products derived from
//         this software without specific prior written permission.
//
//         IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT,
//     INDIRECT, SPECIAL,
//     INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
//     ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
//     EVEN IF REGENTS HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//     REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
//     INCLUDING, BUT NOT LIMITED TO,
//     THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
//     PARTICULAR PURPOSE.THE SOFTWARE AND ACCOMPANYING DOCUMENTATION,
//     IF ANY,
//     PROVIDED HEREUNDER IS PROVIDED
//     "AS IS".REGENTS HAS NO OBLIGATION TO PROVIDE MAINTENANCE,
//     SUPPORT, UPDATES, ENHANCEMENTS,
//     OR MODIFICATIONS.

// The original source code covered by the above license above has been
// modified significantly by the v8 project authors.

#include "src/execution/riscv64/simulator-riscv64.h"

// Only build the simulator if not compiling for real RISCV hardware.
#if defined(USE_SIMULATOR)

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

#include "src/base/bits.h"
#include "src/base/vector.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/riscv64/constants-riscv64.h"
#include "src/diagnostics/disasm.h"
#include "src/heap/combined-heap.h"
#include "src/runtime/runtime-utils.h"
#include "src/utils/ostreams.h"

// The following code about RVV was based from:
//   https://github.com/riscv/riscv-isa-sim
// Copyright (c) 2010-2017, The Regents of the University of California
// (Regents).  All Rights Reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the Regents nor the
//    names of its contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.

// IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
// SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
// ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF
// REGENTS HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED
// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE. THE SOFTWARE AND ACCOMPANYING DOCUMENTATION, IF ANY, PROVIDED
// HEREUNDER IS PROVIDED "AS IS". REGENTS HAS NO OBLIGATION TO PROVIDE
// MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
#define RVV_VI_GENERAL_LOOP_BASE \
  for (uint64_t i = rvv_vstart(); i < rvv_vl(); i++) {
#define RVV_VI_LOOP_END \
  set_rvv_vstart(0);    \
  }

#define RVV_VI_MASK_VARS       \
  const uint8_t midx = i / 64; \
  const uint8_t mpos = i % 64;

#define RVV_VI_LOOP_MASK_SKIP(BODY)                               \
  RVV_VI_MASK_VARS                                                \
  if (instr_.RvvVM() == 0) {                                      \
    bool skip = ((Rvvelt<uint64_t>(0, midx) >> mpos) & 0x1) == 0; \
    if (skip) {                                                   \
      continue;                                                   \
    }                                                             \
  }

#define RVV_VI_VV_LOOP(BODY)       \
  RVV_VI_GENERAL_LOOP_BASE         \
  RVV_VI_LOOP_MASK_SKIP()          \
  if (rvv_vsew() == E8) {          \
    VV_PARAMS(8);                  \
    BODY                           \
  } else if (rvv_vsew() == E16) {  \
    VV_PARAMS(16);                 \
    BODY                           \
  } else if (rvv_vsew() == E32) {  \
    VV_PARAMS(32);                 \
    BODY                           \
  } else if (rvv_vsew() == E64) {  \
    VV_PARAMS(64);                 \
    BODY                           \
  } else if (rvv_vsew() == E128) { \
    VV_PARAMS(128);                \
    BODY                           \
  } else {                         \
    UNREACHABLE();                 \
  }                                \
  RVV_VI_LOOP_END                  \
  rvv_trace_vd();

#define RVV_VI_VV_ULOOP(BODY)      \
  RVV_VI_GENERAL_LOOP_BASE         \
  RVV_VI_LOOP_MASK_SKIP()          \
  if (rvv_vsew() == E8) {          \
    VV_UPARAMS(8);                 \
    BODY                           \
  } else if (rvv_vsew() == E16) {  \
    VV_UPARAMS(16);                \
    BODY                           \
  } else if (rvv_vsew() == E32) {  \
    VV_UPARAMS(32);                \
    BODY                           \
  } else if (rvv_vsew() == E64) {  \
    VV_UPARAMS(64);                \
    BODY                           \
  } else if (rvv_vsew() == E128) { \
    VV_UPARAMS(128);               \
    BODY                           \
  } else {                         \
    UNREACHABLE();                 \
  }                                \
  RVV_VI_LOOP_END                  \
  rvv_trace_vd();

#define RVV_VI_VX_LOOP(BODY)       \
  RVV_VI_GENERAL_LOOP_BASE         \
  RVV_VI_LOOP_MASK_SKIP()          \
  if (rvv_vsew() == E8) {          \
    VX_PARAMS(8);                  \
    BODY                           \
  } else if (rvv_vsew() == E16) {  \
    VX_PARAMS(16);                 \
    BODY                           \
  } else if (rvv_vsew() == E32) {  \
    VX_PARAMS(32);                 \
    BODY                           \
  } else if (rvv_vsew() == E64) {  \
    VX_PARAMS(64);                 \
    BODY                           \
  } else if (rvv_vsew() == E128) { \
    VX_PARAMS(128);                \
    BODY                           \
  } else {                         \
    UNREACHABLE();                 \
  }                                \
  RVV_VI_LOOP_END                  \
  rvv_trace_vd();

#define RVV_VI_VX_ULOOP(BODY)      \
  RVV_VI_GENERAL_LOOP_BASE         \
  RVV_VI_LOOP_MASK_SKIP()          \
  if (rvv_vsew() == E8) {          \
    VX_UPARAMS(8);                 \
    BODY                           \
  } else if (rvv_vsew() == E16) {  \
    VX_UPARAMS(16);                \
    BODY                           \
  } else if (rvv_vsew() == E32) {  \
    VX_UPARAMS(32);                \
    BODY                           \
  } else if (rvv_vsew() == E64) {  \
    VX_UPARAMS(64);                \
    BODY                           \
  } else if (rvv_vsew() == E128) { \
    VX_UPARAMS(128);               \
    BODY                           \
  } else {                         \
    UNREACHABLE();                 \
  }                                \
  RVV_VI_LOOP_END                  \
  rvv_trace_vd();

#define RVV_VI_VI_LOOP(BODY)       \
  RVV_VI_GENERAL_LOOP_BASE         \
  RVV_VI_LOOP_MASK_SKIP()          \
  if (rvv_vsew() == E8) {          \
    VI_PARAMS(8);                  \
    BODY                           \
  } else if (rvv_vsew() == E16) {  \
    VI_PARAMS(16);                 \
    BODY                           \
  } else if (rvv_vsew() == E32) {  \
    VI_PARAMS(32);                 \
    BODY                           \
  } else if (rvv_vsew() == E64) {  \
    VI_PARAMS(64);                 \
    BODY                           \
  } else if (rvv_vsew() == E128) { \
    VI_PARAMS(128);                \
    BODY                           \
  } else {                         \
    UNREACHABLE();                 \
  }                                \
  RVV_VI_LOOP_END                  \
  rvv_trace_vd();

#define RVV_VI_VI_ULOOP(BODY)      \
  RVV_VI_GENERAL_LOOP_BASE         \
  RVV_VI_LOOP_MASK_SKIP()          \
  if (rvv_vsew() == E8) {          \
    VI_UPARAMS(8);                 \
    BODY                           \
  } else if (rvv_vsew() == E16) {  \
    VI_UPARAMS(16);                \
    BODY                           \
  } else if (rvv_vsew() == E32) {  \
    VI_UPARAMS(32);                \
    BODY                           \
  } else if (rvv_vsew() == E64) {  \
    VI_UPARAMS(64);                \
    BODY                           \
  } else if (rvv_vsew() == E128) { \
    VI_UPARAMS(128);               \
    BODY                           \
  } else {                         \
    UNREACHABLE();                 \
  }                                \
  RVV_VI_LOOP_END                  \
  rvv_trace_vd();

#define RVV_VI_VVXI_MERGE_LOOP(BODY) \
  RVV_VI_GENERAL_LOOP_BASE           \
  if (rvv_vsew() == E8) {            \
    VXI_PARAMS(8);                   \
    BODY;                            \
  } else if (rvv_vsew() == E16) {    \
    VXI_PARAMS(16);                  \
    BODY;                            \
  } else if (rvv_vsew() == E32) {    \
    VXI_PARAMS(32);                  \
    BODY;                            \
  } else if (rvv_vsew() == E64) {    \
    VXI_PARAMS(64);                  \
    BODY;                            \
  } else if (rvv_vsew() == E128) {   \
    VXI_PARAMS(128);                 \
    BODY                             \
  }                                  \
  RVV_VI_LOOP_END                    \
  rvv_trace_vd();

#define VV_WITH_CARRY_PARAMS(x)                                            \
  type_sew_t<x>::type vs2 = Rvvelt<type_sew_t<x>::type>(rvv_vs2_reg(), i); \
  type_sew_t<x>::type vs1 = Rvvelt<type_sew_t<x>::type>(rvv_vs1_reg(), i); \
  type_sew_t<x>::type& vd = Rvvelt<type_sew_t<x>::type>(rvv_vd_reg(), i, true);

#define XI_WITH_CARRY_PARAMS(x)                                             \
  type_sew_t<x>::type vs2 = Rvvelt<type_sew_t<x>::type>(rvv_vs2_reg(), i);  \
  type_sew_t<x>::type rs1 = (type_sew_t<x>::type)(get_register(rs1_reg())); \
  type_sew_t<x>::type simm5 = (type_sew_t<x>::type)instr_.RvvSimm5();       \
  type_sew_t<x>::type& vd = Rvvelt<type_sew_t<x>::type>(rvv_vd_reg(), i, true);

// carry/borrow bit loop
#define RVV_VI_VV_LOOP_WITH_CARRY(BODY) \
  CHECK_NE(rvv_vd_reg(), 0);            \
  RVV_VI_GENERAL_LOOP_BASE              \
  RVV_VI_MASK_VARS                      \
  if (rvv_vsew() == E8) {               \
    VV_WITH_CARRY_PARAMS(8)             \
    BODY;                               \
  } else if (rvv_vsew() == E16) {       \
    VV_WITH_CARRY_PARAMS(16)            \
    BODY;                               \
  } else if (rvv_vsew() == E32) {       \
    VV_WITH_CARRY_PARAMS(32)            \
    BODY;                               \
  } else if (rvv_vsew() == E64) {       \
    VV_WITH_CARRY_PARAMS(64)            \
    BODY;                               \
  }                                     \
  RVV_VI_LOOP_END

#define RVV_VI_XI_LOOP_WITH_CARRY(BODY) \
  CHECK_NE(rvv_vd_reg(), 0);            \
  RVV_VI_GENERAL_LOOP_BASE              \
  RVV_VI_MASK_VARS                      \
  if (rvv_vsew() == E8) {               \
    XI_WITH_CARRY_PARAMS(8)             \
    BODY;                               \
  } else if (rvv_vsew() == E16) {       \
    XI_WITH_CARRY_PARAMS(16)            \
    BODY;                               \
  } else if (rvv_vsew() == E32) {       \
    XI_WITH_CARRY_PARAMS(32)            \
    BODY;                               \
  } else if (rvv_vsew() == E64) {       \
    XI_WITH_CARRY_PARAMS(64)            \
    BODY;                               \
  }                                     \
  RVV_VI_LOOP_END

#define VV_CMP_PARAMS(x)                                                   \
  type_sew_t<x>::type vs1 = Rvvelt<type_sew_t<x>::type>(rvv_vs1_reg(), i); \
  type_sew_t<x>::type vs2 = Rvvelt<type_sew_t<x>::type>(rvv_vs2_reg(), i);

#define VX_CMP_PARAMS(x)                                                    \
  type_sew_t<x>::type rs1 = (type_sew_t<x>::type)(get_register(rs1_reg())); \
  type_sew_t<x>::type vs2 = Rvvelt<type_sew_t<x>::type>(rvv_vs2_reg(), i);

#define VI_CMP_PARAMS(x)                                              \
  type_sew_t<x>::type simm5 = (type_sew_t<x>::type)instr_.RvvSimm5(); \
  type_sew_t<x>::type vs2 = Rvvelt<type_sew_t<x>::type>(rvv_vs2_reg(), i);

#define VV_UCMP_PARAMS(x)                                                    \
  type_usew_t<x>::type vs1 = Rvvelt<type_usew_t<x>::type>(rvv_vs1_reg(), i); \
  type_usew_t<x>::type vs2 = Rvvelt<type_usew_t<x>::type>(rvv_vs2_reg(), i);

#define VX_UCMP_PARAMS(x)                                 \
  type_usew_t<x>::type rs1 =                              \
      (type_sew_t<x>::type)(get_register(rvv_vs1_reg())); \
  type_usew_t<x>::type vs2 = Rvvelt<type_usew_t<x>::type>(rvv_vs2_reg(), i);

#define VI_UCMP_PARAMS(x)                                               \
  type_usew_t<x>::type uimm5 = (type_usew_t<x>::type)instr_.RvvUimm5(); \
  type_usew_t<x>::type vs2 = Rvvelt<type_usew_t<x>::type>(rvv_vs2_reg(), i);

#define RVV_VI_LOOP_CMP_BASE                                    \
  CHECK(rvv_vsew() >= E8 && rvv_vsew() <= E64);                 \
  for (reg_t i = rvv_vstart(); i < rvv_vl(); ++i) {             \
    RVV_VI_LOOP_MASK_SKIP();                                    \
    uint64_t mmask = uint64_t(1) << mpos;                       \
    uint64_t& vdi = Rvvelt<uint64_t>(rvv_vd_reg(), midx, true); \
    uint64_t res = 0;

#define RVV_VI_LOOP_CMP_END                         \
  vdi = (vdi & ~mmask) | (((res) << mpos) & mmask); \
  }                                                 \
  rvv_trace_vd();                                   \
  set_rvv_vstart(0);

// comparision result to masking register
#define RVV_VI_VV_LOOP_CMP(BODY)  \
  RVV_VI_LOOP_CMP_BASE            \
  if (rvv_vsew() == E8) {         \
    VV_CMP_PARAMS(8);             \
    BODY;                         \
  } else if (rvv_vsew() == E16) { \
    VV_CMP_PARAMS(16);            \
    BODY;                         \
  } else if (rvv_vsew() == E32) { \
    VV_CMP_PARAMS(32);            \
    BODY;                         \
  } else if (rvv_vsew() == E64) { \
    VV_CMP_PARAMS(64);            \
    BODY;                         \
  }                               \
  RVV_VI_LOOP_CMP_END

#define RVV_VI_VX_LOOP_CMP(BODY)  \
  RVV_VI_LOOP_CMP_BASE            \
  if (rvv_vsew() == E8) {         \
    VX_CMP_PARAMS(8);             \
    BODY;                         \
  } else if (rvv_vsew() == E16) { \
    VX_CMP_PARAMS(16);            \
    BODY;                         \
  } else if (rvv_vsew() == E32) { \
    VX_CMP_PARAMS(32);            \
    BODY;                         \
  } else if (rvv_vsew() == E64) { \
    VX_CMP_PARAMS(64);            \
    BODY;                         \
  }                               \
  RVV_VI_LOOP_CMP_END

#define RVV_VI_VI_LOOP_CMP(BODY)  \
  RVV_VI_LOOP_CMP_BASE            \
  if (rvv_vsew() == E8) {         \
    VI_CMP_PARAMS(8);             \
    BODY;                         \
  } else if (rvv_vsew() == E16) { \
    VI_CMP_PARAMS(16);            \
    BODY;                         \
  } else if (rvv_vsew() == E32) { \
    VI_CMP_PARAMS(32);            \
    BODY;                         \
  } else if (rvv_vsew() == E64) { \
    VI_CMP_PARAMS(64);            \
    BODY;                         \
  }                               \
  RVV_VI_LOOP_CMP_END

#define RVV_VI_VV_ULOOP_CMP(BODY) \
  RVV_VI_LOOP_CMP_BASE            \
  if (rvv_vsew() == E8) {         \
    VV_UCMP_PARAMS(8);            \
    BODY;                         \
  } else if (rvv_vsew() == E16) { \
    VV_UCMP_PARAMS(16);           \
    BODY;                         \
  } else if (rvv_vsew() == E32) { \
    VV_UCMP_PARAMS(32);           \
    BODY;                         \
  } else if (rvv_vsew() == E64) { \
    VV_UCMP_PARAMS(64);           \
    BODY;                         \
  }                               \
  RVV_VI_LOOP_CMP_END

#define RVV_VI_VX_ULOOP_CMP(BODY) \
  RVV_VI_LOOP_CMP_BASE            \
  if (rvv_vsew() == E8) {         \
    VX_UCMP_PARAMS(8);            \
    BODY;                         \
  } else if (rvv_vsew() == E16) { \
    VX_UCMP_PARAMS(16);           \
    BODY;                         \
  } else if (rvv_vsew() == E32) { \
    VX_UCMP_PARAMS(32);           \
    BODY;                         \
  } else if (rvv_vsew() == E64) { \
    VX_UCMP_PARAMS(64);           \
    BODY;                         \
  }                               \
  RVV_VI_LOOP_CMP_END

#define RVV_VI_VI_ULOOP_CMP(BODY) \
  RVV_VI_LOOP_CMP_BASE            \
  if (rvv_vsew() == E8) {         \
    VI_UCMP_PARAMS(8);            \
    BODY;                         \
  } else if (rvv_vsew() == E16) { \
    VI_UCMP_PARAMS(16);           \
    BODY;                         \
  } else if (rvv_vsew() == E32) { \
    VI_UCMP_PARAMS(32);           \
    BODY;                         \
  } else if (rvv_vsew() == E64) { \
    VI_UCMP_PARAMS(64);           \
    BODY;                         \
  }                               \
  RVV_VI_LOOP_CMP_END

#define RVV_VI_VFP_LOOP_BASE                           \
  for (uint64_t i = rvv_vstart(); i < rvv_vl(); ++i) { \
    RVV_VI_LOOP_MASK_SKIP();

#define RVV_VI_VFP_LOOP_END \
  }                         \
  set_rvv_vstart(0);

#define RVV_VI_VFP_VF_LOOP(BODY16, BODY32, BODY64)                   \
  RVV_VI_VFP_LOOP_BASE                                               \
  switch (rvv_vsew()) {                                              \
    case E16: {                                                      \
      UNIMPLEMENTED();                                               \
    }                                                                \
    case E32: {                                                      \
      float& vd = Rvvelt<float>(rvv_vd_reg(), i, true);              \
      float fs1 = static_cast<float>(get_fpu_register(rs1_reg()));   \
      float vs2 = Rvvelt<float>(rvv_vs2_reg(), i);                   \
      BODY32;                                                        \
      break;                                                         \
    }                                                                \
    case E64: {                                                      \
      double& vd = Rvvelt<double>(rvv_vd_reg(), i, true);            \
      double fs1 = static_cast<double>(get_fpu_register(rs1_reg())); \
      double vs2 = Rvvelt<double>(rvv_vs2_reg(), i);                 \
      BODY64;                                                        \
      break;                                                         \
    }                                                                \
    default:                                                         \
      UNREACHABLE();                                                 \
      break;                                                         \
  }                                                                  \
  RVV_VI_VFP_LOOP_END                                                \
  rvv_trace_vd();

#define RVV_VI_VFP_VV_LOOP(BODY16, BODY32, BODY64)        \
  RVV_VI_VFP_LOOP_BASE                                    \
  switch (rvv_vsew()) {                                   \
    case E16: {                                           \
      UNIMPLEMENTED();                                    \
      break;                                              \
    }                                                     \
    case E32: {                                           \
      float& vd = Rvvelt<float>(rvv_vd_reg(), i, true);   \
      float vs1 = Rvvelt<float>(rvv_vs1_reg(), i);        \
      float vs2 = Rvvelt<float>(rvv_vs2_reg(), i);        \
      BODY32;                                             \
      break;                                              \
    }                                                     \
    case E64: {                                           \
      double& vd = Rvvelt<double>(rvv_vd_reg(), i, true); \
      double vs1 = Rvvelt<double>(rvv_vs1_reg(), i);      \
      double vs2 = Rvvelt<double>(rvv_vs2_reg(), i);      \
      BODY64;                                             \
      break;                                              \
    }                                                     \
    default:                                              \
      require(0);                                         \
      break;                                              \
  }                                                       \
  RVV_VI_VFP_LOOP_END                                     \
  rvv_trace_vd();

#define RVV_VI_VFP_LOOP_CMP_BASE                                \
  for (reg_t i = rvv_vstart(); i < rvv_vl(); ++i) {             \
    RVV_VI_LOOP_MASK_SKIP();                                    \
    uint64_t mmask = uint64_t(1) << mpos;                       \
    uint64_t& vdi = Rvvelt<uint64_t>(rvv_vd_reg(), midx, true); \
    uint64_t res = 0;

#define RVV_VI_VFP_LOOP_CMP_END                         \
  switch (rvv_vsew()) {                                 \
    case E16:                                           \
    case E32:                                           \
    case E64: {                                         \
      vdi = (vdi & ~mmask) | (((res) << mpos) & mmask); \
      break;                                            \
    }                                                   \
    default:                                            \
      UNREACHABLE();                                    \
      break;                                            \
  }                                                     \
  }                                                     \
  set_rvv_vstart(0);                                    \
  rvv_trace_vd();

#define RVV_VI_VFP_LOOP_CMP(BODY16, BODY32, BODY64, is_vs1) \
  RVV_VI_VFP_LOOP_CMP_BASE                                  \
  switch (rvv_vsew()) {                                     \
    case E16: {                                             \
      UNIMPLEMENTED();                                      \
    }                                                       \
    case E32: {                                             \
      float vs2 = Rvvelt<float>(rvv_vs2_reg(), i);          \
      float vs1 = Rvvelt<float>(rvv_vs1_reg(), i);          \
      BODY32;                                               \
      break;                                                \
    }                                                       \
    case E64: {                                             \
      double vs2 = Rvvelt<double>(rvv_vs2_reg(), i);        \
      double vs1 = Rvvelt<double>(rvv_vs1_reg(), i);        \
      BODY64;                                               \
      break;                                                \
    }                                                       \
    default:                                                \
      UNREACHABLE();                                        \
      break;                                                \
  }                                                        \
  RVV_VI_VFP_LOOP_CMP_END

// reduction loop - signed
#define RVV_VI_LOOP_REDUCTION_BASE(x)                                  \
  auto& vd_0_des = Rvvelt<type_sew_t<x>::type>(rvv_vd_reg(), 0, true); \
  auto vd_0_res = Rvvelt<type_sew_t<x>::type>(rvv_vs1_reg(), 0);       \
  for (uint64_t i = rvv_vstart(); i < rvv_vl(); ++i) {                 \
    RVV_VI_LOOP_MASK_SKIP();                                           \
    auto vs2 = Rvvelt<type_sew_t<x>::type>(rvv_vs2_reg(), i);

#define RVV_VI_LOOP_REDUCTION_END(x) \
  }                                  \
  if (rvv_vl() > 0) {                \
    vd_0_des = vd_0_res;             \
  }                                  \
  set_rvv_vstart(0);

#define REDUCTION_LOOP(x, BODY) \
  RVV_VI_LOOP_REDUCTION_BASE(x) \
  BODY;                         \
  RVV_VI_LOOP_REDUCTION_END(x)

#define RVV_VI_VV_LOOP_REDUCTION(BODY) \
  if (rvv_vsew() == E8) {              \
    REDUCTION_LOOP(8, BODY)            \
  } else if (rvv_vsew() == E16) {      \
    REDUCTION_LOOP(16, BODY)           \
  } else if (rvv_vsew() == E32) {      \
    REDUCTION_LOOP(32, BODY)           \
  } else if (rvv_vsew() == E64) {      \
    REDUCTION_LOOP(64, BODY)           \
  }                                    \
  rvv_trace_vd();

// reduction loop - unsgied
#define RVV_VI_ULOOP_REDUCTION_BASE(x)                                  \
  auto& vd_0_des = Rvvelt<type_usew_t<x>::type>(rvv_vd_reg(), 0, true); \
  auto vd_0_res = Rvvelt<type_usew_t<x>::type>(rvv_vs1_reg(), 0);       \
  for (reg_t i = rvv_vstart(); i < rvv_vl(); ++i) {                     \
    RVV_VI_LOOP_MASK_SKIP();                                            \
    auto vs2 = Rvvelt<type_usew_t<x>::type>(rvv_vs2_reg(), i);

#define REDUCTION_ULOOP(x, BODY) \
  RVV_VI_ULOOP_REDUCTION_BASE(x) \
  BODY;                          \
  RVV_VI_LOOP_REDUCTION_END(x)

#define RVV_VI_VV_ULOOP_REDUCTION(BODY) \
  if (rvv_vsew() == E8) {               \
    REDUCTION_ULOOP(8, BODY)            \
  } else if (rvv_vsew() == E16) {       \
    REDUCTION_ULOOP(16, BODY)           \
  } else if (rvv_vsew() == E32) {       \
    REDUCTION_ULOOP(32, BODY)           \
  } else if (rvv_vsew() == E64) {       \
    REDUCTION_ULOOP(64, BODY)           \
  }                                     \
  rvv_trace_vd();

#define VI_STRIP(inx) reg_t vreg_inx = inx;

#define VI_ELEMENT_SKIP(inx)       \
  if (inx >= vl) {                 \
    continue;                      \
  } else if (inx < rvv_vstart()) { \
    continue;                      \
  } else {                         \
    RVV_VI_LOOP_MASK_SKIP();       \
  }

#define require_vm                                      \
  do {                                                  \
    if (instr_.RvvVM() == 0) CHECK_NE(rvv_vd_reg(), 0); \
  } while (0);

#define VI_CHECK_STORE(elt_width, is_mask_ldst) \
  reg_t veew = is_mask_ldst ? 1 : sizeof(elt_width##_t) * 8;
// float vemul = is_mask_ldst ? 1 : ((float)veew / rvv_vsew() * Rvvvflmul);
// reg_t emul = vemul < 1 ? 1 : vemul;
// require(vemul >= 0.125 && vemul <= 8);
// require_align(rvv_rd(), vemul);
// require((nf * emul) <= (NVPR / 4) && (rvv_rd() + nf * emul) <= NVPR);

#define VI_CHECK_LOAD(elt_width, is_mask_ldst) \
  VI_CHECK_STORE(elt_width, is_mask_ldst);     \
  require_vm;

/*vd + fn * emul*/
#define RVV_VI_LD(stride, offset, elt_width, is_mask_ldst)                     \
  const reg_t nf = rvv_nf() + 1;                                               \
  const reg_t vl = is_mask_ldst ? ((rvv_vl() + 7) / 8) : rvv_vl();             \
  const int64_t baseAddr = rs1();                                              \
  for (reg_t i = 0; i < vl; ++i) {                                             \
    VI_ELEMENT_SKIP(i);                                                        \
    VI_STRIP(i);                                                               \
    set_rvv_vstart(i);                                                         \
    for (reg_t fn = 0; fn < nf; ++fn) {                                        \
      auto val = ReadMem<elt_width##_t>(                                       \
          baseAddr + (stride) + (offset) * sizeof(elt_width##_t),              \
          instr_.instr());                                                     \
      type_sew_t<sizeof(elt_width##_t)* 8>::type& vd =                         \
          Rvvelt<type_sew_t<sizeof(elt_width##_t) * 8>::type>(rvv_vd_reg(),    \
                                                              vreg_inx, true); \
      vd = val;                                                                \
    }                                                                          \
  }                                                                            \
  set_rvv_vstart(0);                                                           \
  if (::v8::internal::FLAG_trace_sim) {                                        \
    __int128_t value = Vregister_[rvv_vd_reg()];                               \
    SNPrintF(trace_buf_, "0x%016" PRIx64 "%016" PRIx64 " <-- 0x%016" PRIx64,   \
             *(reinterpret_cast<int64_t*>(&value) + 1),                        \
             *reinterpret_cast<int64_t*>(&value),                              \
             (uint64_t)(get_register(rs1_reg())));                             \
  }

#define RVV_VI_ST(stride, offset, elt_width, is_mask_ldst)                     \
  const reg_t nf = rvv_nf() + 1;                                               \
  const reg_t vl = is_mask_ldst ? ((rvv_vl() + 7) / 8) : rvv_vl();             \
  const int64_t baseAddr = rs1();                                              \
  for (reg_t i = 0; i < vl; ++i) {                                             \
    VI_STRIP(i)                                                                \
    VI_ELEMENT_SKIP(i);                                                        \
    set_rvv_vstart(i);                                                         \
    for (reg_t fn = 0; fn < nf; ++fn) {                                        \
      elt_width##_t vs1 = Rvvelt<type_sew_t<sizeof(elt_width##_t) * 8>::type>( \
          rvv_vs3_reg(), vreg_inx);                                            \
      WriteMem(baseAddr + (stride) + (offset) * sizeof(elt_width##_t), vs1,    \
               instr_.instr());                                                \
    }                                                                          \
  }                                                                            \
  set_rvv_vstart(0);                                                           \
  if (::v8::internal::FLAG_trace_sim) {                                        \
    __int128_t value = Vregister_[rvv_vd_reg()];                               \
    SNPrintF(trace_buf_, "0x%016" PRIx64 "%016" PRIx64 " --> 0x%016" PRIx64,   \
             *(reinterpret_cast<int64_t*>(&value) + 1),                        \
             *reinterpret_cast<int64_t*>(&value),                              \
             (uint64_t)(get_register(rs1_reg())));                             \
  }

#define VI_VFP_LOOP_SCALE_BASE                      \
  /*require(STATE.frm < 0x5);*/                     \
  for (reg_t i = rvv_vstart(); i < rvv_vl(); ++i) { \
    RVV_VI_LOOP_MASK_SKIP();

#define RVV_VI_VFP_CVT_SCALE(BODY8, BODY16, BODY32, CHECK8, CHECK16, CHECK32, \
                             is_widen, eew_check)                             \
  CHECK(eew_check);                                                           \
  switch (rvv_vsew()) {                                                       \
    case E8: {                                                                \
      CHECK8                                                                  \
      VI_VFP_LOOP_SCALE_BASE                                                  \
      BODY8 /*set_fp_exceptions*/;                                            \
      RVV_VI_VFP_LOOP_END                                                     \
    } break;                                                                  \
    case E16: {                                                               \
      CHECK16                                                                 \
      VI_VFP_LOOP_SCALE_BASE                                                  \
      BODY16 /*set_fp_exceptions*/;                                           \
      RVV_VI_VFP_LOOP_END                                                     \
    } break;                                                                  \
    case E32: {                                                               \
      CHECK32                                                                 \
      VI_VFP_LOOP_SCALE_BASE                                                  \
      BODY32 /*set_fp_exceptions*/;                                           \
      RVV_VI_VFP_LOOP_END                                                     \
    } break;                                                                  \
    default:                                                                  \
      require(0);                                                             \
      break;                                                                  \
  }                                                                           \
  rvv_trace_vd();

namespace v8 {
namespace internal {

DEFINE_LAZY_LEAKY_OBJECT_GETTER(Simulator::GlobalMonitor,
                                Simulator::GlobalMonitor::Get)

// Util functions.
inline bool HaveSameSign(int64_t a, int64_t b) { return ((a ^ b) >= 0); }

uint32_t get_fcsr_condition_bit(uint32_t cc) {
  if (cc == 0) {
    return 23;
  } else {
    return 24 + cc;
  }
}

// Generated by Assembler::break_()/stop(), ebreak code is passed as immediate
// field of a subsequent LUI instruction; otherwise returns -1
static inline int32_t get_ebreak_code(Instruction* instr) {
  DCHECK(instr->InstructionBits() == kBreakInstr);
  byte* cur = reinterpret_cast<byte*>(instr);
  Instruction* next_instr = reinterpret_cast<Instruction*>(cur + kInstrSize);
  if (next_instr->BaseOpcodeFieldRaw() == RO_LUI)
    return (next_instr->Imm20UValue());
  else
    return -1;
}

// This macro provides a platform independent use of sscanf. The reason for
// SScanF not being implemented in a platform independent was through
// ::v8::internal::OS in the same way as SNPrintF is that the Windows C Run-Time
// Library does not provide vsscanf.
#define SScanF sscanf

// The RiscvDebugger class is used by the simulator while debugging simulated
// code.
class RiscvDebugger {
 public:
  explicit RiscvDebugger(Simulator* sim) : sim_(sim) {}

  void Debug();
  // Print all registers with a nice formatting.
  void PrintRegs(char name_prefix, int start_index, int end_index);
  void PrintAllRegs();
  void PrintAllRegsIncludingFPU();

  static const Instr kNopInstr = 0x0;

 private:
  Simulator* sim_;

  int64_t GetRegisterValue(int regnum);
  int64_t GetFPURegisterValue(int regnum);
  float GetFPURegisterValueFloat(int regnum);
  double GetFPURegisterValueDouble(int regnum);
  __int128_t GetVRegisterValue(int regnum);
  bool GetValue(const char* desc, int64_t* value);
};

#define UNSUPPORTED()                                                     \
  printf("Sim: Unsupported instruction. Func:%s Line:%d\n", __FUNCTION__, \
         __LINE__);                                                       \
  base::OS::Abort();

int64_t RiscvDebugger::GetRegisterValue(int regnum) {
  if (regnum == kNumSimuRegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_register(regnum);
  }
}

int64_t RiscvDebugger::GetFPURegisterValue(int regnum) {
  if (regnum == kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_fpu_register(regnum);
  }
}

float RiscvDebugger::GetFPURegisterValueFloat(int regnum) {
  if (regnum == kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_fpu_register_float(regnum);
  }
}

double RiscvDebugger::GetFPURegisterValueDouble(int regnum) {
  if (regnum == kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_fpu_register_double(regnum);
  }
}

__int128_t RiscvDebugger::GetVRegisterValue(int regnum) {
  if (regnum == kNumVRegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_vregister(regnum);
  }
}

bool RiscvDebugger::GetValue(const char* desc, int64_t* value) {
  int regnum = Registers::Number(desc);
  int fpuregnum = FPURegisters::Number(desc);

  if (regnum != kInvalidRegister) {
    *value = GetRegisterValue(regnum);
    return true;
  } else if (fpuregnum != kInvalidFPURegister) {
    *value = GetFPURegisterValue(fpuregnum);
    return true;
  } else if (strncmp(desc, "0x", 2) == 0) {
    return SScanF(desc + 2, "%" SCNx64, reinterpret_cast<uint64_t*>(value)) ==
           1;
  } else {
    return SScanF(desc, "%" SCNu64, reinterpret_cast<uint64_t*>(value)) == 1;
  }
}

#define REG_INFO(name)                             \
  name, GetRegisterValue(Registers::Number(name)), \
      GetRegisterValue(Registers::Number(name))

void RiscvDebugger::PrintRegs(char name_prefix, int start_index,
                              int end_index) {
  base::EmbeddedVector<char, 10> name1, name2;
  DCHECK(name_prefix == 'a' || name_prefix == 't' || name_prefix == 's');
  DCHECK(start_index >= 0 && end_index <= 99);
  int num_registers = (end_index - start_index) + 1;
  for (int i = 0; i < num_registers / 2; i++) {
    SNPrintF(name1, "%c%d", name_prefix, start_index + 2 * i);
    SNPrintF(name2, "%c%d", name_prefix, start_index + 2 * i + 1);
    PrintF("%3s: 0x%016" PRIx64 "  %14" PRId64 " \t%3s: 0x%016" PRIx64
           "  %14" PRId64 " \n",
           REG_INFO(name1.begin()), REG_INFO(name2.begin()));
  }
  if (num_registers % 2 == 1) {
    SNPrintF(name1, "%c%d", name_prefix, end_index);
    PrintF("%3s: 0x%016" PRIx64 "  %14" PRId64 " \n", REG_INFO(name1.begin()));
  }
}

void RiscvDebugger::PrintAllRegs() {
  PrintF("\n");
  // ra, sp, gp
  PrintF("%3s: 0x%016" PRIx64 " %14" PRId64 "\t%3s: 0x%016" PRIx64 " %14" PRId64
         "\t%3s: 0x%016" PRIx64 " %14" PRId64 "\n",
         REG_INFO("ra"), REG_INFO("sp"), REG_INFO("gp"));

  // tp, fp, pc
  PrintF("%3s: 0x%016" PRIx64 " %14" PRId64 "\t%3s: 0x%016" PRIx64 " %14" PRId64
         "\t%3s: 0x%016" PRIx64 " %14" PRId64 "\n",
         REG_INFO("tp"), REG_INFO("fp"), REG_INFO("pc"));

  // print register a0, .., a7
  PrintRegs('a', 0, 7);
  // print registers s1, ..., s11
  PrintRegs('s', 1, 11);
  // print registers t0, ..., t6
  PrintRegs('t', 0, 6);
}

#undef REG_INFO

void RiscvDebugger::PrintAllRegsIncludingFPU() {
#define FPU_REG_INFO(n) \
  FPURegisters::Name(n), GetFPURegisterValue(n), GetFPURegisterValueDouble(n)

  PrintAllRegs();

  PrintF("\n\n");
  // f0, f1, f2, ... f31.
  DCHECK_EQ(kNumFPURegisters % 2, 0);
  for (int i = 0; i < kNumFPURegisters; i += 2)
    PrintF("%3s: 0x%016" PRIx64 "  %16.4e \t%3s: 0x%016" PRIx64 "  %16.4e\n",
           FPU_REG_INFO(i), FPU_REG_INFO(i + 1));
#undef FPU_REG_INFO
}

void RiscvDebugger::Debug() {
  intptr_t last_pc = -1;
  bool done = false;

#define COMMAND_SIZE 63
#define ARG_SIZE 255

#define STR(a) #a
#define XSTR(a) STR(a)

  char cmd[COMMAND_SIZE + 1];
  char arg1[ARG_SIZE + 1];
  char arg2[ARG_SIZE + 1];
  char* argv[3] = {cmd, arg1, arg2};

  // Make sure to have a proper terminating character if reaching the limit.
  cmd[COMMAND_SIZE] = 0;
  arg1[ARG_SIZE] = 0;
  arg2[ARG_SIZE] = 0;

  while (!done && (sim_->get_pc() != Simulator::end_sim_pc)) {
    if (last_pc != sim_->get_pc()) {
      disasm::NameConverter converter;
      disasm::Disassembler dasm(converter);
      // Use a reasonably large buffer.
      v8::base::EmbeddedVector<char, 256> buffer;
      const char* name = sim_->builtins_.Lookup((Address)sim_->get_pc());
      if (name != nullptr) {
        PrintF("Call builtin:  %s\n", name);
      }
      dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(sim_->get_pc()));
      PrintF("  0x%016" PRIx64 "   %s\n", sim_->get_pc(), buffer.begin());
      last_pc = sim_->get_pc();
    }
    char* line = ReadLine("sim> ");
    if (line == nullptr) {
      break;
    } else {
      char* last_input = sim_->last_debugger_input();
      if (strcmp(line, "\n") == 0 && last_input != nullptr) {
        line = last_input;
      } else {
        // Ownership is transferred to sim_;
        sim_->set_last_debugger_input(line);
      }
      // Use sscanf to parse the individual parts of the command line. At the
      // moment no command expects more than two parameters.
      int argc = SScanF(
            line,
            "%" XSTR(COMMAND_SIZE) "s "
            "%" XSTR(ARG_SIZE) "s "
            "%" XSTR(ARG_SIZE) "s",
            cmd, arg1, arg2);
      if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
        Instruction* instr = reinterpret_cast<Instruction*>(sim_->get_pc());
        if (!(instr->IsTrap()) ||
            instr->InstructionBits() == rtCallRedirInstr) {
          sim_->InstructionDecode(
              reinterpret_cast<Instruction*>(sim_->get_pc()));
        } else {
          // Allow si to jump over generated breakpoints.
          PrintF("/!\\ Jumping over generated breakpoint.\n");
          sim_->set_pc(sim_->get_pc() + kInstrSize);
        }
      } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
        // Execute the one instruction we broke at with breakpoints disabled.
        sim_->InstructionDecode(reinterpret_cast<Instruction*>(sim_->get_pc()));
        // Leave the debugger shell.
        done = true;
      } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
        if (argc == 2) {
          int64_t value;
          double dvalue;
          if (strcmp(arg1, "all") == 0) {
            PrintAllRegs();
          } else if (strcmp(arg1, "allf") == 0) {
            PrintAllRegsIncludingFPU();
          } else {
            int regnum = Registers::Number(arg1);
            int fpuregnum = FPURegisters::Number(arg1);
            int vregnum = VRegisters::Number(arg1);

            if (regnum != kInvalidRegister) {
              value = GetRegisterValue(regnum);
              PrintF("%s: 0x%08" PRIx64 "  %" PRId64 "  \n", arg1, value,
                     value);
            } else if (fpuregnum != kInvalidFPURegister) {
              value = GetFPURegisterValue(fpuregnum);
              dvalue = GetFPURegisterValueDouble(fpuregnum);
              PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n",
                     FPURegisters::Name(fpuregnum), value, dvalue);
            } else if (vregnum != kInvalidVRegister) {
              __int128_t v = GetVRegisterValue(vregnum);
              PrintF("\t%s:0x%016" PRIx64 "%016" PRIx64 "\n",
                     VRegisters::Name(vregnum), (uint64_t)(v >> 64),
                     (uint64_t)v);
            } else {
              PrintF("%s unrecognized\n", arg1);
            }
          }
        } else {
          if (argc == 3) {
            if (strcmp(arg2, "single") == 0) {
              int64_t value;
              float fvalue;
              int fpuregnum = FPURegisters::Number(arg1);

              if (fpuregnum != kInvalidFPURegister) {
                value = GetFPURegisterValue(fpuregnum);
                value &= 0xFFFFFFFFUL;
                fvalue = GetFPURegisterValueFloat(fpuregnum);
                PrintF("%s: 0x%08" PRIx64 "  %11.4e\n", arg1, value, fvalue);
              } else {
                PrintF("%s unrecognized\n", arg1);
              }
            } else {
              PrintF("print <fpu register> single\n");
            }
          } else {
            PrintF("print <register> or print <fpu register> single\n");
          }
        }
      } else if ((strcmp(cmd, "po") == 0) ||
                 (strcmp(cmd, "printobject") == 0)) {
        if (argc == 2) {
          int64_t value;
          StdoutStream os;
          if (GetValue(arg1, &value)) {
            Object obj(value);
            os << arg1 << ": \n";
#ifdef DEBUG
            obj.Print(os);
            os << "\n";
#else
            os << Brief(obj) << "\n";
#endif
          } else {
            os << arg1 << " unrecognized\n";
          }
        } else {
          PrintF("printobject <value>\n");
        }
      } else if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "mem") == 0) {
        int64_t* cur = nullptr;
        int64_t* end = nullptr;
        int next_arg = 1;

        if (strcmp(cmd, "stack") == 0) {
          cur = reinterpret_cast<int64_t*>(sim_->get_register(Simulator::sp));
        } else {  // Command "mem".
          if (argc < 2) {
            PrintF("Need to specify <address> to mem command\n");
            continue;
          }
          int64_t value;
          if (!GetValue(arg1, &value)) {
            PrintF("%s unrecognized\n", arg1);
            continue;
          }
          cur = reinterpret_cast<int64_t*>(value);
          next_arg++;
        }

        int64_t words;
        if (argc == next_arg) {
          words = 10;
        } else {
          if (!GetValue(argv[next_arg], &words)) {
            words = 10;
          }
        }
        end = cur + words;

        while (cur < end) {
          PrintF("  0x%012" PRIxPTR " :  0x%016" PRIx64 "  %14" PRId64 " ",
                 reinterpret_cast<intptr_t>(cur), *cur, *cur);
          Object obj(*cur);
          Heap* current_heap = sim_->isolate_->heap();
          if (obj.IsSmi() ||
              IsValidHeapObject(current_heap, HeapObject::cast(obj))) {
            PrintF(" (");
            if (obj.IsSmi()) {
              PrintF("smi %d", Smi::ToInt(obj));
            } else {
              obj.ShortPrint();
            }
            PrintF(")");
          }
          PrintF("\n");
          cur++;
        }

      } else if ((strcmp(cmd, "disasm") == 0) || (strcmp(cmd, "dpc") == 0) ||
                 (strcmp(cmd, "di") == 0)) {
        disasm::NameConverter converter;
        disasm::Disassembler dasm(converter);
        // Use a reasonably large buffer.
        v8::base::EmbeddedVector<char, 256> buffer;

        byte* cur = nullptr;
        byte* end = nullptr;

        if (argc == 1) {
          cur = reinterpret_cast<byte*>(sim_->get_pc());
          end = cur + (10 * kInstrSize);
        } else if (argc == 2) {
          int regnum = Registers::Number(arg1);
          if (regnum != kInvalidRegister || strncmp(arg1, "0x", 2) == 0) {
            // The argument is an address or a register name.
            int64_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(value);
              // Disassemble 10 instructions at <arg1>.
              end = cur + (10 * kInstrSize);
            }
          } else {
            // The argument is the number of instructions.
            int64_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(sim_->get_pc());
              // Disassemble <arg1> instructions.
              end = cur + (value * kInstrSize);
            }
          }
        } else {
          int64_t value1;
          int64_t value2;
          if (GetValue(arg1, &value1) && GetValue(arg2, &value2)) {
            cur = reinterpret_cast<byte*>(value1);
            end = cur + (value2 * kInstrSize);
          }
        }

        while (cur < end) {
          dasm.InstructionDecode(buffer, cur);
          PrintF("  0x%08" PRIxPTR "   %s\n", reinterpret_cast<intptr_t>(cur),
                 buffer.begin());
          cur += kInstrSize;
        }
      } else if (strcmp(cmd, "gdb") == 0) {
        PrintF("relinquishing control to gdb\n");
        v8::base::OS::DebugBreak();
        PrintF("regaining control from gdb\n");
      } else if (strcmp(cmd, "break") == 0 || strcmp(cmd, "b") == 0 ||
                 strcmp(cmd, "tbreak") == 0) {
        bool is_tbreak = strcmp(cmd, "tbreak") == 0;
        if (argc == 2) {
          int64_t value;
          if (GetValue(arg1, &value)) {
            sim_->SetBreakpoint(reinterpret_cast<Instruction*>(value),
                                is_tbreak);
          } else {
            PrintF("%s unrecognized\n", arg1);
          }
        } else {
          sim_->ListBreakpoints();
          PrintF("Use `break <address>` to set or disable a breakpoint\n");
          PrintF(
              "Use `tbreak <address>` to set or disable a temporary "
              "breakpoint\n");
        }
      } else if (strcmp(cmd, "flags") == 0) {
        PrintF("No flags on RISC-V !\n");
      } else if (strcmp(cmd, "stop") == 0) {
        int64_t value;
        if (argc == 3) {
          // Print information about all/the specified breakpoint(s).
          if (strcmp(arg1, "info") == 0) {
            if (strcmp(arg2, "all") == 0) {
              PrintF("Stop information:\n");
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->PrintStopInfo(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->PrintStopInfo(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "enable") == 0) {
            // Enable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->EnableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->EnableStop(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "disable") == 0) {
            // Disable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->DisableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->DisableStop(value);
            } else {
              PrintF("Unrecognized argument.\n");
            }
          }
        } else {
          PrintF("Wrong usage. Use help command for more information.\n");
        }
      } else if ((strcmp(cmd, "stat") == 0) || (strcmp(cmd, "st") == 0)) {
        // Print registers and disassemble.
        PrintAllRegs();
        PrintF("\n");

        disasm::NameConverter converter;
        disasm::Disassembler dasm(converter);
        // Use a reasonably large buffer.
        v8::base::EmbeddedVector<char, 256> buffer;

        byte* cur = nullptr;
        byte* end = nullptr;

        if (argc == 1) {
          cur = reinterpret_cast<byte*>(sim_->get_pc());
          end = cur + (10 * kInstrSize);
        } else if (argc == 2) {
          int64_t value;
          if (GetValue(arg1, &value)) {
            cur = reinterpret_cast<byte*>(value);
            // no length parameter passed, assume 10 instructions
            end = cur + (10 * kInstrSize);
          }
        } else {
          int64_t value1;
          int64_t value2;
          if (GetValue(arg1, &value1) && GetValue(arg2, &value2)) {
            cur = reinterpret_cast<byte*>(value1);
            end = cur + (value2 * kInstrSize);
          }
        }

        while (cur < end) {
          dasm.InstructionDecode(buffer, cur);
          PrintF("  0x%08" PRIxPTR "   %s\n", reinterpret_cast<intptr_t>(cur),
                 buffer.begin());
          cur += kInstrSize;
        }
      } else if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
        PrintF("cont (alias 'c')\n");
        PrintF("  Continue execution\n");
        PrintF("stepi (alias 'si')\n");
        PrintF("  Step one instruction\n");
        PrintF("print (alias 'p')\n");
        PrintF("  print <register>\n");
        PrintF("  Print register content\n");
        PrintF("  Use register name 'all' to print all GPRs\n");
        PrintF("  Use register name 'allf' to print all GPRs and FPRs\n");
        PrintF("printobject (alias 'po')\n");
        PrintF("  printobject <register>\n");
        PrintF("  Print an object from a register\n");
        PrintF("stack\n");
        PrintF("  stack [<words>]\n");
        PrintF("  Dump stack content, default dump 10 words)\n");
        PrintF("mem\n");
        PrintF("  mem <address> [<words>]\n");
        PrintF("  Dump memory content, default dump 10 words)\n");
        PrintF("flags\n");
        PrintF("  print flags\n");
        PrintF("disasm (alias 'di')\n");
        PrintF("  disasm [<instructions>]\n");
        PrintF("  disasm [<address/register>] (e.g., disasm pc) \n");
        PrintF("  disasm [[<address/register>] <instructions>]\n");
        PrintF("  Disassemble code, default is 10 instructions\n");
        PrintF("  from pc\n");
        PrintF("gdb \n");
        PrintF("  Return to gdb if the simulator was started with gdb\n");
        PrintF("break (alias 'b')\n");
        PrintF("  break : list all breakpoints\n");
        PrintF("  break <address> : set / enable / disable a breakpoint.\n");
        PrintF("tbreak\n");
        PrintF("  tbreak : list all breakpoints\n");
        PrintF(
            "  tbreak <address> : set / enable / disable a temporary "
            "breakpoint.\n");
        PrintF("  Set a breakpoint enabled only for one stop. \n");
        PrintF("stop feature:\n");
        PrintF("  Description:\n");
        PrintF("    Stops are debug instructions inserted by\n");
        PrintF("    the Assembler::stop() function.\n");
        PrintF("    When hitting a stop, the Simulator will\n");
        PrintF("    stop and give control to the Debugger.\n");
        PrintF("    All stop codes are watched:\n");
        PrintF("    - They can be enabled / disabled: the Simulator\n");
        PrintF("       will / won't stop when hitting them.\n");
        PrintF("    - The Simulator keeps track of how many times they \n");
        PrintF("      are met. (See the info command.) Going over a\n");
        PrintF("      disabled stop still increases its counter. \n");
        PrintF("  Commands:\n");
        PrintF("    stop info all/<code> : print infos about number <code>\n");
        PrintF("      or all stop(s).\n");
        PrintF("    stop enable/disable all/<code> : enables / disables\n");
        PrintF("      all or number <code> stop(s)\n");
      } else {
        PrintF("Unknown command: %s\n", cmd);
      }
    }
  }

#undef COMMAND_SIZE
#undef ARG_SIZE

#undef STR
#undef XSTR
}

void Simulator::SetBreakpoint(Instruction* location, bool is_tbreak) {
  for (unsigned i = 0; i < breakpoints_.size(); i++) {
    if (breakpoints_.at(i).location == location) {
      if (breakpoints_.at(i).is_tbreak != is_tbreak) {
        PrintF("Change breakpoint at %p to %s breakpoint\n",
               reinterpret_cast<void*>(location),
               is_tbreak ? "temporary" : "regular");
        breakpoints_.at(i).is_tbreak = is_tbreak;
        return;
      }
      PrintF("Existing breakpoint at %p was %s\n",
             reinterpret_cast<void*>(location),
             breakpoints_.at(i).enabled ? "disabled" : "enabled");
      breakpoints_.at(i).enabled = !breakpoints_.at(i).enabled;
      return;
    }
  }
  Breakpoint new_breakpoint = {location, true, is_tbreak};
  breakpoints_.push_back(new_breakpoint);
  PrintF("Set a %sbreakpoint at %p\n", is_tbreak ? "temporary " : "",
         reinterpret_cast<void*>(location));
}

void Simulator::ListBreakpoints() {
  PrintF("Breakpoints:\n");
  for (unsigned i = 0; i < breakpoints_.size(); i++) {
    PrintF("%p  : %s %s\n",
           reinterpret_cast<void*>(breakpoints_.at(i).location),
           breakpoints_.at(i).enabled ? "enabled" : "disabled",
           breakpoints_.at(i).is_tbreak ? ": temporary" : "");
  }
}

void Simulator::CheckBreakpoints() {
  bool hit_a_breakpoint = false;
  bool is_tbreak = false;
  Instruction* pc_ = reinterpret_cast<Instruction*>(get_pc());
  for (unsigned i = 0; i < breakpoints_.size(); i++) {
    if ((breakpoints_.at(i).location == pc_) && breakpoints_.at(i).enabled) {
      hit_a_breakpoint = true;
      if (breakpoints_.at(i).is_tbreak) {
        // Disable a temporary breakpoint.
        is_tbreak = true;
        breakpoints_.at(i).enabled = false;
      }
      break;
    }
  }
  if (hit_a_breakpoint) {
    PrintF("Hit %sa breakpoint at %p.\n", is_tbreak ? "and disabled " : "",
           reinterpret_cast<void*>(pc_));
    RiscvDebugger dbg(this);
    dbg.Debug();
  }
}

bool Simulator::ICacheMatch(void* one, void* two) {
  DCHECK_EQ(reinterpret_cast<intptr_t>(one) & CachePage::kPageMask, 0);
  DCHECK_EQ(reinterpret_cast<intptr_t>(two) & CachePage::kPageMask, 0);
  return one == two;
}

static uint32_t ICacheHash(void* key) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)) >> 2;
}

static bool AllOnOnePage(uintptr_t start, size_t size) {
  intptr_t start_page = (start & ~CachePage::kPageMask);
  intptr_t end_page = ((start + size) & ~CachePage::kPageMask);
  return start_page == end_page;
}

void Simulator::set_last_debugger_input(char* input) {
  DeleteArray(last_debugger_input_);
  last_debugger_input_ = input;
}

void Simulator::SetRedirectInstruction(Instruction* instruction) {
  instruction->SetInstructionBits(rtCallRedirInstr);
}

void Simulator::FlushICache(base::CustomMatcherHashMap* i_cache,
                            void* start_addr, size_t size) {
  int64_t start = reinterpret_cast<int64_t>(start_addr);
  int64_t intra_line = (start & CachePage::kLineMask);
  start -= intra_line;
  size += intra_line;
  size = ((size - 1) | CachePage::kLineMask) + 1;
  int offset = (start & CachePage::kPageMask);
  while (!AllOnOnePage(start, size - 1)) {
    int bytes_to_flush = CachePage::kPageSize - offset;
    FlushOnePage(i_cache, start, bytes_to_flush);
    start += bytes_to_flush;
    size -= bytes_to_flush;
    DCHECK_EQ((int64_t)0, start & CachePage::kPageMask);
    offset = 0;
  }
  if (size != 0) {
    FlushOnePage(i_cache, start, size);
  }
}

CachePage* Simulator::GetCachePage(base::CustomMatcherHashMap* i_cache,
                                   void* page) {
  base::HashMap::Entry* entry = i_cache->LookupOrInsert(page, ICacheHash(page));
  if (entry->value == nullptr) {
    CachePage* new_page = new CachePage();
    entry->value = new_page;
  }
  return reinterpret_cast<CachePage*>(entry->value);
}

// Flush from start up to and not including start + size.
void Simulator::FlushOnePage(base::CustomMatcherHashMap* i_cache,
                             intptr_t start, size_t size) {
  DCHECK_LE(size, CachePage::kPageSize);
  DCHECK(AllOnOnePage(start, size - 1));
  DCHECK_EQ(start & CachePage::kLineMask, 0);
  DCHECK_EQ(size & CachePage::kLineMask, 0);
  void* page = reinterpret_cast<void*>(start & (~CachePage::kPageMask));
  int offset = (start & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* valid_bytemap = cache_page->ValidityByte(offset);
  memset(valid_bytemap, CachePage::LINE_INVALID, size >> CachePage::kLineShift);
}

void Simulator::CheckICache(base::CustomMatcherHashMap* i_cache,
                            Instruction* instr) {
  int64_t address = reinterpret_cast<int64_t>(instr);
  void* page = reinterpret_cast<void*>(address & (~CachePage::kPageMask));
  void* line = reinterpret_cast<void*>(address & (~CachePage::kLineMask));
  int offset = (address & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* cache_valid_byte = cache_page->ValidityByte(offset);
  bool cache_hit = (*cache_valid_byte == CachePage::LINE_VALID);
  char* cached_line = cache_page->CachedData(offset & ~CachePage::kLineMask);
  if (cache_hit) {
    // Check that the data in memory matches the contents of the I-cache.
    CHECK_EQ(0, memcmp(reinterpret_cast<void*>(instr),
                       cache_page->CachedData(offset), kInstrSize));
  } else {
    // Cache miss.  Load memory into the cache.
    memcpy(cached_line, line, CachePage::kLineLength);
    *cache_valid_byte = CachePage::LINE_VALID;
  }
}

Simulator::Simulator(Isolate* isolate) : isolate_(isolate), builtins_(isolate) {
  // Set up simulator support first. Some of this information is needed to
  // setup the architecture state.
  stack_size_ = FLAG_sim_stack_size * KB;
  stack_ = reinterpret_cast<char*>(malloc(stack_size_));
  pc_modified_ = false;
  icount_ = 0;
  break_count_ = 0;
  // Reset debug helpers.
  breakpoints_.clear();
  // TODO(riscv): 'next' command
  // break_on_next_ = false;

  // Set up architecture state.
  // All registers are initialized to zero to start with.
  for (int i = 0; i < kNumSimuRegisters; i++) {
    registers_[i] = 0;
  }

  for (int i = 0; i < kNumFPURegisters; i++) {
    FPUregisters_[i] = 0;
  }

  FCSR_ = 0;

  // The sp is initialized to point to the bottom (high address) of the
  // allocated stack area. To be safe in potential stack underflows we leave
  // some buffer below.
  registers_[sp] = reinterpret_cast<int64_t>(stack_) + stack_size_ - 64;
  // The ra and pc are initialized to a known bad value that will cause an
  // access violation if the simulator ever tries to execute it.
  registers_[pc] = bad_ra;
  registers_[ra] = bad_ra;

  last_debugger_input_ = nullptr;
}

Simulator::~Simulator() {
  GlobalMonitor::Get()->RemoveLinkedAddress(&global_monitor_thread_);
  free(stack_);
}

// Get the active Simulator for the current thread.
Simulator* Simulator::current(Isolate* isolate) {
  v8::internal::Isolate::PerIsolateThreadData* isolate_data =
      isolate->FindOrAllocatePerThreadDataForThisThread();
  DCHECK_NOT_NULL(isolate_data);

  Simulator* sim = isolate_data->simulator();
  if (sim == nullptr) {
    // TODO(146): delete the simulator object when a thread/isolate goes away.
    sim = new Simulator(isolate);
    isolate_data->set_simulator(sim);
  }
  return sim;
}

// Sets the register in the architecture state. It will also deal with
// updating Simulator internal state for special registers such as PC.
void Simulator::set_register(int reg, int64_t value) {
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters));
  if (reg == pc) {
    pc_modified_ = true;
  }

  // Zero register always holds 0.
  registers_[reg] = (reg == 0) ? 0 : value;
}

void Simulator::set_dw_register(int reg, const int* dbl) {
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters));
  registers_[reg] = dbl[1];
  registers_[reg] = registers_[reg] << 32;
  registers_[reg] += dbl[0];
}

void Simulator::set_fpu_register(int fpureg, int64_t value) {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  FPUregisters_[fpureg] = value;
}

void Simulator::set_fpu_register_word(int fpureg, int32_t value) {
  // Set ONLY lower 32-bits, leaving upper bits untouched.
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  int32_t* pword;
  if (kArchEndian == kLittle) {
    pword = reinterpret_cast<int32_t*>(&FPUregisters_[fpureg]);
  } else {
    pword = reinterpret_cast<int32_t*>(&FPUregisters_[fpureg]) + 1;
  }
  *pword = value;
}

void Simulator::set_fpu_register_hi_word(int fpureg, int32_t value) {
  // Set ONLY upper 32-bits, leaving lower bits untouched.
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  int32_t* phiword;
  if (kArchEndian == kLittle) {
    phiword = (reinterpret_cast<int32_t*>(&FPUregisters_[fpureg])) + 1;
  } else {
    phiword = reinterpret_cast<int32_t*>(&FPUregisters_[fpureg]);
  }
  *phiword = value;
}

void Simulator::set_fpu_register_float(int fpureg, float value) {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  FPUregisters_[fpureg] = box_float(value);
}

void Simulator::set_fpu_register_double(int fpureg, double value) {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  *bit_cast<double*>(&FPUregisters_[fpureg]) = value;
}

// Get the register from the architecture state. This function does handle
// the special case of accessing the PC register.
int64_t Simulator::get_register(int reg) const {
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters));
  if (reg == 0)
    return 0;
  else
    return registers_[reg] + ((reg == pc) ? Instruction::kPCReadOffset : 0);
}

double Simulator::get_double_from_register_pair(int reg) {
  // TODO(plind): bad ABI stuff, refactor or remove.
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters) && ((reg % 2) == 0));

  double dm_val = 0.0;
  // Read the bits from the unsigned integer register_[] array
  // into the double precision floating point value and return it.
  char buffer[sizeof(registers_[0])];
  memcpy(buffer, &registers_[reg], sizeof(registers_[0]));
  memcpy(&dm_val, buffer, sizeof(registers_[0]));
  return (dm_val);
}

int64_t Simulator::get_fpu_register(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return FPUregisters_[fpureg];
}

int32_t Simulator::get_fpu_register_word(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return static_cast<int32_t>(FPUregisters_[fpureg] & 0xFFFFFFFF);
}

int32_t Simulator::get_fpu_register_signed_word(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return static_cast<int32_t>(FPUregisters_[fpureg] & 0xFFFFFFFF);
}

int32_t Simulator::get_fpu_register_hi_word(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return static_cast<int32_t>((FPUregisters_[fpureg] >> 32) & 0xFFFFFFFF);
}

float Simulator::get_fpu_register_float(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  if (!is_boxed_float(FPUregisters_[fpureg])) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  return *bit_cast<float*>(const_cast<int64_t*>(&FPUregisters_[fpureg]));
}

double Simulator::get_fpu_register_double(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return *bit_cast<double*>(&FPUregisters_[fpureg]);
}

__int128_t Simulator::get_vregister(int vreg) const {
  DCHECK((vreg >= 0) && (vreg < kNumVRegisters));
  return Vregister_[vreg];
}

// Runtime FP routines take up to two double arguments and zero
// or one integer arguments. All are constructed here,
// from fa0, fa1, and a0.
void Simulator::GetFpArgs(double* x, double* y, int32_t* z) {
  *x = get_fpu_register_double(fa0);
  *y = get_fpu_register_double(fa1);
  *z = static_cast<int32_t>(get_register(a0));
}

// The return value is in fa0.
void Simulator::SetFpResult(const double& result) {
  set_fpu_register_double(fa0, result);
}

// helper functions to read/write/set/clear CRC values/bits
uint32_t Simulator::read_csr_value(uint32_t csr) {
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      return (FCSR_ & kFcsrFlagsMask);
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      return (FCSR_ & kFcsrFrmMask) >> kFcsrFrmShift;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      return (FCSR_ & kFcsrMask);
    default:
      UNIMPLEMENTED();
  }
}

uint32_t Simulator::get_dynamic_rounding_mode() {
  return read_csr_value(csr_frm);
}

void Simulator::write_csr_value(uint32_t csr, uint64_t val) {
  uint32_t value = (uint32_t)val;
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      DCHECK(value <= ((1 << kFcsrFlagsBits) - 1));
      FCSR_ = (FCSR_ & (~kFcsrFlagsMask)) | value;
      break;
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      DCHECK(value <= ((1 << kFcsrFrmBits) - 1));
      FCSR_ = (FCSR_ & (~kFcsrFrmMask)) | (value << kFcsrFrmShift);
      break;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      DCHECK(value <= ((1 << kFcsrBits) - 1));
      FCSR_ = (FCSR_ & (~kFcsrMask)) | value;
      break;
    default:
      UNIMPLEMENTED();
  }
}

void Simulator::set_csr_bits(uint32_t csr, uint64_t val) {
  uint32_t value = (uint32_t)val;
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      DCHECK(value <= ((1 << kFcsrFlagsBits) - 1));
      FCSR_ = FCSR_ | value;
      break;
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      DCHECK(value <= ((1 << kFcsrFrmBits) - 1));
      FCSR_ = FCSR_ | (value << kFcsrFrmShift);
      break;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      DCHECK(value <= ((1 << kFcsrBits) - 1));
      FCSR_ = FCSR_ | value;
      break;
    default:
      UNIMPLEMENTED();
  }
}

void Simulator::clear_csr_bits(uint32_t csr, uint64_t val) {
  uint32_t value = (uint32_t)val;
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      DCHECK(value <= ((1 << kFcsrFlagsBits) - 1));
      FCSR_ = FCSR_ & (~value);
      break;
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      DCHECK(value <= ((1 << kFcsrFrmBits) - 1));
      FCSR_ = FCSR_ & (~(value << kFcsrFrmShift));
      break;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      DCHECK(value <= ((1 << kFcsrBits) - 1));
      FCSR_ = FCSR_ & (~value);
      break;
    default:
      UNIMPLEMENTED();
  }
}

bool Simulator::test_fflags_bits(uint32_t mask) {
  return (FCSR_ & kFcsrFlagsMask & mask) != 0;
}

template <typename T>
T Simulator::FMaxMinHelper(T a, T b, MaxMinKind kind) {
  // set invalid bit for signaling nan
  if ((a == std::numeric_limits<T>::signaling_NaN()) ||
      (b == std::numeric_limits<T>::signaling_NaN())) {
    set_csr_bits(csr_fflags, kInvalidOperation);
  }

  T result = 0;
  if (std::isnan(a) && std::isnan(b)) {
    result = std::numeric_limits<float>::quiet_NaN();
  } else if (std::isnan(a)) {
    result = b;
  } else if (std::isnan(b)) {
    result = a;
  } else if (b == a) {  // Handle -0.0 == 0.0 case.
    if (kind == MaxMinKind::kMax) {
      result = std::signbit(b) ? a : b;
    } else {
      result = std::signbit(b) ? b : a;
    }
  } else {
    result = (kind == MaxMinKind::kMax) ? fmax(a, b) : fmin(a, b);
  }

  return result;
}

// Raw access to the PC register.
void Simulator::set_pc(int64_t value) {
  pc_modified_ = true;
  registers_[pc] = value;
  DCHECK(has_bad_pc() || ((value % kInstrSize) == 0) ||
         ((value % kShortInstrSize) == 0));
}

bool Simulator::has_bad_pc() const {
  return ((registers_[pc] == bad_ra) || (registers_[pc] == end_sim_pc));
}

// Raw access to the PC register without the special adjustment when reading.
int64_t Simulator::get_pc() const { return registers_[pc]; }

// The RISC-V spec leaves it open to the implementation on how to handle
// unaligned reads and writes. For now, we simply disallow unaligned reads but
// at some point, we may want to implement some other behavior.

// TODO(plind): refactor this messy debug code when we do unaligned access.
void Simulator::DieOrDebug() {
  if (FLAG_riscv_trap_to_simulator_debugger) {
    RiscvDebugger dbg(this);
    dbg.Debug();
  } else {
    base::OS::Abort();
  }
}

void Simulator::TraceRegWr(int64_t value, TraceType t) {
  if (::v8::internal::FLAG_trace_sim) {
    union {
      int64_t fmt_int64;
      int32_t fmt_int32[2];
      float fmt_float[2];
      double fmt_double;
    } v;
    v.fmt_int64 = value;

    switch (t) {
      case WORD:
        SNPrintF(trace_buf_,
                 "%016" PRIx64 "    (%" PRId64 ")    int32:%" PRId32
                 " uint32:%" PRIu32,
                 v.fmt_int64, icount_, v.fmt_int32[0], v.fmt_int32[0]);
        break;
      case DWORD:
        SNPrintF(trace_buf_,
                 "%016" PRIx64 "    (%" PRId64 ")    int64:%" PRId64
                 " uint64:%" PRIu64,
                 value, icount_, value, value);
        break;
      case FLOAT:
        SNPrintF(trace_buf_, "%016" PRIx64 "    (%" PRId64 ")    flt:%e",
                 v.fmt_int64, icount_, v.fmt_float[0]);
        break;
      case DOUBLE:
        SNPrintF(trace_buf_, "%016" PRIx64 "    (%" PRId64 ")    dbl:%e",
                 v.fmt_int64, icount_, v.fmt_double);
        break;
      default:
        UNREACHABLE();
    }
  }
}

// TODO(plind): consider making icount_ printing a flag option.
template <typename T>
void Simulator::TraceMemRd(int64_t addr, T value, int64_t reg_value) {
  if (::v8::internal::FLAG_trace_sim) {
    if (std::is_integral<T>::value) {
      switch (sizeof(T)) {
        case 1:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int8:%" PRId8
                   " uint8:%" PRIu8 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int8_t>(value),
                   static_cast<uint8_t>(value), addr);
          break;
        case 2:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int16:%" PRId16
                   " uint16:%" PRIu16 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int16_t>(value),
                   static_cast<uint16_t>(value), addr);
          break;
        case 4:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int32:%" PRId32
                   " uint32:%" PRIu32 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int32_t>(value),
                   static_cast<uint32_t>(value), addr);
          break;
        case 8:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int64:%" PRId64
                   " uint64:%" PRIu64 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int64_t>(value),
                   static_cast<uint64_t>(value), addr);
          break;
        default:
          UNREACHABLE();
      }
    } else if (std::is_same<float, T>::value) {
      SNPrintF(trace_buf_,
               "%016" PRIx64 "    (%" PRId64 ")    flt:%e <-- [addr: %" PRIx64
               "]",
               reg_value, icount_, static_cast<float>(value), addr);
    } else if (std::is_same<double, T>::value) {
      SNPrintF(trace_buf_,
               "%016" PRIx64 "    (%" PRId64 ")    dbl:%e <-- [addr: %" PRIx64
               "]",
               reg_value, icount_, static_cast<double>(value), addr);
    } else {
      UNREACHABLE();
    }
  }
}

template <typename T>
void Simulator::TraceMemWr(int64_t addr, T value) {
  if (::v8::internal::FLAG_trace_sim) {
    switch (sizeof(T)) {
      case 1:
        SNPrintF(trace_buf_,
                 "                    (%" PRIu64 ")    int8:%" PRId8
                 " uint8:%" PRIu8 " --> [addr: %" PRIx64 "]",
                 icount_, static_cast<int8_t>(value),
                 static_cast<uint8_t>(value), addr);
        break;
      case 2:
        SNPrintF(trace_buf_,
                 "                    (%" PRIu64 ")    int16:%" PRId16
                 " uint16:%" PRIu16 " --> [addr: %" PRIx64 "]",
                 icount_, static_cast<int16_t>(value),
                 static_cast<uint16_t>(value), addr);
        break;
      case 4:
        if (std::is_integral<T>::value) {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64 ")    int32:%" PRId32
                   " uint32:%" PRIu32 " --> [addr: %" PRIx64 "]",
                   icount_, static_cast<int32_t>(value),
                   static_cast<uint32_t>(value), addr);
        } else {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64
                   ")    flt:%e --> [addr: %" PRIx64 "]",
                   icount_, static_cast<float>(value), addr);
        }
        break;
      case 8:
        if (std::is_integral<T>::value) {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64 ")    int64:%" PRId64
                   " uint64:%" PRIu64 " --> [addr: %" PRIx64 "]",
                   icount_, static_cast<int64_t>(value),
                   static_cast<uint64_t>(value), addr);
        } else {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64
                   ")    dbl:%e --> [addr: %" PRIx64 "]",
                   icount_, static_cast<double>(value), addr);
        }
        break;
      default:
        UNREACHABLE();
    }
  }
}

// RISCV Memory Read/Write functions

// TODO(RISCV): check whether the specific board supports unaligned load/store
// (determined by EEI). For now, we assume the board does not support unaligned
// load/store (e.g., trapping)
template <typename T>
T Simulator::ReadMem(int64_t addr, Instruction* instr) {
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    PrintF("Memory read from bad address: 0x%08" PRIx64 " , pc=0x%08" PRIxPTR
           " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
#ifndef V8_COMPRESS_POINTERS  // TODO(RISCV): v8:11812
  // check for natural alignment
  if (!FLAG_riscv_c_extension && ((addr & (sizeof(T) - 1)) != 0)) {
    PrintF("Unaligned read at 0x%08" PRIx64 " , pc=0x%08" V8PRIxPTR "\n", addr,
           reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
#endif
  T* ptr = reinterpret_cast<T*>(addr);
  T value = *ptr;
  return value;
}

template <typename T>
void Simulator::WriteMem(int64_t addr, T value, Instruction* instr) {
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    PrintF("Memory write to bad address: 0x%08" PRIx64 " , pc=0x%08" PRIxPTR
           " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
#ifndef V8_COMPRESS_POINTERS  // TODO(RISCV): v8:11812
  // check for natural alignment
  if (!FLAG_riscv_c_extension && ((addr & (sizeof(T) - 1)) != 0)) {
    PrintF("Unaligned write at 0x%08" PRIx64 " , pc=0x%08" V8PRIxPTR "\n", addr,
           reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
#endif
  T* ptr = reinterpret_cast<T*>(addr);
  TraceMemWr(addr, value);
  // PrintF("Unaligned read at 0x%08" PRIx64 " , pc=0x%08" PRId64 "\n",
  // (int64_t)ptr,
  //        (int64_t)value);
  *ptr = value;
}

// Returns the limit of the stack area to enable checking for stack overflows.
uintptr_t Simulator::StackLimit(uintptr_t c_limit) const {
  // The simulator uses a separate JS stack. If we have exhausted the C stack,
  // we also drop down the JS limit to reflect the exhaustion on the JS stack.
  if (GetCurrentStackPosition() < c_limit) {
    return reinterpret_cast<uintptr_t>(get_sp());
  }

  // Otherwise the limit is the JS stack. Leave a safety margin of 1024 bytes
  // to prevent overrunning the stack when pushing values.
  return reinterpret_cast<uintptr_t>(stack_) + 1024;
}

// Unsupported instructions use Format to print an error and stop execution.
void Simulator::Format(Instruction* instr, const char* format) {
  PrintF("Simulator found unsupported instruction:\n 0x%08" PRIxPTR " : %s\n",
         reinterpret_cast<intptr_t>(instr), format);
  UNIMPLEMENTED_RISCV();
}

// Calls into the V8 runtime are based on this very simple interface.
// Note: To be able to return two values from some calls the code in
// runtime.cc uses the ObjectPair which is essentially two 32-bit values
// stuffed into a 64-bit value. With the code below we assume that all runtime
// calls return 64 bits of result. If they don't, the a1 result register
// contains a bogus value, which is fine because it is caller-saved.

using SimulatorRuntimeCall = ObjectPair (*)(int64_t arg0, int64_t arg1,
                                            int64_t arg2, int64_t arg3,
                                            int64_t arg4, int64_t arg5,
                                            int64_t arg6, int64_t arg7,
                                            int64_t arg8, int64_t arg9);

// These prototypes handle the four types of FP calls.
using SimulatorRuntimeCompareCall = int64_t (*)(double darg0, double darg1);
using SimulatorRuntimeFPFPCall = double (*)(double darg0, double darg1);
using SimulatorRuntimeFPCall = double (*)(double darg0);
using SimulatorRuntimeFPIntCall = double (*)(double darg0, int32_t arg0);

// This signature supports direct call in to API function native callback
// (refer to InvocationCallback in v8.h).
using SimulatorRuntimeDirectApiCall = void (*)(int64_t arg0);
using SimulatorRuntimeProfilingApiCall = void (*)(int64_t arg0, void* arg1);

// This signature supports direct call to accessor getter callback.
using SimulatorRuntimeDirectGetterCall = void (*)(int64_t arg0, int64_t arg1);
using SimulatorRuntimeProfilingGetterCall = void (*)(int64_t arg0, int64_t arg1,
                                                     void* arg2);

// Software interrupt instructions are used by the simulator to call into the
// C-based V8 runtime. They are also used for debugging with simulator.
void Simulator::SoftwareInterrupt() {
  // There are two instructions that could get us here, the ebreak or ecall
  // instructions are "SYSTEM" class opcode distinuished by Imm12Value field w/
  // the rest of instruction fields being zero
  int32_t func = instr_.Imm12Value();
  // We first check if we met a call_rt_redirected.
  if (instr_.InstructionBits() == rtCallRedirInstr) {  // ECALL
    Redirection* redirection = Redirection::FromInstruction(instr_.instr());

    int64_t* stack_pointer = reinterpret_cast<int64_t*>(get_register(sp));

    int64_t arg0 = get_register(a0);
    int64_t arg1 = get_register(a1);
    int64_t arg2 = get_register(a2);
    int64_t arg3 = get_register(a3);
    int64_t arg4 = get_register(a4);
    int64_t arg5 = get_register(a5);
    int64_t arg6 = get_register(a6);
    int64_t arg7 = get_register(a7);
    int64_t arg8 = stack_pointer[0];
    int64_t arg9 = stack_pointer[1];
    STATIC_ASSERT(kMaxCParameters == 10);

    bool fp_call =
        (redirection->type() == ExternalReference::BUILTIN_FP_FP_CALL) ||
        (redirection->type() == ExternalReference::BUILTIN_COMPARE_CALL) ||
        (redirection->type() == ExternalReference::BUILTIN_FP_CALL) ||
        (redirection->type() == ExternalReference::BUILTIN_FP_INT_CALL);

    // This is dodgy but it works because the C entry stubs are never moved.
    // See comment in codegen-arm.cc and bug 1242173.
    int64_t saved_ra = get_register(ra);

    int64_t pc = get_pc();

    intptr_t external =
        reinterpret_cast<intptr_t>(redirection->external_function());

    if (fp_call) {
      double dval0, dval1;  // one or two double parameters
      int32_t ival;         // zero or one integer parameters
      int64_t iresult = 0;  // integer return value
      double dresult = 0;   // double return value
      GetFpArgs(&dval0, &dval1, &ival);
      SimulatorRuntimeCall generic_target =
          reinterpret_cast<SimulatorRuntimeCall>(external);
      if (::v8::internal::FLAG_trace_sim) {
        switch (redirection->type()) {
          case ExternalReference::BUILTIN_FP_FP_CALL:
          case ExternalReference::BUILTIN_COMPARE_CALL:
            PrintF("Call to host function %s at %p with args %f, %f",
                   ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
                   reinterpret_cast<void*>(FUNCTION_ADDR(generic_target)),
                   dval0, dval1);
            break;
          case ExternalReference::BUILTIN_FP_CALL:
            PrintF("Call to host function %s at %p with arg %f",
                   ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
                   reinterpret_cast<void*>(FUNCTION_ADDR(generic_target)),
                   dval0);
            break;
          case ExternalReference::BUILTIN_FP_INT_CALL:
            PrintF("Call to host function %s at %p with args %f, %d",
                   ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
                   reinterpret_cast<void*>(FUNCTION_ADDR(generic_target)),
                   dval0, ival);
            break;
          default:
            UNREACHABLE();
        }
      }
      switch (redirection->type()) {
        case ExternalReference::BUILTIN_COMPARE_CALL: {
          SimulatorRuntimeCompareCall target =
              reinterpret_cast<SimulatorRuntimeCompareCall>(external);
          iresult = target(dval0, dval1);
          set_register(a0, static_cast<int64_t>(iresult));
          //  set_register(a1, static_cast<int64_t>(iresult >> 32));
          break;
        }
        case ExternalReference::BUILTIN_FP_FP_CALL: {
          SimulatorRuntimeFPFPCall target =
              reinterpret_cast<SimulatorRuntimeFPFPCall>(external);
          dresult = target(dval0, dval1);
          SetFpResult(dresult);
          break;
        }
        case ExternalReference::BUILTIN_FP_CALL: {
          SimulatorRuntimeFPCall target =
              reinterpret_cast<SimulatorRuntimeFPCall>(external);
          dresult = target(dval0);
          SetFpResult(dresult);
          break;
        }
        case ExternalReference::BUILTIN_FP_INT_CALL: {
          SimulatorRuntimeFPIntCall target =
              reinterpret_cast<SimulatorRuntimeFPIntCall>(external);
          dresult = target(dval0, ival);
          SetFpResult(dresult);
          break;
        }
        default:
          UNREACHABLE();
      }
      if (::v8::internal::FLAG_trace_sim) {
        switch (redirection->type()) {
          case ExternalReference::BUILTIN_COMPARE_CALL:
            PrintF("Returned %08x\n", static_cast<int32_t>(iresult));
            break;
          case ExternalReference::BUILTIN_FP_FP_CALL:
          case ExternalReference::BUILTIN_FP_CALL:
          case ExternalReference::BUILTIN_FP_INT_CALL:
            PrintF("Returned %f\n", dresult);
            break;
          default:
            UNREACHABLE();
        }
      }
    } else if (redirection->type() == ExternalReference::DIRECT_API_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function %s at %p args %08" PRIx64 " \n",
               ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
               reinterpret_cast<void*>(external), arg0);
      }
      SimulatorRuntimeDirectApiCall target =
          reinterpret_cast<SimulatorRuntimeDirectApiCall>(external);
      target(arg0);
    } else if (redirection->type() == ExternalReference::PROFILING_API_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function %s at %p args %08" PRIx64 "  %08" PRIx64
               " \n",
               ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
               reinterpret_cast<void*>(external), arg0, arg1);
      }
      SimulatorRuntimeProfilingApiCall target =
          reinterpret_cast<SimulatorRuntimeProfilingApiCall>(external);
      target(arg0, Redirection::ReverseRedirection(arg1));
    } else if (redirection->type() == ExternalReference::DIRECT_GETTER_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function %s at %p args %08" PRIx64 "  %08" PRIx64
               " \n",
               ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
               reinterpret_cast<void*>(external), arg0, arg1);
      }
      SimulatorRuntimeDirectGetterCall target =
          reinterpret_cast<SimulatorRuntimeDirectGetterCall>(external);
      target(arg0, arg1);
    } else if (redirection->type() ==
               ExternalReference::PROFILING_GETTER_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function %s at %p args %08" PRIx64 "  %08" PRIx64
               "  %08" PRIx64 " \n",
               ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
               reinterpret_cast<void*>(external), arg0, arg1, arg2);
      }
      SimulatorRuntimeProfilingGetterCall target =
          reinterpret_cast<SimulatorRuntimeProfilingGetterCall>(external);
      target(arg0, arg1, Redirection::ReverseRedirection(arg2));
    } else {
      DCHECK(redirection->type() == ExternalReference::BUILTIN_CALL ||
             redirection->type() == ExternalReference::BUILTIN_CALL_PAIR);
      SimulatorRuntimeCall target =
          reinterpret_cast<SimulatorRuntimeCall>(external);
      if (::v8::internal::FLAG_trace_sim) {
        PrintF(
            "Call to host function %s at %p "
            "args %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64
            " , %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64
            " , %08" PRIx64 " , %08" PRIx64 " \n",
            ExternalReferenceTable::NameOfIsolateIndependentAddress(pc),
            reinterpret_cast<void*>(FUNCTION_ADDR(target)), arg0, arg1, arg2,
            arg3, arg4, arg5, arg6, arg7, arg8, arg9);
      }
      ObjectPair result =
          target(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9);
      set_register(a0, (int64_t)(result.x));
      set_register(a1, (int64_t)(result.y));
    }
    if (::v8::internal::FLAG_trace_sim) {
      PrintF("Returned %08" PRIx64 "  : %08" PRIx64 " \n", get_register(a1),
             get_register(a0));
    }
    set_register(ra, saved_ra);
    set_pc(get_register(ra));

  } else if (func == 1) {  // EBREAK
    int32_t code = get_ebreak_code(instr_.instr());
    set_pc(get_pc() + kInstrSize * 2);
    if (code != -1 && static_cast<uint32_t>(code) <= kMaxStopCode) {
      if (IsWatchpoint(code)) {
        PrintWatchpoint(code);
      } else {
        IncreaseStopCounter(code);
        HandleStop(code);
      }
    } else {
      // All remaining break_ codes, and all traps are handled here.
      RiscvDebugger dbg(this);
      dbg.Debug();
    }
  } else {
    UNREACHABLE();
  }
}

// Stop helper functions.
bool Simulator::IsWatchpoint(uint64_t code) {
  return (code <= kMaxWatchpointCode);
}

void Simulator::PrintWatchpoint(uint64_t code) {
  RiscvDebugger dbg(this);
  ++break_count_;
  PrintF("\n---- watchpoint %" PRId64 "  marker: %3d  (instr count: %8" PRId64
         " ) ----------"
         "----------------------------------",
         code, break_count_, icount_);
  dbg.PrintAllRegs();  // Print registers and continue running.
}

void Simulator::HandleStop(uint64_t code) {
  // Stop if it is enabled, otherwise go on jumping over the stop
  // and the message address.
  if (IsEnabledStop(code)) {
    RiscvDebugger dbg(this);
    PrintF("Simulator hit stop (%" PRId64 ")\n", code);
    dbg.Debug();
  }
}

bool Simulator::IsStopInstruction(Instruction* instr) {
  if (instr->InstructionBits() != kBreakInstr) return false;
  int32_t code = get_ebreak_code(instr);
  return code != -1 && static_cast<uint32_t>(code) > kMaxWatchpointCode &&
         static_cast<uint32_t>(code) <= kMaxStopCode;
}

bool Simulator::IsEnabledStop(uint64_t code) {
  DCHECK_LE(code, kMaxStopCode);
  DCHECK_GT(code, kMaxWatchpointCode);
  return !(watched_stops_[code].count & kStopDisabledBit);
}

void Simulator::EnableStop(uint64_t code) {
  if (!IsEnabledStop(code)) {
    watched_stops_[code].count &= ~kStopDisabledBit;
  }
}

void Simulator::DisableStop(uint64_t code) {
  if (IsEnabledStop(code)) {
    watched_stops_[code].count |= kStopDisabledBit;
  }
}

void Simulator::IncreaseStopCounter(uint64_t code) {
  DCHECK_LE(code, kMaxStopCode);
  if ((watched_stops_[code].count & ~(1 << 31)) == 0x7FFFFFFF) {
    PrintF("Stop counter for code %" PRId64
           "  has overflowed.\n"
           "Enabling this code and reseting the counter to 0.\n",
           code);
    watched_stops_[code].count = 0;
    EnableStop(code);
  } else {
    watched_stops_[code].count++;
  }
}

// Print a stop status.
void Simulator::PrintStopInfo(uint64_t code) {
  if (code <= kMaxWatchpointCode) {
    PrintF("That is a watchpoint, not a stop.\n");
    return;
  } else if (code > kMaxStopCode) {
    PrintF("Code too large, only %u stops can be used\n", kMaxStopCode + 1);
    return;
  }
  const char* state = IsEnabledStop(code) ? "Enabled" : "Disabled";
  int32_t count = watched_stops_[code].count & ~kStopDisabledBit;
  // Don't print the state of unused breakpoints.
  if (count != 0) {
    if (watched_stops_[code].desc) {
      PrintF("stop %" PRId64 "  - 0x%" PRIx64 " : \t%s, \tcounter = %i, \t%s\n",
             code, code, state, count, watched_stops_[code].desc);
    } else {
      PrintF("stop %" PRId64 "  - 0x%" PRIx64 " : \t%s, \tcounter = %i\n", code,
             code, state, count);
    }
  }
}

void Simulator::SignalException(Exception e) {
  FATAL("Error: Exception %i raised.", static_cast<int>(e));
}

// RISCV Instruction Decode Routine
void Simulator::DecodeRVRType() {
  switch (instr_.InstructionBits() & kRTypeMask) {
    case RO_ADD: {
      set_rd(sext_xlen(rs1() + rs2()));
      break;
    }
    case RO_SUB: {
      set_rd(sext_xlen(rs1() - rs2()));
      break;
    }
    case RO_SLL: {
      set_rd(sext_xlen(rs1() << (rs2() & (xlen - 1))));
      break;
    }
    case RO_SLT: {
      set_rd(sreg_t(rs1()) < sreg_t(rs2()));
      break;
    }
    case RO_SLTU: {
      set_rd(reg_t(rs1()) < reg_t(rs2()));
      break;
    }
    case RO_XOR: {
      set_rd(rs1() ^ rs2());
      break;
    }
    case RO_SRL: {
      set_rd(sext_xlen(zext_xlen(rs1()) >> (rs2() & (xlen - 1))));
      break;
    }
    case RO_SRA: {
      set_rd(sext_xlen(sext_xlen(rs1()) >> (rs2() & (xlen - 1))));
      break;
    }
    case RO_OR: {
      set_rd(rs1() | rs2());
      break;
    }
    case RO_AND: {
      set_rd(rs1() & rs2());
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_ADDW: {
      set_rd(sext32(rs1() + rs2()));
      break;
    }
    case RO_SUBW: {
      set_rd(sext32(rs1() - rs2()));
      break;
    }
    case RO_SLLW: {
      set_rd(sext32(rs1() << (rs2() & 0x1F)));
      break;
    }
    case RO_SRLW: {
      set_rd(sext32(uint32_t(rs1()) >> (rs2() & 0x1F)));
      break;
    }
    case RO_SRAW: {
      set_rd(sext32(int32_t(rs1()) >> (rs2() & 0x1F)));
      break;
    }
#endif /* V8_TARGET_ARCH_64_BIT */
      // TODO(riscv): Add RISCV M extension macro
    case RO_MUL: {
      set_rd(rs1() * rs2());
      break;
    }
    case RO_MULH: {
      set_rd(mulh(rs1(), rs2()));
      break;
    }
    case RO_MULHSU: {
      set_rd(mulhsu(rs1(), rs2()));
      break;
    }
    case RO_MULHU: {
      set_rd(mulhu(rs1(), rs2()));
      break;
    }
    case RO_DIV: {
      sreg_t lhs = sext_xlen(rs1());
      sreg_t rhs = sext_xlen(rs2());
      if (rhs == 0) {
        set_rd(-1);
      } else if (lhs == INT64_MIN && rhs == -1) {
        set_rd(lhs);
      } else {
        set_rd(sext_xlen(lhs / rhs));
      }
      break;
    }
    case RO_DIVU: {
      reg_t lhs = zext_xlen(rs1());
      reg_t rhs = zext_xlen(rs2());
      if (rhs == 0) {
        set_rd(UINT64_MAX);
      } else {
        set_rd(zext_xlen(lhs / rhs));
      }
      break;
    }
    case RO_REM: {
      sreg_t lhs = sext_xlen(rs1());
      sreg_t rhs = sext_xlen(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else if (lhs == INT64_MIN && rhs == -1) {
        set_rd(0);
      } else {
        set_rd(sext_xlen(lhs % rhs));
      }
      break;
    }
    case RO_REMU: {
      reg_t lhs = zext_xlen(rs1());
      reg_t rhs = zext_xlen(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else {
        set_rd(zext_xlen(lhs % rhs));
      }
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_MULW: {
      set_rd(sext32(sext32(rs1()) * sext32(rs2())));
      break;
    }
    case RO_DIVW: {
      sreg_t lhs = sext32(rs1());
      sreg_t rhs = sext32(rs2());
      if (rhs == 0) {
        set_rd(-1);
      } else if (lhs == INT32_MIN && rhs == -1) {
        set_rd(lhs);
      } else {
        set_rd(sext32(lhs / rhs));
      }
      break;
    }
    case RO_DIVUW: {
      reg_t lhs = zext32(rs1());
      reg_t rhs = zext32(rs2());
      if (rhs == 0) {
        set_rd(UINT32_MAX);
      } else {
        set_rd(zext32(lhs / rhs));
      }
      break;
    }
    case RO_REMW: {
      sreg_t lhs = sext32(rs1());
      sreg_t rhs = sext32(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else if (lhs == INT32_MIN && rhs == -1) {
        set_rd(0);
      } else {
        set_rd(sext32(lhs % rhs));
      }
      break;
    }
    case RO_REMUW: {
      reg_t lhs = zext32(rs1());
      reg_t rhs = zext32(rs2());
      if (rhs == 0) {
        set_rd(zext32(lhs));
      } else {
        set_rd(zext32(lhs % rhs));
      }
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
      // TODO(riscv): End Add RISCV M extension macro
    default: {
      switch (instr_.BaseOpcode()) {
        case AMO:
          DecodeRVRAType();
          break;
        case OP_FP:
          DecodeRVRFPType();
          break;
        default:
          UNSUPPORTED();
      }
    }
  }
}

float Simulator::RoundF2FHelper(float input_val, int rmode) {
  if (rmode == DYN) rmode = get_dynamic_rounding_mode();

  float rounded = 0;
  switch (rmode) {
    case RNE: {  // Round to Nearest, tiest to Even
      rounded = floorf(input_val);
      float error = input_val - rounded;

      // Take care of correctly handling the range [-0.5, -0.0], which must
      // yield -0.0.
      if ((-0.5 <= input_val) && (input_val < 0.0)) {
        rounded = -0.0;

        // If the error is greater than 0.5, or is equal to 0.5 and the integer
        // result is odd, round up.
      } else if ((error > 0.5) ||
                 ((error == 0.5) && (std::fmod(rounded, 2) != 0))) {
        rounded++;
      }
      break;
    }
    case RTZ:  // Round towards Zero
      rounded = std::truncf(input_val);
      break;
    case RDN:  // Round Down (towards -infinity)
      rounded = floorf(input_val);
      break;
    case RUP:  // Round Up (towards +infinity)
      rounded = ceilf(input_val);
      break;
    case RMM:  // Round to Nearest, tiest to Max Magnitude
      rounded = std::roundf(input_val);
      break;
    default:
      UNREACHABLE();
  }

  return rounded;
}

double Simulator::RoundF2FHelper(double input_val, int rmode) {
  if (rmode == DYN) rmode = get_dynamic_rounding_mode();

  double rounded = 0;
  switch (rmode) {
    case RNE: {  // Round to Nearest, tiest to Even
      rounded = std::floor(input_val);
      double error = input_val - rounded;

      // Take care of correctly handling the range [-0.5, -0.0], which must
      // yield -0.0.
      if ((-0.5 <= input_val) && (input_val < 0.0)) {
        rounded = -0.0;

        // If the error is greater than 0.5, or is equal to 0.5 and the integer
        // result is odd, round up.
      } else if ((error > 0.5) ||
                 ((error == 0.5) && (std::fmod(rounded, 2) != 0))) {
        rounded++;
      }
      break;
    }
    case RTZ:  // Round towards Zero
      rounded = std::trunc(input_val);
      break;
    case RDN:  // Round Down (towards -infinity)
      rounded = std::floor(input_val);
      break;
    case RUP:  // Round Up (towards +infinity)
      rounded = std::ceil(input_val);
      break;
    case RMM:  // Round to Nearest, tiest to Max Magnitude
      rounded = std::round(input_val);
      break;
    default:
      UNREACHABLE();
  }
  return rounded;
}

// convert rounded floating-point to integer types, handle input values that
// are out-of-range, underflow, or NaN, and set appropriate fflags
template <typename I_TYPE, typename F_TYPE>
I_TYPE Simulator::RoundF2IHelper(F_TYPE original, int rmode) {
  DCHECK(std::is_integral<I_TYPE>::value);

  DCHECK((std::is_same<F_TYPE, float>::value ||
          std::is_same<F_TYPE, double>::value));

  I_TYPE max_i = std::numeric_limits<I_TYPE>::max();
  I_TYPE min_i = std::numeric_limits<I_TYPE>::min();

  if (!std::isfinite(original)) {
    set_fflags(kInvalidOperation);
    if (std::isnan(original) ||
        original == std::numeric_limits<F_TYPE>::infinity()) {
      return max_i;
    } else {
      DCHECK(original == -std::numeric_limits<F_TYPE>::infinity());
      return min_i;
    }
  }

  F_TYPE rounded = RoundF2FHelper(original, rmode);
  if (original != rounded) set_fflags(kInexact);

  if (!std::isfinite(rounded)) {
    set_fflags(kInvalidOperation);
    if (std::isnan(rounded) ||
        rounded == std::numeric_limits<F_TYPE>::infinity()) {
      return max_i;
    } else {
      DCHECK(rounded == -std::numeric_limits<F_TYPE>::infinity());
      return min_i;
    }
  }

  // Since integer max values are either all 1s (for unsigned) or all 1s
  // except for sign-bit (for signed), they cannot be represented precisely in
  // floating point, in order to precisely tell whether the rounded floating
  // point is within the max range, we compare against (max_i+1) which would
  // have a single 1 w/ many trailing zeros
  float max_i_plus_1 =
      std::is_same<uint64_t, I_TYPE>::value
          ? 0x1p64f  // uint64_t::max + 1 cannot be represented in integers,
                     // so use its float representation directly
          : static_cast<float>(static_cast<uint64_t>(max_i) + 1);
  if (rounded >= max_i_plus_1) {
    set_fflags(kOverflow | kInvalidOperation);
    return max_i;
  }

  // Since min_i (either 0 for unsigned, or for signed) is represented
  // precisely in floating-point,  comparing rounded directly against min_i
  if (rounded <= min_i) {
    if (rounded < min_i) set_fflags(kOverflow | kInvalidOperation);
    return min_i;
  }

  F_TYPE underflow_fval =
      std::is_same<F_TYPE, float>::value ? FLT_MIN : DBL_MIN;
  if (rounded < underflow_fval && rounded > -underflow_fval && rounded != 0) {
    set_fflags(kUnderflow);
  }

  return static_cast<I_TYPE>(rounded);
}

template <typename T>
static int64_t FclassHelper(T value) {
  switch (std::fpclassify(value)) {
    case FP_INFINITE:
      return (std::signbit(value) ? kNegativeInfinity : kPositiveInfinity);
    case FP_NAN:
      return (isSnan(value) ? kSignalingNaN : kQuietNaN);
    case FP_NORMAL:
      return (std::signbit(value) ? kNegativeNormalNumber
                                  : kPositiveNormalNumber);
    case FP_SUBNORMAL:
      return (std::signbit(value) ? kNegativeSubnormalNumber
                                  : kPositiveSubnormalNumber);
    case FP_ZERO:
      return (std::signbit(value) ? kNegativeZero : kPositiveZero);
    default:
      UNREACHABLE();
  }
}

template <typename T>
bool Simulator::CompareFHelper(T input1, T input2, FPUCondition cc) {
  DCHECK(std::is_floating_point<T>::value);
  bool result = false;
  switch (cc) {
    case LT:
    case LE:
      // FLT, FLE are signaling compares
      if (std::isnan(input1) || std::isnan(input2)) {
        set_fflags(kInvalidOperation);
        result = false;
      } else {
        result = (cc == LT) ? (input1 < input2) : (input1 <= input2);
      }
      break;

    case EQ:
      if (std::numeric_limits<T>::signaling_NaN() == input1 ||
          std::numeric_limits<T>::signaling_NaN() == input2) {
        set_fflags(kInvalidOperation);
      }
      if (std::isnan(input1) || std::isnan(input2)) {
        result = false;
      } else {
        result = (input1 == input2);
      }
      break;
    case NE:
      if (std::numeric_limits<T>::signaling_NaN() == input1 ||
          std::numeric_limits<T>::signaling_NaN() == input2) {
        set_fflags(kInvalidOperation);
      }
      if (std::isnan(input1) || std::isnan(input2)) {
        result = true;
      } else {
        result = (input1 != input2);
      }
      break;
    default:
      UNREACHABLE();
  }
  return result;
}

template <typename T>
static inline bool is_invalid_fmul(T src1, T src2) {
  return (isinf(src1) && src2 == static_cast<T>(0.0)) ||
         (src1 == static_cast<T>(0.0) && isinf(src2));
}

template <typename T>
static inline bool is_invalid_fadd(T src1, T src2) {
  return (isinf(src1) && isinf(src2) &&
          std::signbit(src1) != std::signbit(src2));
}

template <typename T>
static inline bool is_invalid_fsub(T src1, T src2) {
  return (isinf(src1) && isinf(src2) &&
          std::signbit(src1) == std::signbit(src2));
}

template <typename T>
static inline bool is_invalid_fdiv(T src1, T src2) {
  return ((src1 == 0 && src2 == 0) || (isinf(src1) && isinf(src2)));
}

template <typename T>
static inline bool is_invalid_fsqrt(T src1) {
  return (src1 < 0);
}

void Simulator::DecodeRVRAType() {
  // TODO(riscv): Add macro for RISCV A extension
  // Special handling for A extension instructions because it uses func5
  // For all A extension instruction, V8 simulator is pure sequential. No
  // Memory address lock or other synchronizaiton behaviors.
  switch (instr_.InstructionBits() & kRATypeMask) {
    case RO_LR_W: {
      base::MutexGuard lock_guard(&GlobalMonitor::Get()->mutex);
      int64_t addr = rs1();
      auto val = ReadMem<int32_t>(addr, instr_.instr());
      set_rd(sext32(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      local_monitor_.NotifyLoadLinked(addr, TransactionSize::Word);
      GlobalMonitor::Get()->NotifyLoadLinked_Locked(addr,
                                                    &global_monitor_thread_);
      break;
    }
    case RO_SC_W: {
      int64_t addr = rs1();
      base::MutexGuard lock_guard(&GlobalMonitor::Get()->mutex);
      if (local_monitor_.NotifyStoreConditional(addr, TransactionSize::Word) &&
          GlobalMonitor::Get()->NotifyStoreConditional_Locked(
              addr, &global_monitor_thread_)) {
        local_monitor_.NotifyStore();
        GlobalMonitor::Get()->NotifyStore_Locked(&global_monitor_thread_);
        WriteMem<int32_t>(rs1(), (int32_t)rs2(), instr_.instr());
        set_rd(0, false);
      } else {
        set_rd(1, false);
      }
      break;
    }
    case RO_AMOSWAP_W: {
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return (uint32_t)rs2(); }, instr_.instr(),
          WORD)));
      break;
    }
    case RO_AMOADD_W: {
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs + (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOXOR_W: {
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs ^ (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOAND_W: {
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs & (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOOR_W: {
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs | (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMIN_W: {
      set_rd(sext32(amo<int32_t>(
          rs1(), [&](int32_t lhs) { return std::min(lhs, (int32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMAX_W: {
      set_rd(sext32(amo<int32_t>(
          rs1(), [&](int32_t lhs) { return std::max(lhs, (int32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMINU_W: {
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return std::min(lhs, (uint32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMAXU_W: {
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return std::max(lhs, (uint32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_LR_D: {
      base::MutexGuard lock_guard(&GlobalMonitor::Get()->mutex);
      int64_t addr = rs1();
      auto val = ReadMem<int64_t>(addr, instr_.instr());
      set_rd(val, false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      local_monitor_.NotifyLoadLinked(addr, TransactionSize::DoubleWord);
      GlobalMonitor::Get()->NotifyLoadLinked_Locked(addr,
                                                    &global_monitor_thread_);
      break;
    }
    case RO_SC_D: {
      int64_t addr = rs1();
      base::MutexGuard lock_guard(&GlobalMonitor::Get()->mutex);
      if (local_monitor_.NotifyStoreConditional(addr,
                                                TransactionSize::DoubleWord) &&
          (GlobalMonitor::Get()->NotifyStoreConditional_Locked(
              addr, &global_monitor_thread_))) {
        GlobalMonitor::Get()->NotifyStore_Locked(&global_monitor_thread_);
        WriteMem<int64_t>(rs1(), rs2(), instr_.instr());
        set_rd(0, false);
      } else {
        set_rd(1, false);
      }
      break;
    }
    case RO_AMOSWAP_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return rs2(); }, instr_.instr(), DWORD));
      break;
    }
    case RO_AMOADD_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs + rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOXOR_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs ^ rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOAND_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs & rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOOR_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs | rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOMIN_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return std::min(lhs, rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMAX_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return std::max(lhs, rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMINU_D: {
      set_rd(amo<uint64_t>(
          rs1(), [&](uint64_t lhs) { return std::min(lhs, (uint64_t)rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMAXU_D: {
      set_rd(amo<uint64_t>(
          rs1(), [&](uint64_t lhs) { return std::max(lhs, (uint64_t)rs2()); },
          instr_.instr(), DWORD));
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
    // TODO(riscv): End Add macro for RISCV A extension
    default: {
      UNSUPPORTED();
    }
  }
}

void Simulator::DecodeRVRFPType() {
  // OP_FP instructions (F/D) uses func7 first. Some further uses func3 and
  // rs2()

  // kRATypeMask is only for func7
  switch (instr_.InstructionBits() & kRFPTypeMask) {
    // TODO(riscv): Add macro for RISCV F extension
    case RO_FADD_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fadd(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return frs1 + frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FSUB_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fsub(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return frs1 - frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FMUL_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fmul(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return frs1 * frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FDIV_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fdiv(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else if (frs2 == 0.0f) {
          this->set_fflags(kDivideByZero);
          return (std::signbit(frs1) == std::signbit(frs2)
                      ? std::numeric_limits<float>::infinity()
                      : -std::numeric_limits<float>::infinity());
        } else {
          return frs1 / frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FSQRT_S: {
      if (instr_.Rs2Value() == 0b00000) {
        // TODO(riscv): use rm value (round mode)
        auto fn = [this](float frs) {
          if (is_invalid_fsqrt(frs)) {
            this->set_fflags(kInvalidOperation);
            return std::numeric_limits<float>::quiet_NaN();
          } else {
            return std::sqrt(frs);
          }
        };
        set_frd(CanonicalizeFPUOp1<float>(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FSGNJ_S: {  // RO_FSGNJN_S  RO_FSQNJX_S
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FSGNJ_S
          set_frd(fsgnj32(frs1(), frs2(), false, false));
          break;
        }
        case 0b001: {  // RO_FSGNJN_S
          set_frd(fsgnj32(frs1(), frs2(), true, false));
          break;
        }
        case 0b010: {  // RO_FSQNJX_S
          set_frd(fsgnj32(frs1(), frs2(), false, true));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMIN_S: {  // RO_FMAX_S
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FMIN_S
          set_frd(FMaxMinHelper(frs1(), frs2(), MaxMinKind::kMin));
          break;
        }
        case 0b001: {  // RO_FMAX_S
          set_frd(FMaxMinHelper(frs1(), frs2(), MaxMinKind::kMax));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_W_S: {  // RO_FCVT_WU_S , 64F RO_FCVT_L_S RO_FCVT_LU_S
      float original_val = frs1();
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_W_S
          set_rd(RoundF2IHelper<int32_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00001: {  // RO_FCVT_WU_S
          set_rd(sext32(
              RoundF2IHelper<uint32_t>(original_val, instr_.RoundMode())));
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_L_S
          set_rd(RoundF2IHelper<int64_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00011: {  // RO_FCVT_LU_S
          set_rd(RoundF2IHelper<uint64_t>(original_val, instr_.RoundMode()));
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMV: {  // RO_FCLASS_S
      switch (instr_.Funct3Value()) {
        case 0b000: {
          if (instr_.Rs2Value() == 0b00000) {
            // RO_FMV_X_W
            set_rd(sext_xlen(get_fpu_register_word(rs1_reg())));
          } else {
            UNSUPPORTED();
          }
          break;
        }
        case 0b001: {  // RO_FCLASS_S
          set_rd(FclassHelper(frs1()));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FLE_S: {  // RO_FEQ_S RO_FLT_S RO_FLE_S
      switch (instr_.Funct3Value()) {
        case 0b010: {  // RO_FEQ_S
          set_rd(CompareFHelper(frs1(), frs2(), EQ));
          break;
        }
        case 0b001: {  // RO_FLT_S
          set_rd(CompareFHelper(frs1(), frs2(), LT));
          break;
        }
        case 0b000: {  // RO_FLE_S
          set_rd(CompareFHelper(frs1(), frs2(), LE));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_S_W: {  // RO_FCVT_S_WU , 64F RO_FCVT_S_L RO_FCVT_S_LU
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_S_W
          set_frd(static_cast<float>((int32_t)rs1()));
          break;
        }
        case 0b00001: {  // RO_FCVT_S_WU
          set_frd(static_cast<float>((uint32_t)rs1()));
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_S_L
          set_frd(static_cast<float>((int64_t)rs1()));
          break;
        }
        case 0b00011: {  // RO_FCVT_S_LU
          set_frd(static_cast<float>((uint64_t)rs1()));
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMV_W_X: {
      if (instr_.Funct3Value() == 0b000) {
        // since FMV preserves source bit-pattern, no need to canonize
        set_frd(bit_cast<float>((uint32_t)rs1()));
      } else {
        UNSUPPORTED();
      }
      break;
    }
      // TODO(riscv): Add macro for RISCV D extension
    case RO_FADD_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fadd(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return drs1 + drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FSUB_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fsub(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return drs1 - drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FMUL_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fmul(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return drs1 * drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FDIV_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fdiv(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else if (drs2 == 0.0) {
          this->set_fflags(kDivideByZero);
          return (std::signbit(drs1) == std::signbit(drs2)
                      ? std::numeric_limits<double>::infinity()
                      : -std::numeric_limits<double>::infinity());
        } else {
          return drs1 / drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FSQRT_D: {
      if (instr_.Rs2Value() == 0b00000) {
        // TODO(riscv): use rm value (round mode)
        auto fn = [this](double drs) {
          if (is_invalid_fsqrt(drs)) {
            this->set_fflags(kInvalidOperation);
            return std::numeric_limits<double>::quiet_NaN();
          } else {
            return std::sqrt(drs);
          }
        };
        set_drd(CanonicalizeFPUOp1<double>(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FSGNJ_D: {  // RO_FSGNJN_D RO_FSQNJX_D
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FSGNJ_D
          set_drd(fsgnj64(drs1(), drs2(), false, false));
          break;
        }
        case 0b001: {  // RO_FSGNJN_D
          set_drd(fsgnj64(drs1(), drs2(), true, false));
          break;
        }
        case 0b010: {  // RO_FSQNJX_D
          set_drd(fsgnj64(drs1(), drs2(), false, true));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMIN_D: {  // RO_FMAX_D
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FMIN_D
          set_drd(FMaxMinHelper(drs1(), drs2(), MaxMinKind::kMin));
          break;
        }
        case 0b001: {  // RO_FMAX_D
          set_drd(FMaxMinHelper(drs1(), drs2(), MaxMinKind::kMax));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case (RO_FCVT_S_D & kRFPTypeMask): {
      if (instr_.Rs2Value() == 0b00001) {
        auto fn = [](double drs) { return static_cast<float>(drs); };
        set_frd(CanonicalizeDoubleToFloatOperation(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FCVT_D_S: {
      if (instr_.Rs2Value() == 0b00000) {
        auto fn = [](float frs) { return static_cast<double>(frs); };
        set_drd(CanonicalizeFloatToDoubleOperation(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FLE_D: {  // RO_FEQ_D RO_FLT_D RO_FLE_D
      switch (instr_.Funct3Value()) {
        case 0b010: {  // RO_FEQ_S
          set_rd(CompareFHelper(drs1(), drs2(), EQ));
          break;
        }
        case 0b001: {  // RO_FLT_D
          set_rd(CompareFHelper(drs1(), drs2(), LT));
          break;
        }
        case 0b000: {  // RO_FLE_D
          set_rd(CompareFHelper(drs1(), drs2(), LE));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case (RO_FCLASS_D & kRFPTypeMask): {  // RO_FCLASS_D , 64D RO_FMV_X_D
      if (instr_.Rs2Value() != 0b00000) {
        UNSUPPORTED();
      }
      switch (instr_.Funct3Value()) {
        case 0b001: {  // RO_FCLASS_D
          set_rd(FclassHelper(drs1()));
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b000: {  // RO_FMV_X_D
          set_rd(bit_cast<int64_t>(drs1()));
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_W_D: {  // RO_FCVT_WU_D , 64F RO_FCVT_L_D RO_FCVT_LU_D
      double original_val = drs1();
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_W_D
          set_rd(RoundF2IHelper<int32_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00001: {  // RO_FCVT_WU_D
          set_rd(sext32(
              RoundF2IHelper<uint32_t>(original_val, instr_.RoundMode())));
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_L_D
          set_rd(RoundF2IHelper<int64_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00011: {  // RO_FCVT_LU_D
          set_rd(RoundF2IHelper<uint64_t>(original_val, instr_.RoundMode()));
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_D_W: {  // RO_FCVT_D_WU , 64F RO_FCVT_D_L RO_FCVT_D_LU
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_D_W
          set_drd((int32_t)rs1());
          break;
        }
        case 0b00001: {  // RO_FCVT_D_WU
          set_drd((uint32_t)rs1());
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_D_L
          set_drd((int64_t)rs1());
          break;
        }
        case 0b00011: {  // RO_FCVT_D_LU
          set_drd((uint64_t)rs1());
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_FMV_D_X: {
      if (instr_.Funct3Value() == 0b000 && instr_.Rs2Value() == 0b00000) {
        // Since FMV preserves source bit-pattern, no need to canonize
        set_drd(bit_cast<double>(rs1()));
      } else {
        UNSUPPORTED();
      }
      break;
    }
#endif /* V8_TARGET_ARCH_64_BIT */
    default: {
      UNSUPPORTED();
    }
  }
}

void Simulator::DecodeRVR4Type() {
  switch (instr_.InstructionBits() & kR4TypeMask) {
    // TODO(riscv): use F Extension macro block
    case RO_FMADD_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fadd(frs1 * frs2, frs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return std::fma(frs1, frs2, frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FMSUB_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fsub(frs1 * frs2, frs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return std::fma(frs1, frs2, -frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FNMSUB_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fsub(frs3, frs1 * frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return -std::fma(frs1, frs2, -frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FNMADD_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fadd(frs1 * frs2, frs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return -std::fma(frs1, frs2, frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    // TODO(riscv): use F Extension macro block
    case RO_FMADD_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fadd(drs1 * drs2, drs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return std::fma(drs1, drs2, drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FMSUB_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fsub(drs1 * drs2, drs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return std::fma(drs1, drs2, -drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FNMSUB_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fsub(drs3, drs1 * drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return -std::fma(drs1, drs2, -drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FNMADD_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fadd(drs1 * drs2, drs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return -std::fma(drs1, drs2, drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    default:
      UNSUPPORTED();
  }
}

bool Simulator::DecodeRvvVL() {
  uint32_t instr_temp =
      instr_.InstructionBits() & (kRvvMopMask | kRvvNfMask | kBaseOpcodeMask);
  if (RO_V_VL == instr_temp) {
    if (!(instr_.InstructionBits() & (kRvvRs2Mask))) {
      switch (instr_.vl_vs_width()) {
        case 8: {
          RVV_VI_LD(0, (i * nf + fn), int8, false);
          break;
        }
        case 16: {
          UNIMPLEMENTED_RISCV();
          break;
        }
        default:
          UNIMPLEMENTED_RISCV();
          break;
      }
      return true;
    } else {
      UNIMPLEMENTED_RISCV();
      return true;
    }
  } else if (RO_V_VLS == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VLX == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VLSEG2 == instr_temp || RO_V_VLSEG3 == instr_temp ||
             RO_V_VLSEG4 == instr_temp || RO_V_VLSEG5 == instr_temp ||
             RO_V_VLSEG6 == instr_temp || RO_V_VLSEG7 == instr_temp ||
             RO_V_VLSEG8 == instr_temp) {
    if (!(instr_.InstructionBits() & (kRvvRs2Mask))) {
      UNIMPLEMENTED_RISCV();
      return true;
    } else {
      UNIMPLEMENTED_RISCV();
      return true;
    }
  } else if (RO_V_VLSSEG2 == instr_temp || RO_V_VLSSEG3 == instr_temp ||
             RO_V_VLSSEG4 == instr_temp || RO_V_VLSSEG5 == instr_temp ||
             RO_V_VLSSEG6 == instr_temp || RO_V_VLSSEG7 == instr_temp ||
             RO_V_VLSSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VLXSEG2 == instr_temp || RO_V_VLXSEG3 == instr_temp ||
             RO_V_VLXSEG4 == instr_temp || RO_V_VLXSEG5 == instr_temp ||
             RO_V_VLXSEG6 == instr_temp || RO_V_VLXSEG7 == instr_temp ||
             RO_V_VLXSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else {
    return false;
  }
}

bool Simulator::DecodeRvvVS() {
  uint32_t instr_temp =
      instr_.InstructionBits() & (kRvvMopMask | kRvvNfMask | kBaseOpcodeMask);
  if (RO_V_VS == instr_temp) {
    if (!(instr_.InstructionBits() & (kRvvRs2Mask))) {
      switch (instr_.vl_vs_width()) {
        case 8: {
          RVV_VI_ST(0, (i * nf + fn), uint8, false);
          break;
        }
        case 16: {
          UNIMPLEMENTED_RISCV();
          break;
        }
        default:
          UNIMPLEMENTED_RISCV();
          break;
      }
    } else {
      UNIMPLEMENTED_RISCV();
    }
    return true;
  } else if (RO_V_VSS == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSX == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSU == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSSEG2 == instr_temp || RO_V_VSSEG3 == instr_temp ||
             RO_V_VSSEG4 == instr_temp || RO_V_VSSEG5 == instr_temp ||
             RO_V_VSSEG6 == instr_temp || RO_V_VSSEG7 == instr_temp ||
             RO_V_VSSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSSSEG2 == instr_temp || RO_V_VSSSEG3 == instr_temp ||
             RO_V_VSSSEG4 == instr_temp || RO_V_VSSSEG5 == instr_temp ||
             RO_V_VSSSEG6 == instr_temp || RO_V_VSSSEG7 == instr_temp ||
             RO_V_VSSSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSXSEG2 == instr_temp || RO_V_VSXSEG3 == instr_temp ||
             RO_V_VSXSEG4 == instr_temp || RO_V_VSXSEG5 == instr_temp ||
             RO_V_VSXSEG6 == instr_temp || RO_V_VSXSEG7 == instr_temp ||
             RO_V_VSXSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else {
    return false;
  }
}

Builtin Simulator::LookUp(Address pc) {
  for (Builtin builtin = Builtins::kFirst; builtin <= Builtins::kLast;
       ++builtin) {
    if (builtins_.code(builtin).contains(isolate_, pc)) return builtin;
  }
  return Builtin::kNoBuiltinId;
}

void Simulator::DecodeRVIType() {
  switch (instr_.InstructionBits() & kITypeMask) {
    case RO_JALR: {
      set_rd(get_pc() + kInstrSize);
      // Note: No need to shift 2 for JALR's imm12, but set lowest bit to 0.
      int64_t next_pc = (rs1() + imm12()) & ~reg_t(1);
      set_pc(next_pc);
      if (::v8::internal::FLAG_trace_sim) {
        Builtin builtin = LookUp((Address)get_pc());
        if (builtin != Builtin::kNoBuiltinId) {
          auto code = builtins_.code(builtin);
          if ((rs1_reg() != ra || imm12() != 0)) {
            if ((Address)get_pc() == code.InstructionStart()) {
              int64_t arg0 = get_register(a0);
              int64_t arg1 = get_register(a1);
              int64_t arg2 = get_register(a2);
              int64_t arg3 = get_register(a3);
              int64_t arg4 = get_register(a4);
              int64_t arg5 = get_register(a5);
              int64_t arg6 = get_register(a6);
              int64_t arg7 = get_register(a7);
              int64_t* stack_pointer =
                  reinterpret_cast<int64_t*>(get_register(sp));
              int64_t arg8 = stack_pointer[0];
              int64_t arg9 = stack_pointer[1];
              PrintF(
                  "Call to Builtin at %s "
                  "a0 %08" PRIx64 " ,a1 %08" PRIx64 " ,a2 %08" PRIx64
                  " ,a3 %08" PRIx64 " ,a4 %08" PRIx64 " ,a5 %08" PRIx64
                  " ,a6 %08" PRIx64 " ,a7 %08" PRIx64 " ,0(sp) %08" PRIx64
                  " ,8(sp) %08" PRIx64 " ,sp %08" PRIx64 ",fp %08" PRIx64 " \n",
                  builtins_.name(builtin), arg0, arg1, arg2, arg3, arg4, arg5,
                  arg6, arg7, arg8, arg9, get_register(sp), get_register(fp));
            }
          } else if (rd_reg() == zero_reg) {
            PrintF("Return to Builtin at %s \n", builtins_.name(builtin));
          }
        }
      }
      break;
    }
    case RO_LB: {
      int64_t addr = rs1() + imm12();
      int8_t val = ReadMem<int8_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      break;
    }
    case RO_LH: {
      int64_t addr = rs1() + imm12();
      int16_t val = ReadMem<int16_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      break;
    }
    case RO_LW: {
      int64_t addr = rs1() + imm12();
      int32_t val = ReadMem<int32_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      break;
    }
    case RO_LBU: {
      int64_t addr = rs1() + imm12();
      uint8_t val = ReadMem<uint8_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      break;
    }
    case RO_LHU: {
      int64_t addr = rs1() + imm12();
      uint16_t val = ReadMem<uint16_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_LWU: {
      int64_t addr = rs1() + imm12();
      uint32_t val = ReadMem<uint32_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      break;
    }
    case RO_LD: {
      int64_t addr = rs1() + imm12();
      int64_t val = ReadMem<int64_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rd_reg()));
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
    case RO_ADDI: {
      set_rd(sext_xlen(rs1() + imm12()));
      break;
    }
    case RO_SLTI: {
      set_rd(sreg_t(rs1()) < sreg_t(imm12()));
      break;
    }
    case RO_SLTIU: {
      set_rd(reg_t(rs1()) < reg_t(imm12()));
      break;
    }
    case RO_XORI: {
      set_rd(imm12() ^ rs1());
      break;
    }
    case RO_ORI: {
      set_rd(imm12() | rs1());
      break;
    }
    case RO_ANDI: {
      set_rd(imm12() & rs1());
      break;
    }
    case RO_SLLI: {
      require(shamt6() < xlen);
      set_rd(sext_xlen(rs1() << shamt6()));
      break;
    }
    case RO_SRLI: {  //  RO_SRAI
      if (!instr_.IsArithShift()) {
        require(shamt6() < xlen);
        set_rd(sext_xlen(zext_xlen(rs1()) >> shamt6()));
      } else {
        require(shamt6() < xlen);
        set_rd(sext_xlen(sext_xlen(rs1()) >> shamt6()));
      }
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_ADDIW: {
      set_rd(sext32(rs1() + imm12()));
      break;
    }
    case RO_SLLIW: {
      set_rd(sext32(rs1() << shamt5()));
      break;
    }
    case RO_SRLIW: {  //  RO_SRAIW
      if (!instr_.IsArithShift()) {
        set_rd(sext32(uint32_t(rs1()) >> shamt5()));
      } else {
        set_rd(sext32(int32_t(rs1()) >> shamt5()));
      }
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
    case RO_FENCE: {
      // DO nothing in sumulator
      break;
    }
    case RO_ECALL: {                   // RO_EBREAK
      if (instr_.Imm12Value() == 0) {  // ECALL
        SoftwareInterrupt();
      } else if (instr_.Imm12Value() == 1) {  // EBREAK
        SoftwareInterrupt();
      } else {
        UNSUPPORTED();
      }
      break;
    }
      // TODO(riscv): use Zifencei Standard Extension macro block
    case RO_FENCE_I: {
      // spike: flush icache.
      break;
    }
      // TODO(riscv): use Zicsr Standard Extension macro block
    case RO_CSRRW: {
      if (rd_reg() != zero_reg) {
        set_rd(zext_xlen(read_csr_value(csr_reg())));
      }
      write_csr_value(csr_reg(), rs1());
      break;
    }
    case RO_CSRRS: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (rs1_reg() != zero_reg) {
        set_csr_bits(csr_reg(), rs1());
      }
      break;
    }
    case RO_CSRRC: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (rs1_reg() != zero_reg) {
        clear_csr_bits(csr_reg(), rs1());
      }
      break;
    }
    case RO_CSRRWI: {
      if (rd_reg() != zero_reg) {
        set_rd(zext_xlen(read_csr_value(csr_reg())));
      }
      write_csr_value(csr_reg(), imm5CSR());
      break;
    }
    case RO_CSRRSI: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (imm5CSR() != 0) {
        set_csr_bits(csr_reg(), imm5CSR());
      }
      break;
    }
    case RO_CSRRCI: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (imm5CSR() != 0) {
        clear_csr_bits(csr_reg(), imm5CSR());
      }
      break;
    }
    // TODO(riscv): use F Extension macro block
    case RO_FLW: {
      int64_t addr = rs1() + imm12();
      float val = ReadMem<float>(addr, instr_.instr());
      set_frd(val, false);
      TraceMemRd(addr, val, get_fpu_register(frd_reg()));
      break;
    }
    // TODO(riscv): use D Extension macro block
    case RO_FLD: {
      int64_t addr = rs1() + imm12();
      double val = ReadMem<double>(addr, instr_.instr());
      set_drd(val, false);
      TraceMemRd(addr, val, get_fpu_register(frd_reg()));
      break;
    }
    default: {
      if (!DecodeRvvVL()) {
        UNSUPPORTED();
      }
      break;
    }
  }
}

void Simulator::DecodeRVSType() {
  switch (instr_.InstructionBits() & kSTypeMask) {
    case RO_SB:
      WriteMem<uint8_t>(rs1() + s_imm12(), (uint8_t)rs2(), instr_.instr());
      break;
    case RO_SH:
      WriteMem<uint16_t>(rs1() + s_imm12(), (uint16_t)rs2(), instr_.instr());
      break;
    case RO_SW:
      WriteMem<uint32_t>(rs1() + s_imm12(), (uint32_t)rs2(), instr_.instr());
      break;
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_SD:
      WriteMem<uint64_t>(rs1() + s_imm12(), (uint64_t)rs2(), instr_.instr());
      break;
#endif /*V8_TARGET_ARCH_64_BIT*/
    // TODO(riscv): use F Extension macro block
    case RO_FSW: {
      WriteMem<uint32_t>(rs1() + s_imm12(),
                         (uint32_t)get_fpu_register_word(rs2_reg()),
                         instr_.instr());
      break;
    }
    // TODO(riscv): use D Extension macro block
    case RO_FSD: {
      WriteMem<double>(rs1() + s_imm12(), drs2(), instr_.instr());
      break;
    }
    default:
      if (!DecodeRvvVS()) {
        UNSUPPORTED();
      }
      break;
  }
}

void Simulator::DecodeRVBType() {
  switch (instr_.InstructionBits() & kBTypeMask) {
    case RO_BEQ:
      if (rs1() == rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BNE:
      if (rs1() != rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BLT:
      if (rs1() < rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BGE:
      if (rs1() >= rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BLTU:
      if ((reg_t)rs1() < (reg_t)rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BGEU:
      if ((reg_t)rs1() >= (reg_t)rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeRVUType() {
  // U Type doesn't have additoinal mask
  switch (instr_.BaseOpcodeFieldRaw()) {
    case RO_LUI:
      set_rd(u_imm20());
      break;
    case RO_AUIPC:
      set_rd(sext_xlen(u_imm20() + get_pc()));
      break;
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeRVJType() {
  // J Type doesn't have additional mask
  switch (instr_.BaseOpcodeValue()) {
    case RO_JAL: {
      set_rd(get_pc() + kInstrSize);
      int64_t next_pc = get_pc() + imm20J();
      set_pc(next_pc);
      break;
    }
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeCRType() {
  switch (instr_.RvcFunct4Value()) {
    case 0b1000:
      if (instr_.RvcRs1Value() != 0 && instr_.RvcRs2Value() == 0) {  // c.jr
        set_pc(rvc_rs1());
      } else if (instr_.RvcRdValue() != 0 &&
                 instr_.RvcRs2Value() != 0) {  // c.mv
        set_rvc_rd(sext_xlen(rvc_rs2()));
      } else {
        UNSUPPORTED_RISCV();
      }
      break;
    case 0b1001:
      if (instr_.RvcRs1Value() == 0 && instr_.RvcRs2Value() == 0) {  // c.ebreak
        RiscvDebugger dbg(this);
        dbg.Debug();
      } else if (instr_.RvcRdValue() != 0 &&
                 instr_.RvcRs2Value() == 0) {  // c.jalr
        set_register(ra, get_pc() + kShortInstrSize);
        set_pc(rvc_rs1());
      } else if (instr_.RvcRdValue() != 0 &&
                 instr_.RvcRs2Value() != 0) {  // c.add
        set_rvc_rd(sext_xlen(rvc_rs1() + rvc_rs2()));
      } else {
        UNSUPPORTED();
      }
      break;
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCAType() {
  switch (instr_.InstructionBits() & kCATypeMask) {
    case RO_C_SUB:
      set_rvc_rs1s(sext_xlen(rvc_rs1s() - rvc_rs2s()));
      break;
    case RO_C_XOR:
      set_rvc_rs1s(rvc_rs1s() ^ rvc_rs2s());
      break;
    case RO_C_OR:
      set_rvc_rs1s(rvc_rs1s() | rvc_rs2s());
      break;
    case RO_C_AND:
      set_rvc_rs1s(rvc_rs1s() & rvc_rs2s());
      break;
    case RO_C_SUBW:
      set_rvc_rs1s(sext32(rvc_rs1s() - rvc_rs2s()));
      break;
    case RO_C_ADDW:
      set_rvc_rs1s(sext32(rvc_rs1s() + rvc_rs2s()));
      break;
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCIType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_NOP_ADDI:
      if (instr_.RvcRdValue() == 0)  // c.nop
        break;
      else  // c.addi
        set_rvc_rd(sext_xlen(rvc_rs1() + rvc_imm6()));
      break;
    case RO_C_ADDIW:
      set_rvc_rd(sext32(rvc_rs1() + rvc_imm6()));
      break;
    case RO_C_LI:
      set_rvc_rd(sext_xlen(rvc_imm6()));
      break;
    case RO_C_LUI_ADD:
      if (instr_.RvcRdValue() == 2) {
        // c.addi16sp
        int64_t value = get_register(sp) + rvc_imm6_addi16sp();
        set_register(sp, value);
      } else if (instr_.RvcRdValue() != 0 && instr_.RvcRdValue() != 2) {
        // c.lui
        set_rvc_rd(rvc_u_imm6());
      } else {
        UNSUPPORTED();
      }
      break;
    case RO_C_SLLI:
      set_rvc_rd(sext_xlen(rvc_rs1() << rvc_shamt6()));
      break;
    case RO_C_FLDSP: {
      int64_t addr = get_register(sp) + rvc_imm6_ldsp();
      double val = ReadMem<double>(addr, instr_.instr());
      set_rvc_drd(val, false);
      TraceMemRd(addr, val, get_fpu_register(rvc_frd_reg()));
      break;
    }
    case RO_C_LWSP: {
      int64_t addr = get_register(sp) + rvc_imm6_lwsp();
      int64_t val = ReadMem<int32_t>(addr, instr_.instr());
      set_rvc_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rvc_rd_reg()));
      break;
    }
    case RO_C_LDSP: {
      int64_t addr = get_register(sp) + rvc_imm6_ldsp();
      int64_t val = ReadMem<int64_t>(addr, instr_.instr());
      set_rvc_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rvc_rd_reg()));
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCIWType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_ADDI4SPN: {
      set_rvc_rs2s(get_register(sp) + rvc_imm8_addi4spn());
      break;
      default:
        UNSUPPORTED();
    }
  }
}

void Simulator::DecodeCSSType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_FSDSP: {
      int64_t addr = get_register(sp) + rvc_imm6_sdsp();
      WriteMem<double>(addr, static_cast<double>(rvc_drs2()), instr_.instr());
      break;
    }
    case RO_C_SWSP: {
      int64_t addr = get_register(sp) + rvc_imm6_swsp();
      WriteMem<int32_t>(addr, (int32_t)rvc_rs2(), instr_.instr());
      break;
    }
    case RO_C_SDSP: {
      int64_t addr = get_register(sp) + rvc_imm6_sdsp();
      WriteMem<int64_t>(addr, (int64_t)rvc_rs2(), instr_.instr());
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCLType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_LW: {
      int64_t addr = rvc_rs1s() + rvc_imm5_w();
      int64_t val = ReadMem<int32_t>(addr, instr_.instr());
      set_rvc_rs2s(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rvc_rs2s_reg()));
      break;
    }
    case RO_C_LD: {
      int64_t addr = rvc_rs1s() + rvc_imm5_d();
      int64_t val = ReadMem<int64_t>(addr, instr_.instr());
      set_rvc_rs2s(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(rvc_rs2s_reg()));
      break;
    }
    case RO_C_FLD: {
      int64_t addr = rvc_rs1s() + rvc_imm5_d();
      double val = ReadMem<double>(addr, instr_.instr());
      set_rvc_drs2s(val, false);
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCSType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_SW: {
      int64_t addr = rvc_rs1s() + rvc_imm5_w();
      WriteMem<int32_t>(addr, (int32_t)rvc_rs2s(), instr_.instr());
      break;
    }
    case RO_C_SD: {
      int64_t addr = rvc_rs1s() + rvc_imm5_d();
      WriteMem<int64_t>(addr, (int64_t)rvc_rs2s(), instr_.instr());
      break;
    }
    case RO_C_FSD: {
      int64_t addr = rvc_rs1s() + rvc_imm5_d();
      WriteMem<double>(addr, static_cast<double>(rvc_drs2s()), instr_.instr());
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCJType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_J: {
      set_pc(get_pc() + instr_.RvcImm11CJValue());
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCBType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_BNEZ:
      if (rvc_rs1() != 0) {
        int64_t next_pc = get_pc() + rvc_imm8_b();
        set_pc(next_pc);
      }
      break;
    case RO_C_BEQZ:
      if (rvc_rs1() == 0) {
        int64_t next_pc = get_pc() + rvc_imm8_b();
        set_pc(next_pc);
      }
      break;
    case RO_C_MISC_ALU:
      if (instr_.RvcFunct2BValue() == 0b00) {  // c.srli
        set_rvc_rs1s(sext_xlen(sext_xlen(rvc_rs1s()) >> rvc_shamt6()));
      } else if (instr_.RvcFunct2BValue() == 0b01) {  // c.srai
        require(rvc_shamt6() < xlen);
        set_rvc_rs1s(sext_xlen(sext_xlen(rvc_rs1s()) >> rvc_shamt6()));
      } else if (instr_.RvcFunct2BValue() == 0b10) {  // c.andi
        set_rvc_rs1s(rvc_imm6() & rvc_rs1s());
      } else {
        UNSUPPORTED();
      }
      break;
    default:
      UNSUPPORTED();
  }
}

/**
 * RISCV-ISA-SIM
 *
 * @link      https://github.com/riscv/riscv-isa-sim/
 * @copyright Copyright (c)  The Regents of the University of California
 * @license   hhttps://github.com/riscv/riscv-isa-sim/blob/master/LICENSE
 */
// ref:  https://locklessinc.com/articles/sat_arithmetic/
template <typename T, typename UT>
static inline T sat_add(T x, T y, bool& sat) {
  UT ux = x;
  UT uy = y;
  UT res = ux + uy;
  sat = false;
  int sh = sizeof(T) * 8 - 1;

  /* Calculate overflowed result. (Don't change the sign bit of ux) */
  ux = (ux >> sh) + (((UT)0x1 << sh) - 1);

  /* Force compiler to use cmovns instruction */
  if ((T)((ux ^ uy) | ~(uy ^ res)) >= 0) {
    res = ux;
    sat = true;
  }

  return res;
}

template <typename T, typename UT>
static inline T sat_sub(T x, T y, bool& sat) {
  UT ux = x;
  UT uy = y;
  UT res = ux - uy;
  sat = false;
  int sh = sizeof(T) * 8 - 1;

  /* Calculate overflowed result. (Don't change the sign bit of ux) */
  ux = (ux >> sh) + (((UT)0x1 << sh) - 1);

  /* Force compiler to use cmovns instruction */
  if ((T)((ux ^ uy) & (ux ^ res)) < 0) {
    res = ux;
    sat = true;
  }

  return res;
}

void Simulator::DecodeRvvIVV() {
  DCHECK_EQ(instr_.InstructionBits() & (kBaseOpcodeMask | kFunct3Mask), OP_IVV);
  switch (instr_.InstructionBits() & kVTypeMask) {
    case RO_V_VADD_VV: {
      RVV_VI_VV_LOOP({ vd = vs1 + vs2; });
      break;
    }
    case RO_V_VSADD_VV: {
      RVV_VI_GENERAL_LOOP_BASE
      bool sat = false;
      switch (rvv_vsew()) {
        case E8: {
          VV_PARAMS(8);
          vd = sat_add<int8_t, uint8_t>(vs2, vs1, sat);
          break;
        }
        case E16: {
          VV_PARAMS(16);
          vd = sat_add<int16_t, uint16_t>(vs2, vs1, sat);
          break;
        }
        case E32: {
          VV_PARAMS(32);
          vd = sat_add<int32_t, uint32_t>(vs2, vs1, sat);
          break;
        }
        default: {
          VV_PARAMS(64);
          vd = sat_add<int64_t, uint64_t>(vs2, vs1, sat);
          break;
        }
      }
      set_rvv_vxsat(sat);
      RVV_VI_LOOP_END
      break;
    }
    case RO_V_VSUB_VV: {
      RVV_VI_VV_LOOP({ vd = vs2 - vs1; })
      break;
    }
    case RO_V_VSSUB_VV: {
      RVV_VI_GENERAL_LOOP_BASE
      bool sat = false;
      switch (rvv_vsew()) {
        case E8: {
          VV_PARAMS(8);
          vd = sat_sub<int8_t, uint8_t>(vs2, vs1, sat);
          break;
        }
        case E16: {
          VV_PARAMS(16);
          vd = sat_sub<int16_t, uint16_t>(vs2, vs1, sat);
          break;
        }
        case E32: {
          VV_PARAMS(32);
          vd = sat_sub<int32_t, uint32_t>(vs2, vs1, sat);
          break;
        }
        default: {
          VV_PARAMS(64);
          vd = sat_sub<int64_t, uint64_t>(vs2, vs1, sat);
          break;
        }
      }
      set_rvv_vxsat(sat);
      RVV_VI_LOOP_END
      break;
    }
    case RO_V_VAND_VV: {
      RVV_VI_VV_LOOP({ vd = vs1 & vs2; })
      break;
    }
    case RO_V_VOR_VV: {
      RVV_VI_VV_LOOP({ vd = vs1 | vs2; })
      break;
    }
    case RO_V_VXOR_VV: {
      RVV_VI_VV_LOOP({ vd = vs1 ^ vs2; })
      break;
    }
    case RO_V_VMAXU_VV: {
      RVV_VI_VV_ULOOP({
        if (vs1 <= vs2) {
          vd = vs2;
        } else {
          vd = vs1;
        }
      })
      break;
    }
    case RO_V_VMAX_VV: {
      RVV_VI_VV_LOOP({
        if (vs1 <= vs2) {
          vd = vs2;
        } else {
          vd = vs1;
        }
      })
      break;
    }
    case RO_V_VMINU_VV: {
      RVV_VI_VV_ULOOP({
        if (vs1 <= vs2) {
          vd = vs1;
        } else {
          vd = vs2;
        }
      })
      break;
    }
    case RO_V_VMIN_VV: {
      RVV_VI_VV_LOOP({
        if (vs1 <= vs2) {
          vd = vs1;
        } else {
          vd = vs2;
        }
      })
      break;
    }
    case RO_V_VMV_VV: {
      if (instr_.RvvVM()) {
        RVV_VI_VVXI_MERGE_LOOP({
          vd = vs1;
          USE(simm5);
          USE(vs2);
          USE(rs1);
        });
      } else {
        RVV_VI_VVXI_MERGE_LOOP({
          bool use_first = (Rvvelt<uint64_t>(0, (i / 64)) >> (i % 64)) & 0x1;
          vd = use_first ? vs1 : vs2;
          USE(simm5);
          USE(rs1);
        });
      }
      break;
    }
    case RO_V_VMSEQ_VV: {
      RVV_VI_VV_LOOP_CMP({ res = vs1 == vs2; })
      break;
    }
    case RO_V_VMSNE_VV: {
      RVV_VI_VV_LOOP_CMP({ res = vs1 != vs2; })
      break;
    }
    case RO_V_VMSLTU_VV: {
      RVV_VI_VV_ULOOP_CMP({ res = vs2 < vs1; })
      break;
    }
    case RO_V_VMSLT_VV: {
      RVV_VI_VV_LOOP_CMP({ res = vs2 < vs1; })
      break;
    }
    case RO_V_VMSLE_VV: {
      RVV_VI_VV_LOOP_CMP({ res = vs2 <= vs1; })
      break;
    }
    case RO_V_VMSLEU_VV: {
      RVV_VI_VV_ULOOP_CMP({ res = vs2 <= vs1; })
      break;
    }
    case RO_V_VADC_VV:
      if (instr_.RvvVM()) {
        RVV_VI_VV_LOOP_WITH_CARRY({
          auto& v0 = Rvvelt<uint64_t>(0, midx);
          vd = vs1 + vs2 + (v0 >> mpos) & 0x1;
        })
      } else {
        UNREACHABLE();
      }
      break;
    case RO_V_VSLL_VV: {
      RVV_VI_VV_LOOP({ vd = vs2 << vs1; })
      break;
    }
    case RO_V_VRGATHER_VV: {
      RVV_VI_GENERAL_LOOP_BASE
      switch (rvv_vsew()) {
        case E8: {
          auto vs1 = Rvvelt<uint8_t>(rvv_vs1_reg(), i);
          // if (i > 255) continue;
          Rvvelt<uint8_t>(rvv_vd_reg(), i, true) =
              vs1 >= rvv_vlmax() ? 0 : Rvvelt<uint8_t>(rvv_vs2_reg(), vs1);
          break;
        }
        case E16: {
          auto vs1 = Rvvelt<uint16_t>(rvv_vs1_reg(), i);
          Rvvelt<uint16_t>(rvv_vd_reg(), i, true) =
              vs1 >= rvv_vlmax() ? 0 : Rvvelt<uint16_t>(rvv_vs2_reg(), vs1);
          break;
        }
        case E32: {
          auto vs1 = Rvvelt<uint32_t>(rvv_vs1_reg(), i);
          Rvvelt<uint32_t>(rvv_vd_reg(), i, true) =
              vs1 >= rvv_vlmax() ? 0 : Rvvelt<uint32_t>(rvv_vs2_reg(), vs1);
          break;
        }
        default: {
          auto vs1 = Rvvelt<uint64_t>(rvv_vs1_reg(), i);
          Rvvelt<uint64_t>(rvv_vd_reg(), i, true) =
              vs1 >= rvv_vlmax() ? 0 : Rvvelt<uint64_t>(rvv_vs2_reg(), vs1);
          break;
        }
      }
      RVV_VI_LOOP_END;
      break;
    }
    default:
      // v8::base::EmbeddedVector<char, 256> buffer;
      // SNPrintF(trace_buf_, " ");
      // disasm::NameConverter converter;
      // disasm::Disassembler dasm(converter);
      // // Use a reasonably large buffer.
      // dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(&instr_));

      // PrintF("EXECUTING  0x%08" PRIxPTR "   %-44s\n",
      //        reinterpret_cast<intptr_t>(&instr_), buffer.begin());
      UNIMPLEMENTED_RISCV();
      break;
  }
  set_rvv_vstart(0);
}

void Simulator::DecodeRvvIVI() {
  DCHECK_EQ(instr_.InstructionBits() & (kBaseOpcodeMask | kFunct3Mask), OP_IVI);
  switch (instr_.InstructionBits() & kVTypeMask) {
    case RO_V_VADD_VI: {
      RVV_VI_VI_LOOP({ vd = simm5 + vs2; })
      break;
    }
    case RO_V_VSADD_VI: {
      RVV_VI_GENERAL_LOOP_BASE
      bool sat = false;
      switch (rvv_vsew()) {
        case E8: {
          VI_PARAMS(8);
          vd = sat_add<int8_t, uint8_t>(vs2, simm5, sat);
          break;
        }
        case E16: {
          VI_PARAMS(16);
          vd = sat_add<int16_t, uint16_t>(vs2, simm5, sat);
          break;
        }
        case E32: {
          VI_PARAMS(32);
          vd = sat_add<int32_t, uint32_t>(vs2, simm5, sat);
          break;
        }
        default: {
          VI_PARAMS(64);
          vd = sat_add<int64_t, uint64_t>(vs2, simm5, sat);
          break;
        }
      }
      set_rvv_vxsat(sat);
      RVV_VI_LOOP_END
      break;
    }
    case RO_V_VRSUB_VI: {
      RVV_VI_VI_LOOP({ vd = vs2 - simm5; })
      break;
    }
    case RO_V_VAND_VI: {
      RVV_VI_VI_LOOP({ vd = simm5 & vs2; })
      break;
    }
    case RO_V_VOR_VI: {
      RVV_VI_VI_LOOP({ vd = simm5 | vs2; })
      break;
    }
    case RO_V_VXOR_VI: {
      RVV_VI_VI_LOOP({ vd = simm5 ^ vs2; })
      break;
    }
    case RO_V_VMV_VI:
      if (instr_.RvvVM()) {
        RVV_VI_VVXI_MERGE_LOOP({
          vd = simm5;
          USE(vs1);
          USE(vs2);
          USE(rs1);
        });
      } else {
        RVV_VI_VVXI_MERGE_LOOP({
          bool use_first = (Rvvelt<uint64_t>(0, (i / 64)) >> (i % 64)) & 0x1;
          vd = use_first ? simm5 : vs2;
          USE(vs1);
          USE(rs1);
        });
      }
      break;
    case RO_V_VMSEQ_VI:
      RVV_VI_VI_LOOP_CMP({ res = simm5 == vs2; })
      break;
    case RO_V_VMSNE_VI:
      RVV_VI_VI_LOOP_CMP({ res = simm5 != vs2; })
      break;
    case RO_V_VMSLEU_VI:
      RVV_VI_VI_ULOOP_CMP({ res = vs2 <= uimm5; })
      break;
    case RO_V_VMSLE_VI:
      RVV_VI_VI_LOOP_CMP({ res = vs2 <= simm5; })
      break;
    case RO_V_VMSGT_VI:
      RVV_VI_VI_LOOP_CMP({ res = vs2 > simm5; })
      break;
    case RO_V_VSLIDEDOWN_VI: {
      const uint8_t sh = instr_.RvvUimm5();
      RVV_VI_GENERAL_LOOP_BASE

      reg_t offset = 0;
      bool is_valid = (i + sh) < rvv_vlmax();

      if (is_valid) {
        offset = sh;
      }

      switch (rvv_sew()) {
        case E8: {
          VI_XI_SLIDEDOWN_PARAMS(8, offset);
          vd = is_valid ? vs2 : 0;
        } break;
        case E16: {
          VI_XI_SLIDEDOWN_PARAMS(16, offset);
          vd = is_valid ? vs2 : 0;
        } break;
        case E32: {
          VI_XI_SLIDEDOWN_PARAMS(32, offset);
          vd = is_valid ? vs2 : 0;
        } break;
        default: {
          VI_XI_SLIDEDOWN_PARAMS(64, offset);
          vd = is_valid ? vs2 : 0;
        } break;
      }
      RVV_VI_LOOP_END
    } break;
    case RO_V_VSRL_VI:
      RVV_VI_VI_LOOP({ vd = vs2 >> simm5; })
      break;
    case RO_V_VSLL_VI:
      RVV_VI_VI_LOOP({ vd = vs2 << simm5; })
      break;
    case RO_V_VADC_VI:
      if (instr_.RvvVM()) {
        RVV_VI_XI_LOOP_WITH_CARRY({
          auto& v0 = Rvvelt<uint64_t>(0, midx);
          vd = simm5 + vs2 + (v0 >> mpos) & 0x1;
          USE(rs1);
        })
      } else {
        UNREACHABLE();
      }
      break;
    default:
      UNIMPLEMENTED_RISCV();
      break;
  }
}

void Simulator::DecodeRvvIVX() {
  DCHECK_EQ(instr_.InstructionBits() & (kBaseOpcodeMask | kFunct3Mask), OP_IVX);
  switch (instr_.InstructionBits() & kVTypeMask) {
    case RO_V_VADD_VX: {
      RVV_VI_VX_LOOP({ vd = rs1 + vs2; })
      break;
    }
    case RO_V_VSADD_VX: {
      RVV_VI_GENERAL_LOOP_BASE
      bool sat = false;
      switch (rvv_vsew()) {
        case E8: {
          VX_PARAMS(8);
          vd = sat_add<int8_t, uint8_t>(vs2, rs1, sat);
          break;
        }
        case E16: {
          VX_PARAMS(16);
          vd = sat_add<int16_t, uint16_t>(vs2, rs1, sat);
          break;
        }
        case E32: {
          VX_PARAMS(32);
          vd = sat_add<int32_t, uint32_t>(vs2, rs1, sat);
          break;
        }
        default: {
          VX_PARAMS(64);
          vd = sat_add<int64_t, uint64_t>(vs2, rs1, sat);
          break;
        }
      }
      set_rvv_vxsat(sat);
      RVV_VI_LOOP_END
      break;
    }
    case RO_V_VSUB_VX: {
      RVV_VI_VX_LOOP({ vd = vs2 - rs1; })
      break;
    }
    case RO_V_VSSUB_VX: {
      RVV_VI_GENERAL_LOOP_BASE
      bool sat = false;
      switch (rvv_vsew()) {
        case E8: {
          VX_PARAMS(8);
          vd = sat_sub<int8_t, uint8_t>(vs2, rs1, sat);
          break;
        }
        case E16: {
          VX_PARAMS(16);
          vd = sat_sub<int16_t, uint16_t>(vs2, rs1, sat);
          break;
        }
        case E32: {
          VX_PARAMS(32);
          vd = sat_sub<int32_t, uint32_t>(vs2, rs1, sat);
          break;
        }
        default: {
          VX_PARAMS(64);
          vd = sat_sub<int64_t, uint64_t>(vs2, rs1, sat);
          break;
        }
      }
      set_rvv_vxsat(sat);
      RVV_VI_LOOP_END
      break;
    }
    case RO_V_VRSUB_VX: {
      RVV_VI_VX_LOOP({ vd = rs1 - vs2; })
      break;
    }
    case RO_V_VAND_VX: {
      RVV_VI_VX_LOOP({ vd = rs1 & vs2; })
      break;
    }
    case RO_V_VOR_VX: {
      RVV_VI_VX_LOOP({ vd = rs1 | vs2; })
      break;
    }
    case RO_V_VXOR_VX: {
      RVV_VI_VX_LOOP({ vd = rs1 ^ vs2; })
      break;
    }
    case RO_V_VMAX_VX: {
      RVV_VI_VX_LOOP({
        if (rs1 <= vs2) {
          vd = vs2;
        } else {
          vd = rs1;
        }
      })
      break;
    }
    case RO_V_VMAXU_VX: {
      RVV_VI_VX_ULOOP({
        if (rs1 <= vs2) {
          vd = vs2;
        } else {
          vd = rs1;
        }
      })
      break;
    }
    case RO_V_VMINU_VX: {
      RVV_VI_VX_ULOOP({
        if (rs1 <= vs2) {
          vd = rs1;
        } else {
          vd = vs2;
        }
      })
      break;
    }
    case RO_V_VMIN_VX: {
      RVV_VI_VX_LOOP({
        if (rs1 <= vs2) {
          vd = rs1;
        } else {
          vd = vs2;
        }
      })
      break;
    }
    case RO_V_VMV_VX:
      if (instr_.RvvVM()) {
        RVV_VI_VVXI_MERGE_LOOP({
          vd = rs1;
          USE(vs1);
          USE(vs2);
          USE(simm5);
        });
      } else {
        RVV_VI_VVXI_MERGE_LOOP({
          bool use_first = (Rvvelt<uint64_t>(0, (i / 64)) >> (i % 64)) & 0x1;
          vd = use_first ? rs1 : vs2;
          USE(vs1);
          USE(simm5);
        });
      }
      break;
    case RO_V_VMSEQ_VX:
      RVV_VI_VX_LOOP_CMP({ res = vs2 == rs1; })
      break;
    case RO_V_VMSNE_VX:
      RVV_VI_VX_LOOP_CMP({ res = vs2 != rs1; })
      break;
    case RO_V_VMSLT_VX:
      RVV_VI_VX_LOOP_CMP({ res = vs2 < rs1; })
      break;
    case RO_V_VMSLTU_VX:
      RVV_VI_VX_ULOOP_CMP({ res = vs2 < rs1; })
      break;
    case RO_V_VMSLE_VX:
      RVV_VI_VX_LOOP_CMP({ res = vs2 <= rs1; })
      break;
    case RO_V_VMSLEU_VX:
      RVV_VI_VX_ULOOP_CMP({ res = vs2 <= rs1; })
      break;
    case RO_V_VMSGT_VX:
      RVV_VI_VX_LOOP_CMP({ res = vs2 > rs1; })
      break;
    case RO_V_VMSGTU_VX:
      RVV_VI_VX_ULOOP_CMP({ res = vs2 > rs1; })
      break;
    case RO_V_VSLIDEDOWN_VX:
      UNIMPLEMENTED_RISCV();
      break;
    case RO_V_VADC_VX:
      if (instr_.RvvVM()) {
        RVV_VI_XI_LOOP_WITH_CARRY({
          auto& v0 = Rvvelt<uint64_t>(0, midx);
          vd = rs1 + vs2 + (v0 >> mpos) & 0x1;
          USE(simm5);
        })
      } else {
        UNREACHABLE();
      }
      break;
    case RO_V_VSLL_VX: {
      RVV_VI_VX_LOOP({ vd = vs2 << rs1; })
      break;
    }
    case RO_V_VSRL_VX: {
      RVV_VI_VX_LOOP({ vd = int32_t(uint32_t(vs2) >> (rs1 & (xlen - 1))); })
      break;
    }
    default:
      UNIMPLEMENTED_RISCV();
      break;
  }
}

void Simulator::DecodeRvvMVV() {
  DCHECK_EQ(instr_.InstructionBits() & (kBaseOpcodeMask | kFunct3Mask), OP_MVV);
  switch (instr_.InstructionBits() & kVTypeMask) {
    case RO_V_VWXUNARY0: {
      if (rvv_vs1_reg() == 0) {
        switch (rvv_vsew()) {
          case E8:
            set_rd(Rvvelt<type_sew_t<8>::type>(rvv_vs2_reg(), 0));
            break;
          case E16:
            set_rd(Rvvelt<type_sew_t<16>::type>(rvv_vs2_reg(), 0));
            break;
          case E32:
            set_rd(Rvvelt<type_sew_t<32>::type>(rvv_vs2_reg(), 0));
            break;
          case E64:
            set_rd(Rvvelt<type_sew_t<64>::type>(rvv_vs2_reg(), 0));
            break;
          default:
            UNREACHABLE();
        }
        set_rvv_vstart(0);
        SNPrintF(trace_buf_, "0x%ld", get_register(rd_reg()));
      } else {
        v8::base::EmbeddedVector<char, 256> buffer;
        disasm::NameConverter converter;
        disasm::Disassembler dasm(converter);
        dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(&instr_));
        PrintF("EXECUTING  0x%08" PRIxPTR "   %-44s\n",
               reinterpret_cast<intptr_t>(&instr_), buffer.begin());
        UNIMPLEMENTED_RISCV();
      }
    } break;
    case RO_V_VREDMAXU:
      RVV_VI_VV_ULOOP_REDUCTION(
          { vd_0_res = (vd_0_res >= vs2) ? vd_0_res : vs2; })
      break;
    case RO_V_VREDMAX:
      RVV_VI_VV_LOOP_REDUCTION(
          { vd_0_res = (vd_0_res >= vs2) ? vd_0_res : vs2; })
      break;
    case RO_V_VREDMINU:
      RVV_VI_VV_ULOOP_REDUCTION(
          { vd_0_res = (vd_0_res <= vs2) ? vd_0_res : vs2; })
      break;
    case RO_V_VREDMIN:
      RVV_VI_VV_LOOP_REDUCTION(
          { vd_0_res = (vd_0_res <= vs2) ? vd_0_res : vs2; })
      break;
    default:
      v8::base::EmbeddedVector<char, 256> buffer;
      disasm::NameConverter converter;
      disasm::Disassembler dasm(converter);
      dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(&instr_));
      PrintF("EXECUTING  0x%08" PRIxPTR "   %-44s\n",
             reinterpret_cast<intptr_t>(&instr_), buffer.begin());
      UNIMPLEMENTED_RISCV();
      break;
  }
}

void Simulator::DecodeRvvMVX() {
  DCHECK_EQ(instr_.InstructionBits() & (kBaseOpcodeMask | kFunct3Mask), OP_MVX);
  switch (instr_.InstructionBits() & kVTypeMask) {
    case RO_V_VRXUNARY0:
      if (instr_.Vs2Value() == 0x0) {
        if (rvv_vl() > 0 && rvv_vstart() < rvv_vl()) {
          switch (rvv_vsew()) {
            case E8:
              Rvvelt<uint8_t>(rvv_vd_reg(), 0, true) =
                  (uint8_t)get_register(rs1_reg());
              break;
            case E16:
              Rvvelt<uint16_t>(rvv_vd_reg(), 0, true) =
                  (uint16_t)get_register(rs1_reg());
              break;
            case E32:
              Rvvelt<uint32_t>(rvv_vd_reg(), 0, true) =
                  (uint32_t)get_register(rs1_reg());
              break;
            case E64:
              Rvvelt<uint64_t>(rvv_vd_reg(), 0, true) =
                  (uint64_t)get_register(rs1_reg());
              break;
            default:
              UNREACHABLE();
          }
          // set_rvv_vl(0);
        }
        set_rvv_vstart(0);
        rvv_trace_vd();
      } else {
        UNSUPPORTED_RISCV();
      }
      break;
    default:
      v8::base::EmbeddedVector<char, 256> buffer;
      disasm::NameConverter converter;
      disasm::Disassembler dasm(converter);
      dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(&instr_));
      PrintF("EXECUTING  0x%08" PRIxPTR "   %-44s\n",
             reinterpret_cast<intptr_t>(&instr_), buffer.begin());
      UNIMPLEMENTED_RISCV();
      break;
  }
}

void Simulator::DecodeRvvFVV() {
  DCHECK_EQ(instr_.InstructionBits() & (kBaseOpcodeMask | kFunct3Mask), OP_FVV);
  switch (instr_.InstructionBits() & kVTypeMask) {
    case RO_V_VFDIV_VV: {
      RVV_VI_VFP_VV_LOOP(
          { UNIMPLEMENTED(); },
          {
            // TODO(riscv): use rm value (round mode)
            auto fn = [this](float vs1, float vs2) {
              if (is_invalid_fdiv(vs1, vs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<float>::quiet_NaN();
              } else if (vs2 == 0.0f) {
                this->set_fflags(kDivideByZero);
                return (std::signbit(vs1) == std::signbit(vs2)
                            ? std::numeric_limits<float>::infinity()
                            : -std::numeric_limits<float>::infinity());
              } else {
                return vs1 / vs2;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<float>::quiet_NaN();
            }
            vd = alu_out;
          },
          {
            // TODO(riscv): use rm value (round mode)
            auto fn = [this](double vs1, double vs2) {
              if (is_invalid_fdiv(vs1, vs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<double>::quiet_NaN();
              } else if (vs2 == 0.0f) {
                this->set_fflags(kDivideByZero);
                return (std::signbit(vs1) == std::signbit(vs2)
                            ? std::numeric_limits<double>::infinity()
                            : -std::numeric_limits<double>::infinity());
              } else {
                return vs1 / vs2;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<double>::quiet_NaN();
            }
            vd = alu_out;
          })
      break;
    }
    case RO_V_VFMUL_VV: {
      RVV_VI_VFP_VV_LOOP(
          { UNIMPLEMENTED(); },
          {
            // TODO(riscv): use rm value (round mode)
            auto fn = [this](double drs1, double drs2) {
              if (is_invalid_fmul(drs1, drs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<double>::quiet_NaN();
              } else {
                return drs1 * drs2;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<float>::quiet_NaN();
            }
            vd = alu_out;
          },
          {
            // TODO(riscv): use rm value (round mode)
            auto fn = [this](double drs1, double drs2) {
              if (is_invalid_fmul(drs1, drs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<double>::quiet_NaN();
              } else {
                return drs1 * drs2;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<double>::quiet_NaN();
            }
            vd = alu_out;
          })
      break;
    }
    case RO_V_VFUNARY0:
      switch (instr_.Vs1Value()) {
        case VFCVT_X_F_V:
          RVV_VI_VFP_VF_LOOP(
              { UNIMPLEMENTED(); },
              {
                Rvvelt<int32_t>(rvv_vd_reg(), i) =
                    RoundF2IHelper<int32_t>(vs2, read_csr_value(csr_frm));
                USE(vd);
                USE(fs1);
              },
              {
                Rvvelt<int64_t>(rvv_vd_reg(), i) =
                    RoundF2IHelper<int64_t>(vs2, read_csr_value(csr_frm));
                USE(vd);
                USE(fs1);
              })
          break;
        case VFCVT_XU_F_V:
          RVV_VI_VFP_VF_LOOP(
              { UNIMPLEMENTED(); },
              {
                Rvvelt<uint32_t>(rvv_vd_reg(), i) =
                    RoundF2IHelper<uint32_t>(vs2, read_csr_value(csr_frm));
                USE(vd);
                USE(fs1);
              },
              {
                Rvvelt<uint64_t>(rvv_vd_reg(), i) =
                    RoundF2IHelper<uint64_t>(vs2, read_csr_value(csr_frm));
                USE(vd);
                USE(fs1);
              })
          break;
        case VFCVT_F_XU_V:
          RVV_VI_VFP_VF_LOOP({ UNIMPLEMENTED(); },
                             {
                               auto vs2_i = Rvvelt<uint32_t>(rvv_vs2_reg(), i);
                               vd = static_cast<float>(vs2_i);
                               USE(vs2);
                               USE(fs1);
                             },
                             {
                               auto vs2_i = Rvvelt<uint64_t>(rvv_vs2_reg(), i);
                               vd = static_cast<double>(vs2_i);
                               USE(vs2);
                               USE(fs1);
                             })
          break;
        case VFCVT_F_X_V:
          RVV_VI_VFP_VF_LOOP({ UNIMPLEMENTED(); },
                             {
                               auto vs2_i = Rvvelt<int32_t>(rvv_vs2_reg(), i);
                               vd = static_cast<float>(vs2_i);
                               USE(vs2);
                               USE(fs1);
                             },
                             {
                               auto vs2_i = Rvvelt<int64_t>(rvv_vs2_reg(), i);
                               vd = static_cast<double>(vs2_i);
                               USE(vs2);
                               USE(fs1);
                             })
          break;
        case VFNCVT_F_F_W:
          RVV_VI_VFP_CVT_SCALE(
              { UNREACHABLE(); }, { UNREACHABLE(); },
              {
                auto vs2 = Rvvelt<double>(rvv_vs2_reg(), i);
                Rvvelt<float>(rvv_vd_reg(), i, true) =
                    CanonicalizeDoubleToFloatOperation(
                        [](double drs) { return static_cast<float>(drs); },
                        vs2);
              },
              { ; }, { ; }, { ; }, false, (rvv_vsew() >= E16))
          break;
        default:
          UNSUPPORTED_RISCV();
          break;
      }
      break;
    case RO_V_VFUNARY1:
      switch (instr_.Vs1Value()) {
        case VFCLASS_V:
          RVV_VI_VFP_VF_LOOP(
              { UNIMPLEMENTED(); },
              {
                int32_t& vd_i = Rvvelt<int32_t>(rvv_vd_reg(), i, true);
                vd_i = int32_t(FclassHelper(vs2));
                USE(fs1);
                USE(vd);
              },
              {
                int64_t& vd_i = Rvvelt<int64_t>(rvv_vd_reg(), i, true);
                vd_i = FclassHelper(vs2);
                USE(fs1);
                USE(vd);
              })
          break;
        default:
          break;
      }
      break;
    case RO_V_VMFEQ_VV: {
      RVV_VI_VFP_LOOP_CMP({ UNIMPLEMENTED(); },
                          { res = CompareFHelper(vs1, vs2, EQ); },
                          { res = CompareFHelper(vs1, vs2, EQ); }, true)
    } break;
    case RO_V_VMFNE_VV: {
      RVV_VI_VFP_LOOP_CMP({ UNIMPLEMENTED(); },
                          { res = CompareFHelper(vs1, vs2, NE); },
                          { res = CompareFHelper(vs1, vs2, NE); }, true)
    } break;
    case RO_V_VMFLT_VV: {
      RVV_VI_VFP_LOOP_CMP({ UNIMPLEMENTED(); },
                          { res = CompareFHelper(vs1, vs2, LT); },
                          { res = CompareFHelper(vs1, vs2, LT); }, true)
    } break;
    case RO_V_VMFLE_VV: {
      RVV_VI_VFP_LOOP_CMP({ UNIMPLEMENTED(); },
                          { res = CompareFHelper(vs1, vs2, LE); },
                          { res = CompareFHelper(vs1, vs2, LE); }, true)
    } break;
    case RO_V_VFMAX_VV: {
      RVV_VI_VFP_VV_LOOP({ UNIMPLEMENTED(); },
                         { vd = FMaxMinHelper(vs2, vs1, MaxMinKind::kMax); },
                         { vd = FMaxMinHelper(vs2, vs1, MaxMinKind::kMax); })
      break;
    }
    case RO_V_VFMIN_VV: {
      RVV_VI_VFP_VV_LOOP({ UNIMPLEMENTED(); },
                         { vd = FMaxMinHelper(vs2, vs1, MaxMinKind::kMin); },
                         { vd = FMaxMinHelper(vs2, vs1, MaxMinKind::kMin); })
      break;
    }
    case RO_V_VFSGNJ_VV:
      RVV_VI_VFP_VV_LOOP({ UNIMPLEMENTED(); },
                         { vd = fsgnj32(vs2, vs1, false, false); },
                         { vd = fsgnj64(vs2, vs1, false, false); })
      break;
    case RO_V_VFSGNJN_VV:
      RVV_VI_VFP_VV_LOOP({ UNIMPLEMENTED(); },
                         { vd = fsgnj32(vs2, vs1, true, false); },
                         { vd = fsgnj64(vs2, vs1, true, false); })
      break;
    case RO_V_VFSGNJX_VV:
      RVV_VI_VFP_VV_LOOP({ UNIMPLEMENTED(); },
                         { vd = fsgnj32(vs2, vs1, false, true); },
                         { vd = fsgnj64(vs2, vs1, false, true); })
      break;
    case RO_V_VFADD_VV:
      RVV_VI_VFP_VV_LOOP(
          { UNIMPLEMENTED(); },
          {
            auto fn = [this](float frs1, float frs2) {
              if (is_invalid_fadd(frs1, frs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<float>::quiet_NaN();
              } else {
                return frs1 + frs2;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<float>::quiet_NaN();
            }
            vd = alu_out;
          },
          {
            auto fn = [this](double frs1, double frs2) {
              if (is_invalid_fadd(frs1, frs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<double>::quiet_NaN();
              } else {
                return frs1 + frs2;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<double>::quiet_NaN();
            }
            vd = alu_out;
          })
      break;
    case RO_V_VFSUB_VV:
      RVV_VI_VFP_VV_LOOP(
          { UNIMPLEMENTED(); },
          {
            auto fn = [this](float frs1, float frs2) {
              if (is_invalid_fsub(frs1, frs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<float>::quiet_NaN();
              } else {
                return frs2 - frs1;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<float>::quiet_NaN();
            }

            vd = alu_out;
          },
          {
            auto fn = [this](double frs1, double frs2) {
              if (is_invalid_fsub(frs1, frs2)) {
                this->set_fflags(kInvalidOperation);
                return std::numeric_limits<double>::quiet_NaN();
              } else {
                return frs2 - frs1;
              }
            };
            auto alu_out = fn(vs1, vs2);
            // if any input or result is NaN, the result is quiet_NaN
            if (std::isnan(alu_out) || std::isnan(vs1) || std::isnan(vs2)) {
              // signaling_nan sets kInvalidOperation bit
              if (isSnan(alu_out) || isSnan(vs1) || isSnan(vs2))
                set_fflags(kInvalidOperation);
              alu_out = std::numeric_limits<double>::quiet_NaN();
            }
            vd = alu_out;
          })
      break;
    default:
      UNSUPPORTED_RISCV();
      break;
  }
}

void Simulator::DecodeRvvFVF() {
  DCHECK_EQ(instr_.InstructionBits() & (kBaseOpcodeMask | kFunct3Mask), OP_FVF);
  switch (instr_.InstructionBits() & kVTypeMask) {
    case RO_V_VFSGNJ_VF:
      RVV_VI_VFP_VF_LOOP(
          {}, { vd = fsgnj32(vs2, fs1, false, false); },
          { vd = fsgnj64(vs2, fs1, false, false); })
      break;
    case RO_V_VFSGNJN_VF:
      RVV_VI_VFP_VF_LOOP(
          {}, { vd = fsgnj32(vs2, fs1, true, false); },
          { vd = fsgnj64(vs2, fs1, true, false); })
      break;
    case RO_V_VFSGNJX_VF:
      RVV_VI_VFP_VF_LOOP(
          {}, { vd = fsgnj32(vs2, fs1, false, true); },
          { vd = fsgnj64(vs2, fs1, false, true); })
      break;
    default:
      UNSUPPORTED_RISCV();
      break;
  }
}
void Simulator::DecodeVType() {
  switch (instr_.InstructionBits() & (kFunct3Mask | kBaseOpcodeMask)) {
    case OP_IVV:
      DecodeRvvIVV();
      return;
    case OP_FVV:
      DecodeRvvFVV();
      return;
    case OP_MVV:
      DecodeRvvMVV();
      return;
    case OP_IVI:
      DecodeRvvIVI();
      return;
    case OP_IVX:
      DecodeRvvIVX();
      return;
    case OP_FVF:
      UNIMPLEMENTED_RISCV();
      return;
    case OP_MVX:
      DecodeRvvMVX();
      return;
  }
  switch (instr_.InstructionBits() &
          (kBaseOpcodeMask | kFunct3Mask | 0x80000000)) {
    case RO_V_VSETVLI: {
      uint64_t avl;
      set_rvv_vtype(rvv_zimm());
      if (rs1_reg() != zero_reg) {
        avl = rs1();
      } else if (rd_reg() != zero_reg) {
        avl = ~0;
      } else {
        avl = rvv_vl();
      }
      avl = avl <= rvv_vlmax() ? avl : rvv_vlmax();
      set_rvv_vl(avl);
      set_rd(rvv_vl());
      rvv_trace_status();
      break;
    }
    case RO_V_VSETVL: {
      if (!(instr_.InstructionBits() & 0x40000000)) {
        uint64_t avl;
        set_rvv_vtype(rs2());
        if (rs1_reg() != zero_reg) {
          avl = rs1();
        } else if (rd_reg() != zero_reg) {
          avl = ~0;
        } else {
          avl = rvv_vl();
        }
        avl = avl <= rvv_vlmax()        ? avl
              : avl < (rvv_vlmax() * 2) ? avl / 2
                                        : rvv_vlmax();
        set_rvv_vl(avl);
        set_rd(rvv_vl());
        rvv_trace_status();
      } else {
        DCHECK_EQ(instr_.InstructionBits() &
                      (kBaseOpcodeMask | kFunct3Mask | 0xC0000000),
                  RO_V_VSETIVLI);
        uint64_t avl;
        set_rvv_vtype(rvv_zimm());
        avl = instr_.Rvvuimm();
        avl = avl <= rvv_vlmax()        ? avl
              : avl < (rvv_vlmax() * 2) ? avl / 2
                                        : rvv_vlmax();
        set_rvv_vl(avl);
        set_rd(rvv_vl());
        rvv_trace_status();
        break;
      }
      break;
    }
    default:
      FATAL("Error: Unsupport on FILE:%s:%d.", __FILE__, __LINE__);
  }
}
// Executes the current instruction.
void Simulator::InstructionDecode(Instruction* instr) {
  if (v8::internal::FLAG_check_icache) {
    CheckICache(i_cache(), instr);
  }
  pc_modified_ = false;

  v8::base::EmbeddedVector<char, 256> buffer;

  if (::v8::internal::FLAG_trace_sim) {
    SNPrintF(trace_buf_, " ");
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    // Use a reasonably large buffer.
    dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(instr));

    // PrintF("EXECUTING  0x%08" PRIxPTR "   %-44s\n",
    //        reinterpret_cast<intptr_t>(instr), buffer.begin());
  }

  instr_ = instr;
  switch (instr_.InstructionType()) {
    case Instruction::kRType:
      DecodeRVRType();
      break;
    case Instruction::kR4Type:
      DecodeRVR4Type();
      break;
    case Instruction::kIType:
      DecodeRVIType();
      break;
    case Instruction::kSType:
      DecodeRVSType();
      break;
    case Instruction::kBType:
      DecodeRVBType();
      break;
    case Instruction::kUType:
      DecodeRVUType();
      break;
    case Instruction::kJType:
      DecodeRVJType();
      break;
    case Instruction::kCRType:
      DecodeCRType();
      break;
    case Instruction::kCAType:
      DecodeCAType();
      break;
    case Instruction::kCJType:
      DecodeCJType();
      break;
    case Instruction::kCBType:
      DecodeCBType();
      break;
    case Instruction::kCIType:
      DecodeCIType();
      break;
    case Instruction::kCIWType:
      DecodeCIWType();
      break;
    case Instruction::kCSSType:
      DecodeCSSType();
      break;
    case Instruction::kCLType:
      DecodeCLType();
      break;
    case Instruction::kCSType:
      DecodeCSType();
      break;
    case Instruction::kVType:
      DecodeVType();
      break;
    default:
      if (1) {
        std::cout << "Unrecognized instruction [@pc=0x" << std::hex
                  << registers_[pc] << "]: 0x" << instr->InstructionBits()
                  << std::endl;
      }
      UNSUPPORTED();
  }

  if (::v8::internal::FLAG_trace_sim) {
    PrintF("  0x%012" PRIxPTR "      %-44s\t%s\n",
           reinterpret_cast<intptr_t>(instr), buffer.begin(),
           trace_buf_.begin());
  }

  if (!pc_modified_) {
    set_register(pc,
                 reinterpret_cast<int64_t>(instr) + instr->InstructionSize());
  }
}

void Simulator::Execute() {
  // Get the PC to simulate. Cannot use the accessor here as we need the
  // raw PC value and not the one used as input to arithmetic instructions.
  int64_t program_counter = get_pc();
  while (program_counter != end_sim_pc) {
    Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
    icount_++;
    if (icount_ == static_cast<int64_t>(::v8::internal::FLAG_stop_sim_at)) {
      RiscvDebugger dbg(this);
      dbg.Debug();
    } else {
      InstructionDecode(instr);
    }
    CheckBreakpoints();
    program_counter = get_pc();
  }
}

void Simulator::CallInternal(Address entry) {
  // Adjust JS-based stack limit to C-based stack limit.
  isolate_->stack_guard()->AdjustStackLimitForSimulator();

  // Prepare to execute the code at entry.
  set_register(pc, static_cast<int64_t>(entry));
  // Put down marker for end of simulation. The simulator will stop simulation
  // when the PC reaches this value. By saving the "end simulation" value into
  // the LR the simulation stops when returning to this call point.
  set_register(ra, end_sim_pc);

  // Remember the values of callee-saved registers.
  int64_t s0_val = get_register(s0);
  int64_t s1_val = get_register(s1);
  int64_t s2_val = get_register(s2);
  int64_t s3_val = get_register(s3);
  int64_t s4_val = get_register(s4);
  int64_t s5_val = get_register(s5);
  int64_t s6_val = get_register(s6);
  int64_t s7_val = get_register(s7);
  int64_t s8_val = get_register(s8);
  int64_t s9_val = get_register(s9);
  int64_t s10_val = get_register(s10);
  int64_t s11_val = get_register(s11);
  int64_t gp_val = get_register(gp);
  int64_t sp_val = get_register(sp);

  // Set up the callee-saved registers with a known value. To be able to check
  // that they are preserved properly across JS execution.
  int64_t callee_saved_value = icount_;
  set_register(s0, callee_saved_value);
  set_register(s1, callee_saved_value);
  set_register(s2, callee_saved_value);
  set_register(s3, callee_saved_value);
  set_register(s4, callee_saved_value);
  set_register(s5, callee_saved_value);
  set_register(s6, callee_saved_value);
  set_register(s7, callee_saved_value);
  set_register(s8, callee_saved_value);
  set_register(s9, callee_saved_value);
  set_register(s10, callee_saved_value);
  set_register(s11, callee_saved_value);
  set_register(gp, callee_saved_value);

  // Start the simulation.
  Execute();

  // Check that the callee-saved registers have been preserved.
  CHECK_EQ(callee_saved_value, get_register(s0));
  CHECK_EQ(callee_saved_value, get_register(s1));
  CHECK_EQ(callee_saved_value, get_register(s2));
  CHECK_EQ(callee_saved_value, get_register(s3));
  CHECK_EQ(callee_saved_value, get_register(s4));
  CHECK_EQ(callee_saved_value, get_register(s5));
  CHECK_EQ(callee_saved_value, get_register(s6));
  CHECK_EQ(callee_saved_value, get_register(s7));
  CHECK_EQ(callee_saved_value, get_register(s8));
  CHECK_EQ(callee_saved_value, get_register(s9));
  CHECK_EQ(callee_saved_value, get_register(s10));
  CHECK_EQ(callee_saved_value, get_register(s11));
  CHECK_EQ(callee_saved_value, get_register(gp));

  // Restore callee-saved registers with the original value.
  set_register(s0, s0_val);
  set_register(s1, s1_val);
  set_register(s2, s2_val);
  set_register(s3, s3_val);
  set_register(s4, s4_val);
  set_register(s5, s5_val);
  set_register(s6, s6_val);
  set_register(s7, s7_val);
  set_register(s8, s8_val);
  set_register(s9, s9_val);
  set_register(s10, s10_val);
  set_register(s11, s11_val);
  set_register(gp, gp_val);
  set_register(sp, sp_val);
}

intptr_t Simulator::CallImpl(Address entry, int argument_count,
                             const intptr_t* arguments) {
  constexpr int kRegisterPassedArguments = 8;
  // Set up arguments.

  // RISC-V 64G ISA has a0-a7 for passing arguments
  int reg_arg_count = std::min(kRegisterPassedArguments, argument_count);
  if (reg_arg_count > 0) set_register(a0, arguments[0]);
  if (reg_arg_count > 1) set_register(a1, arguments[1]);
  if (reg_arg_count > 2) set_register(a2, arguments[2]);
  if (reg_arg_count > 3) set_register(a3, arguments[3]);
  if (reg_arg_count > 4) set_register(a4, arguments[4]);
  if (reg_arg_count > 5) set_register(a5, arguments[5]);
  if (reg_arg_count > 6) set_register(a6, arguments[6]);
  if (reg_arg_count > 7) set_register(a7, arguments[7]);

  if (::v8::internal::FLAG_trace_sim) {
    std::cout << "CallImpl: reg_arg_count = " << reg_arg_count << std::hex
              << " entry-pc (JSEntry) = 0x" << entry
              << " a0 (Isolate-root) = 0x" << get_register(a0)
              << " a1 (orig_func/new_target) = 0x" << get_register(a1)
              << " a2 (func/target) = 0x" << get_register(a2)
              << " a3 (receiver) = 0x" << get_register(a3) << " a4 (argc) = 0x"
              << get_register(a4) << " a5 (argv) = 0x" << get_register(a5)
              << std::endl;
  }

  // Remaining arguments passed on stack.
  int64_t original_stack = get_register(sp);
  // Compute position of stack on entry to generated code.
  int stack_args_count = argument_count - reg_arg_count;
  int stack_args_size = stack_args_count * sizeof(*arguments) + kCArgsSlotsSize;
  int64_t entry_stack = original_stack - stack_args_size;

  if (base::OS::ActivationFrameAlignment() != 0) {
    entry_stack &= -base::OS::ActivationFrameAlignment();
  }
  // Store remaining arguments on stack, from low to high memory.
  intptr_t* stack_argument = reinterpret_cast<intptr_t*>(entry_stack);
  memcpy(stack_argument + kCArgSlotCount, arguments + reg_arg_count,
         stack_args_count * sizeof(*arguments));
  set_register(sp, entry_stack);

  CallInternal(entry);

  // Pop stack passed arguments.
  CHECK_EQ(entry_stack, get_register(sp));
  set_register(sp, original_stack);

  // return get_register(a0);
  // RISCV uses a0 to return result
  return get_register(a0);
}

double Simulator::CallFP(Address entry, double d0, double d1) {
  set_fpu_register_double(fa0, d0);
  set_fpu_register_double(fa1, d1);
  CallInternal(entry);
  return get_fpu_register_double(fa0);
}

uintptr_t Simulator::PushAddress(uintptr_t address) {
  int64_t new_sp = get_register(sp) - sizeof(uintptr_t);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(new_sp);
  *stack_slot = address;
  set_register(sp, new_sp);
  return new_sp;
}

uintptr_t Simulator::PopAddress() {
  int64_t current_sp = get_register(sp);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(current_sp);
  uintptr_t address = *stack_slot;
  set_register(sp, current_sp + sizeof(uintptr_t));
  return address;
}

Simulator::LocalMonitor::LocalMonitor()
    : access_state_(MonitorAccess::Open),
      tagged_addr_(0),
      size_(TransactionSize::None) {}

void Simulator::LocalMonitor::Clear() {
  access_state_ = MonitorAccess::Open;
  tagged_addr_ = 0;
  size_ = TransactionSize::None;
}

void Simulator::LocalMonitor::NotifyLoad() {
  if (access_state_ == MonitorAccess::RMW) {
    // A non linked load could clear the local monitor. As a result, it's
    // most strict to unconditionally clear the local monitor on load.
    Clear();
  }
}

void Simulator::LocalMonitor::NotifyLoadLinked(uintptr_t addr,
                                               TransactionSize size) {
  access_state_ = MonitorAccess::RMW;
  tagged_addr_ = addr;
  size_ = size;
}

void Simulator::LocalMonitor::NotifyStore() {
  if (access_state_ == MonitorAccess::RMW) {
    // A non exclusive store could clear the local monitor. As a result, it's
    // most strict to unconditionally clear the local monitor on store.
    Clear();
  }
}

bool Simulator::LocalMonitor::NotifyStoreConditional(uintptr_t addr,
                                                     TransactionSize size) {
  if (access_state_ == MonitorAccess::RMW) {
    if (addr == tagged_addr_ && size_ == size) {
      Clear();
      return true;
    } else {
      return false;
    }
  } else {
    DCHECK(access_state_ == MonitorAccess::Open);
    return false;
  }
}

Simulator::GlobalMonitor::LinkedAddress::LinkedAddress()
    : access_state_(MonitorAccess::Open),
      tagged_addr_(0),
      next_(nullptr),
      prev_(nullptr),
      failure_counter_(0) {}

void Simulator::GlobalMonitor::LinkedAddress::Clear_Locked() {
  access_state_ = MonitorAccess::Open;
  tagged_addr_ = 0;
}

void Simulator::GlobalMonitor::LinkedAddress::NotifyLoadLinked_Locked(
    uintptr_t addr) {
  access_state_ = MonitorAccess::RMW;
  tagged_addr_ = addr;
}

void Simulator::GlobalMonitor::LinkedAddress::NotifyStore_Locked() {
  if (access_state_ == MonitorAccess::RMW) {
    // A non exclusive store could clear the global monitor. As a result, it's
    // most strict to unconditionally clear global monitors on store.
    Clear_Locked();
  }
}

bool Simulator::GlobalMonitor::LinkedAddress::NotifyStoreConditional_Locked(
    uintptr_t addr, bool is_requesting_thread) {
  if (access_state_ == MonitorAccess::RMW) {
    if (is_requesting_thread) {
      if (addr == tagged_addr_) {
        Clear_Locked();
        // Introduce occasional sc/scd failures. This is to simulate the
        // behavior of hardware, which can randomly fail due to background
        // cache evictions.
        if (failure_counter_++ >= kMaxFailureCounter) {
          failure_counter_ = 0;
          return false;
        } else {
          return true;
        }
      }
    } else if ((addr & kExclusiveTaggedAddrMask) ==
               (tagged_addr_ & kExclusiveTaggedAddrMask)) {
      // Check the masked addresses when responding to a successful lock by
      // another thread so the implementation is more conservative (i.e. the
      // granularity of locking is as large as possible.)
      Clear_Locked();
      return false;
    }
  }
  return false;
}

void Simulator::GlobalMonitor::NotifyLoadLinked_Locked(
    uintptr_t addr, LinkedAddress* linked_address) {
  linked_address->NotifyLoadLinked_Locked(addr);
  PrependProcessor_Locked(linked_address);
}

void Simulator::GlobalMonitor::NotifyStore_Locked(
    LinkedAddress* linked_address) {
  // Notify each thread of the store operation.
  for (LinkedAddress* iter = head_; iter; iter = iter->next_) {
    iter->NotifyStore_Locked();
  }
}

bool Simulator::GlobalMonitor::NotifyStoreConditional_Locked(
    uintptr_t addr, LinkedAddress* linked_address) {
  DCHECK(IsProcessorInLinkedList_Locked(linked_address));
  if (linked_address->NotifyStoreConditional_Locked(addr, true)) {
    // Notify the other processors that this StoreConditional succeeded.
    for (LinkedAddress* iter = head_; iter; iter = iter->next_) {
      if (iter != linked_address) {
        iter->NotifyStoreConditional_Locked(addr, false);
      }
    }
    return true;
  } else {
    return false;
  }
}

bool Simulator::GlobalMonitor::IsProcessorInLinkedList_Locked(
    LinkedAddress* linked_address) const {
  return head_ == linked_address || linked_address->next_ ||
         linked_address->prev_;
}

void Simulator::GlobalMonitor::PrependProcessor_Locked(
    LinkedAddress* linked_address) {
  if (IsProcessorInLinkedList_Locked(linked_address)) {
    return;
  }

  if (head_) {
    head_->prev_ = linked_address;
  }
  linked_address->prev_ = nullptr;
  linked_address->next_ = head_;
  head_ = linked_address;
}

void Simulator::GlobalMonitor::RemoveLinkedAddress(
    LinkedAddress* linked_address) {
  base::MutexGuard lock_guard(&mutex);
  if (!IsProcessorInLinkedList_Locked(linked_address)) {
    return;
  }

  if (linked_address->prev_) {
    linked_address->prev_->next_ = linked_address->next_;
  } else {
    head_ = linked_address->next_;
  }
  if (linked_address->next_) {
    linked_address->next_->prev_ = linked_address->prev_;
  }
  linked_address->prev_ = nullptr;
  linked_address->next_ = nullptr;
}

#undef SScanF

}  // namespace internal
}  // namespace v8

#endif  // USE_SIMULATOR

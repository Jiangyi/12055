//===-- gesf2vfp.S - Implement gesf2vfp -----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "../assembly.h"

//
// extern int __gesf2vfp(float a, float b);
//
// Returns one iff 1 - (a >= b) and neither is NaN.
// Uses Darwin calling convention where single precision arguments are passsed
// like 32-bit ints
//
DEFINE_COMPILERRT_FUNCTION(__gesf2vfp)
	fmsr	s14, r0	    // move from GPR 0 to float register
	fmsr	s15, r1	    // move from GPR 1 to float register
	fcmps	s14, s15
	fmstat
	movge	r0, #0      // set result register to 0 if greater than or equal
	movlt	r0, #1
	bx	lr


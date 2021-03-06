/*
 * Copyright (c) 2013 Ambroz Bizjak
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef AMBROLIB_INT_DIVIDE_H
#define AMBROLIB_INT_DIVIDE_H

#include <stdint.h>

#include <aprinter/meta/ChooseInt.h>
#include <aprinter/meta/PowerOfTwo.h>

#ifdef AMBROLIB_AVR
#include <aprinter/avr-asm-ops/div_11_16_l15_s13.h>
#endif

#include <aprinter/BeginNamespace.h>

template <int NumBits1, bool Signed1, int NumBits2, bool Signed2, int LeftShift, int ResSatBits, bool SupportZero>
class IntDivide {
public:
    static_assert(LeftShift >= 0, "LeftShift must be non-negative");
    
    typedef ChooseInt<NumBits1, Signed1> Op1Type;
    typedef ChooseInt<NumBits2, Signed2> Op2Type;
    typedef ChooseInt<ResSatBits, (Signed1 || Signed2)> ResType;
    
    template <typename Option = int>
    __attribute__((always_inline)) inline static ResType call (Op1Type op1, Op2Type op2, Option opt = 0)
    {
        return
#ifdef AMBROLIB_AVR
            (LeftShift == 15 && ResSatBits == 13 && !Signed1 && NumBits1 > 8 && NumBits1 <= 11 && !Signed2 && NumBits2 <= 16) ? div_11_16_l15_s13(op1, op2, opt) :
#endif
            default_divide(op1, op2);
    }
    
private:
    typedef ChooseInt<(NumBits1 + LeftShift), (Signed1 || Signed2)> TempResType;
    typedef ChooseInt<NumBits2, (Signed1 || Signed2)> TempType2;
    
    static ResType default_divide (Op1Type op1, Op2Type op2)
    {
        if (SupportZero && op2 == 0) {
            return (op1 < 0) ? -PowerOfTwoMinusOne<ResType, ResSatBits>::Value :
                   (op1 == 0) ? 0 :
                   PowerOfTwoMinusOne<ResType, ResSatBits>::Value;
        }
        TempResType res = (((TempResType)op1 * PowerOfTwo<TempResType, LeftShift>::Value) / (TempType2)op2);
        if (ResSatBits < NumBits1 + LeftShift) {
            if (res > PowerOfTwoMinusOne<ResType, ResSatBits>::Value) {
                res = PowerOfTwoMinusOne<ResType, ResSatBits>::Value;
            } else if (Signed1 || Signed2) {
                if (res < -PowerOfTwoMinusOne<ResType, ResSatBits>::Value) {
                    res = -PowerOfTwoMinusOne<ResType, ResSatBits>::Value;
                }
            }
        }
        return res;
    }
};

#include <aprinter/EndNamespace.h>

#endif

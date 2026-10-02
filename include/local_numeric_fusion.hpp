#pragma once
#include "il.hpp"
namespace rmb {
// a: up to four 7-bit numeric-local slots; b: constant index or remaining slots.
// s: original IL slots consumed. flags: exact expression/evaluation order.
enum class LocalNumericPattern : std::uint8_t {
    Move,Constant,AddPush,SubPush,MulPush,AddStore,SubStore,MulStore,
    AddProduct,ProductAdd,ProductsDifferenceAdd,ConstantProductAdd,
    AddConstant,SubConstant,MulConstant,ConstantMul,AddConstantProduct
};
inline unsigned local_operand(const Op& op,unsigned index) {
    return index<4?(static_cast<std::uint32_t>(op.a)>>(index*7))&127u:
                  (static_cast<std::uint32_t>(op.b)>>((index-4)*7))&127u;
}
inline bool valid_local_numeric(const Op& op,std::size_t slots,std::size_t constants) {
    unsigned count=0,length=0;bool constant=false;
    switch(static_cast<LocalNumericPattern>(op.flags)) {
    case LocalNumericPattern::Move:count=2;length=2;break;
    case LocalNumericPattern::Constant:count=1;length=2;constant=true;break;
    case LocalNumericPattern::AddPush:case LocalNumericPattern::SubPush:case LocalNumericPattern::MulPush:
        count=2;length=3;break;
    case LocalNumericPattern::AddStore:case LocalNumericPattern::SubStore:case LocalNumericPattern::MulStore:
        count=3;length=4;break;
    case LocalNumericPattern::AddProduct:case LocalNumericPattern::ProductAdd:count=4;length=6;break;
    case LocalNumericPattern::ProductsDifferenceAdd:count=6;length=10;break;
    case LocalNumericPattern::ConstantProductAdd:count=4;length=8;constant=true;break;
    case LocalNumericPattern::AddConstant:case LocalNumericPattern::SubConstant:
    case LocalNumericPattern::MulConstant:case LocalNumericPattern::ConstantMul:
        count=1;length=3;constant=true;break;
    case LocalNumericPattern::AddConstantProduct:count=3;length=6;constant=true;break;
    default:return false;
    }
    if(op.s!=length||op.a<0)return false;
    const unsigned packed_count=count>4?4:count;
    if((static_cast<std::uint32_t>(op.a)>>(packed_count*7))!=0)return false;
    if(constant) {if(op.b<0||static_cast<std::size_t>(op.b)>=constants)return false;}
    else if(op.b<0||(count<=4?op.b!=0:(static_cast<std::uint32_t>(op.b)>>((count-4)*7))!=0))return false;
    for(unsigned i=0;i<count;++i)if(local_operand(op,i)>=slots)return false;
    return true;
}
static_assert(kMaxFunctionLocals<=128,"Local fusion requires 7-bit slots, separate from globals");
}

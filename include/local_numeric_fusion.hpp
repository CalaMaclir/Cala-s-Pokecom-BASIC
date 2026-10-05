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
inline bool valid_local_gray(const Op& op,std::size_t slots) {
    if(op.flags!=0||op.s!=12||op.a<0||op.b<0||op.b>127||
       (static_cast<std::uint32_t>(op.a)>>28)!=0)return false;
    for(unsigned i=0;i<4;++i)if(local_operand(op,i)>=slots)return false;
    return static_cast<unsigned>(op.b)<slots;
}
inline bool valid_local_gray_span(const CompiledProgram& p,std::size_t i,std::size_t slots,std::size_t end_pc) {
    if(i>=p.code_count)return false;
    const auto& op=p.code[i];
    if(!valid_local_gray(op,slots)||i+op.s>p.code_count||i+op.s>end_pc)return false;
            // Retained tails support exact overflow diagnostics and source mapping.
            if(p.code[i+1].code!=OpCode::LOAD_LOCAL_NUM||p.code[i+1].a!=static_cast<int>(local_operand(op,2))||
               p.code[i+2].code!=OpCode::MUL_NUM||
               !((p.code[i+3].code==OpCode::FN1_NUM&&p.code[i+3].a==FnId::INT)||
                 (p.code[i+3].code==OpCode::CALLFN&&p.code[i+3].a==FnId::INT&&p.code[i+3].b==1))||
               p.code[i+4].code!=OpCode::STORE_LOCAL_NUM||p.code[i+4].a!=static_cast<int>(local_operand(op,0)))return false;
            for(unsigned j=5;j<=7;++j)if(p.code[i+j].code!=OpCode::LOAD_LOCAL_NUM||
                p.code[i+j].a!=static_cast<int>(local_operand(op,0)))return false;
            if(p.code[i+8].code!=OpCode::CALLFN||p.code[i+8].a!=FnId::GCOLOR||p.code[i+8].b!=3||
               p.code[i+9].code!=OpCode::LOAD_LOCAL_NUM||p.code[i+9].a!=static_cast<int>(local_operand(op,3))||
               p.code[i+10].code!=OpCode::LOAD_LOCAL_NUM||p.code[i+10].a!=op.b||
               p.code[i+11].code!=OpCode::CALLFN||p.code[i+11].a!=FnId::GPSET||p.code[i+11].b!=2)return false;
    return true;
}
static_assert(kMaxFunctionLocals<=128,"Local fusion requires 7-bit slots, separate from globals");
}


//
// Created by juanb on 12/07/2019.
//

#ifndef CPPLOX_JIT_HPP
#define CPPLOX_JIT_HPP

#include <llvm/IR/Function.h>

class VM;
class Chunk;

llvm::Function* generade_code(llvm::Module* module, Chunk* chunk, llvm::StructType* value_type, llvm::PointerType* valutePtr_type);
llvm::Function* generate_falsey(llvm::Module* module, llvm::StructType* value_type, llvm::PointerType* valutePtr_type);
llvm::Function* generate_equal(llvm::Module* module, llvm::StructType* value_type, llvm::PointerType* valutePtr_type);

#endif //CPPLOX_JIT_HPP

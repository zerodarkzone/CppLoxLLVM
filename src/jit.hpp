//
// Created by juanb on 12/07/2019.
//

#ifndef CPPLOX_JIT_HPP
#define CPPLOX_JIT_HPP

#include <llvm/IR/Function.h>

class VM;
class Chunk;

llvm::Function* generate_main(llvm::Module* module, const std::string& name, llvm::StructType* value_type, llvm::PointerType* valutePtr_type);
llvm::Function* generade_code(llvm::Module* module, Chunk* chunk, const std::string& name, llvm::GlobalValue::LinkageTypes linkage, llvm::StructType* value_type, llvm::PointerType* valutePtr_type);
llvm::Function* generate_falsey(llvm::Module* module, llvm::StructType* value_type, llvm::PointerType* valutePtr_type);
llvm::Function* generate_equal(llvm::Module* module, llvm::StructType* value_type, llvm::PointerType* valutePtr_type);

#endif //CPPLOX_JIT_HPP

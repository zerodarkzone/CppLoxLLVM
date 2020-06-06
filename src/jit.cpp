//
// Created by juanb on 12/07/2019.
//

#include <cmath>
#include <unordered_set>
#include <vector>
#include <iomanip>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/IR/LegacyPassManager.h>

#include "jit.hpp"
#include "chunk.hpp"
#include "vm.hpp"
#include "utils.hpp"

constexpr int MEMORY_SIZE = 30000;
const char* const JIT_FUNC_NAME = "__llvmjit";

// Host function callable from JITed code. Given a pointer to program memory,
// dumps non-zero entries to std::cout. This function is declared extern "C" so
// that we can refer to it in the emitted LLVM IR without mangling the name.
// Alternatively, we could use the LLVM mangler to mangle the name for us (and
// then the extern "C" wouldn't be necessary).
extern "C" void dump_memory(uint8_t* memory) {
	std::cout << "* Memory nonzero locations:\n";
	for (size_t i = 0, pcount = 0; i < MEMORY_SIZE; ++i) {
		if (memory[i]) {
			std::cout << std::right << "[" << std::setw(3) << i
			          << "] = " << std::setw(3) << std::left
			          << static_cast<int32_t>(memory[i]) << "      ";
			pcount++;

			if (pcount > 0 && pcount % 4 == 0) {
				std::cout << "\n";
			}
		}
	}
	std::cout << "\n";
}

extern "C" void numberError(VM* vm, int32_t pc)
{
	using namespace std::string_literals;
	vm->runtimeError(pc, "Operands must be numbers."s);
}

extern "C" void variableError(VM* vm, uint32_t pos)
{
	using namespace std::string_literals;
	vm->runtimeError("Undefined variable "s, vm->m_globalNames[pos], "."s);
}

extern "C" bool equal(Value *a, Value *b)
{
	return *a == *b;
}

extern "C" int concatenate(VM *vm, Value *a, Value *b, Value *res)
{
	if (b->isObjString() && a->isObjString())
	{
		auto b_str = b->asObjString()->value;
		auto a_str = a->asObjString()->value;
		*res = Value::Object(Memory::createString(vm, a_str + b_str));
	}
	else if(b->isObjString() && a->isNumber())
	{
		auto b_str = b->asObjString()->value;
		auto a_num = a->asNumber();
		char buff[128];
		std::sprintf(buff, "%g", a_num);
		*res = Value::Object(Memory::createString(vm, std::string(buff) + b_str));
	}
	else if(b->isNumber() && a->isObjString())
	{
		auto b_num = b->asNumber();
		auto a_str = a->asObjString()->value;
		char buff[1024];
		std::sprintf(buff, "%g", b_num);
		*res = Value::Object(Memory::createString(vm, a_str + std::string(buff)));
	}
	else
	{
		vm->runtimeError("Operands must be numbers or strings.");
		return (int)InterpretResult::RUNTIME_ERROR;
	}
	return (int)InterpretResult::OK;
}

extern "C" void print(Value* val)
{
	std::cout << *val << std::endl;
}

std::unordered_set<uint32_t> jumpBlocks(Chunk* chunk)
{
	auto size = chunk->size();
	std::unordered_set<uint32_t> labels;

	for (auto offset = 0u; offset < size;)
	{
		labels.insert(offset);
		auto instruction = chunk->get(offset);
		switch (instruction)
		{
			case OpCode::CONSTANT:
			case OpCode::GET_LOCAL:
			case OpCode::SET_LOCAL:
			case OpCode::GET_GLOBAL:
			case OpCode::DEFINE_GLOBAL:
			case OpCode::SET_GLOBAL:
				offset += 2;
				break;
			case OpCode::GET_LOCAL_SHORT:
			case OpCode::SET_LOCAL_SHORT:
				offset += 3;
				break;
			case OpCode::CONSTANT_LONG:
			case OpCode::GET_GLOBAL_LONG:
			case OpCode::DEFINE_GLOBAL_LONG:
			case OpCode::SET_GLOBAL_LONG:
				offset += 4;
				break;
			case OpCode::JUMP:
			{
				uint16_t jump = chunk->get(offset + 1u) |
								static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);
				//labels.insert(offset + 3 + jump);
				offset += 3;
				break;
			}
			case OpCode::JUMP_IF_FALSE:
			{
				uint16_t jump = chunk->get(offset + 1u) |
								static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);
				//labels.insert(offset + 3 + jump);
				offset += 3;
				break;
			}
			case OpCode::JUMP_IF_TRUE:
			{
				uint16_t jump = chunk->get(offset + 1u) |
								static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);
				//labels.insert(offset + 3 + jump);
				offset += 3;
				break;
			}
			case OpCode::JUMP_BACK:
			{
				uint16_t jump = chunk->get(offset + 1u) |
								static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);
				//labels.insert(offset + 3 - jump);
				offset += 3;
				break;
			}
			default:
				offset += 1;
				break;
		}
	}
	return labels;
}

llvm::Function* generate_equal(llvm::Module* module, llvm::StructType* value_type, llvm::PointerType* valutePtr_type)
{
	llvm::LLVMContext& context = module->getContext();

	llvm::Type* bool_type = llvm::Type::getInt1Ty(context);
	llvm::PointerType* in64Ptr_type = llvm::Type::getInt64PtrTy(context);

	llvm::FunctionType* jit_func_type =
			llvm::FunctionType::get(bool_type, {valutePtr_type, valutePtr_type}, false);
	llvm::Function* jit_func = llvm::Function::Create(
			jit_func_type, llvm::Function::InternalLinkage, "_equal", module);

	llvm::BasicBlock* entry_bb =
			llvm::BasicBlock::Create(context, "entry", jit_func);
	llvm::IRBuilder<> builder(entry_bb);

	auto type_number = builder.getInt8(static_cast<uint8_t>(ValueType::NUMBER));


	llvm::Argument* a_ptr = jit_func->arg_begin();
	llvm::Argument* b_ptr = jit_func->arg_begin() + 1;

	llvm::Value* a_type_ptr = builder.CreateStructGEP(value_type, a_ptr, 0, "type_ptr");
	llvm::Value* a_type = builder.CreateLoad(a_type_ptr, false, "val_type");

	llvm::Value* b_type_ptr = builder.CreateStructGEP(value_type, b_ptr, 0, "type_ptr");
	llvm::Value* b_type = builder.CreateLoad(b_type_ptr, false, "val_type");

	llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
	llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "then", jit_func);
	llvm::Value* ne_type = builder.CreateICmpNE(a_type, b_type, "ne_type");
	builder.CreateCondBr(ne_type, then_bb, else_bb);
	builder.SetInsertPoint(then_bb);
	builder.CreateRet(builder.getFalse());

	builder.SetInsertPoint(else_bb);
	llvm::Value* a_value_ptr = builder.CreateStructGEP(value_type, a_ptr, 1, "a_value_ptr");
	llvm::Value* a_int_ptr = builder.CreateBitCast(a_value_ptr, in64Ptr_type, "a_int_ptr");
	llvm::Value* b_value_ptr = builder.CreateStructGEP(value_type, b_ptr, 1, "b_value_ptr");
	llvm::Value* b_int_ptr = builder.CreateBitCast(b_value_ptr, in64Ptr_type, "b_int_ptr");

	llvm::Value* a_value = builder.CreateLoad(a_int_ptr, "a_value");
	llvm::Value* b_value = builder.CreateLoad(b_int_ptr, "b_value");

	builder.CreateRet(builder.CreateICmpEQ(a_value, b_value));


	/*llvm::BasicBlock* def_bb = llvm::BasicBlock::Create(context, "default", jit_func);
	llvm::BasicBlock* num_bb = llvm::BasicBlock::Create(context, "number", jit_func);
	llvm::BasicBlock* bool_bb = llvm::BasicBlock::Create(context, "bool", jit_func);
	llvm::BasicBlock* obj_bb = llvm::BasicBlock::Create(context, "obj", jit_func);

	llvm::SwitchInst* sw = builder.CreateSwitch(a_type, def_bb, 3);
	sw->addCase(type_bool, bool_bb);
	sw->addCase(type_obj, obj_bb);
	sw->addCase(type_number, num_bb);

	builder.SetInsertPoint(bool_bb);
	llvm::Value* a_bool_ptr = builder.CreateBitCast(a_value_ptr, boolPtr_type, "a_bool_ptr");
	llvm::Value* a_bool = builder.CreateLoad(a_bool_ptr, "a_bool");

	llvm::Value* b_bool_ptr = builder.CreateBitCast(b_value_ptr, boolPtr_type, "b_bool_ptr");
	llvm::Value* b_bool = builder.CreateLoad(b_bool_ptr, "b_bool");

	builder.CreateRet(builder.CreateICmpEQ(a_bool, b_bool));

	builder.SetInsertPoint(obj_bb);
	llvm::Value* a_obj_ptr = builder.CreateBitCast(a_value_ptr, in64Ptr_type, "a_obj_ptr");
	llvm::Value* a_obj = builder.CreateLoad(a_obj_ptr, "a_obj");

	llvm::Value* b_obj_ptr = builder.CreateBitCast(b_value_ptr, in64Ptr_type, "b_obj_ptr");
	llvm::Value* b_obj = builder.CreateLoad(b_obj_ptr, "b_obj");

	builder.CreateRet(builder.CreateICmpEQ(a_obj, b_obj));

	builder.SetInsertPoint(num_bb);
	llvm::Value* a_val = builder.CreateLoad(a_value_ptr, "a_val");
	llvm::Value* b_val = builder.CreateLoad(b_value_ptr, "b_val");

	builder.CreateRet(builder.CreateFCmpOEQ(a_val, b_val));

	builder.SetInsertPoint(def_bb);*/
	//builder.CreateRet(builder.getTrue());

	return jit_func;
}

llvm::Function* generate_falsey(llvm::Module* module, llvm::StructType* value_type, llvm::PointerType* valutePtr_type)
{
	llvm::LLVMContext& context = module->getContext();

	llvm::Type* bool_type = llvm::Type::getInt1Ty(context);
	llvm::PointerType* boolPtr_type = llvm::Type::getInt1PtrTy(context);

	llvm::FunctionType* jit_func_type =
			llvm::FunctionType::get(bool_type, {valutePtr_type}, false);
	llvm::Function* jit_func = llvm::Function::Create(
			jit_func_type, llvm::Function::InternalLinkage, "_is_falsey", module);

	llvm::BasicBlock* entry_bb =
			llvm::BasicBlock::Create(context, "entry", jit_func);
	llvm::IRBuilder<> builder(entry_bb);

	auto type_bool = builder.getInt8(static_cast<uint8_t>(ValueType::BOOL));
	auto type_nil = builder.getInt8(static_cast<uint8_t>(ValueType::NIL));

	llvm::Argument* val_ptr = jit_func->arg_begin();

	llvm::Value* val_type_ptr = builder.CreateStructGEP(value_type, val_ptr, 0, "type_ptr");
	llvm::Value* val_type = builder.CreateLoad(val_type_ptr, false, "val_type");


	llvm::BasicBlock* true_bb = llvm::BasicBlock::Create(context, "true", jit_func);
	llvm::BasicBlock* false_bb = llvm::BasicBlock::Create(context, "false", jit_func);
	llvm::BasicBlock* not_nil_bb = llvm::BasicBlock::Create(context, "not_nil", jit_func);
	llvm::BasicBlock* bool_bb = llvm::BasicBlock::Create(context, "bool", jit_func);

	llvm::Value* is_nil = builder.CreateICmpEQ(val_type, type_nil, "is_nil");
	builder.CreateCondBr(is_nil, true_bb, not_nil_bb);

	builder.SetInsertPoint(not_nil_bb);

	llvm::Value* is_bool = builder.CreateICmpEQ(val_type, type_bool, "is_bool");
	builder.CreateCondBr(is_bool, bool_bb, false_bb);

	builder.SetInsertPoint(bool_bb);

	llvm::Value* val_value_ptr = builder.CreateStructGEP(value_type, val_ptr, 1, "value_ptr");
	llvm::Value* val_bool_ptr = builder.CreateBitCast(val_value_ptr, boolPtr_type, "bool_ptr");

	llvm::Value* val_bool = builder.CreateLoad(val_bool_ptr, false, "val_value");

	builder.CreateCondBr(val_bool, false_bb, true_bb);

	// return true
	builder.SetInsertPoint(true_bb);
	builder.CreateRet(builder.getTrue());

	// return false
	builder.SetInsertPoint(false_bb);
	builder.CreateRet(builder.getFalse());

	return jit_func;
}

llvm::Function* generade_code(llvm::Module* module, Chunk* chunk, llvm::StructType* value_type, llvm::PointerType* valutePtr_type)
{
	llvm::LLVMContext& context = module->getContext();

	// Add a declaration for external functions used in the JITed code. We use
	llvm::Type* int32_type = llvm::Type::getInt32Ty(context);
	llvm::Type* int64_type = llvm::Type::getInt64Ty(context);
	llvm::Type* void_type = llvm::Type::getVoidTy(context);
	llvm::Type* uint8_type = llvm::Type::getInt8Ty(context);
	llvm::Type* double_type = llvm::Type::getDoubleTy(context);
	llvm::Type* bool_type = llvm::Type::getInt1Ty(context);

	llvm::PointerType* voidPtr_type = llvm::PointerType::get(void_type, 0);
	llvm::PointerType* in64Ptr_type = llvm::Type::getInt64PtrTy(context);
	llvm::PointerType* boolPtr_type = llvm::Type::getInt1PtrTy(context);

	llvm::StructType* obj_type = llvm::StructType::create(context, "SObj");
	llvm::PointerType* objPtr_type = llvm::PointerType::get(obj_type, 0);
	obj_type->setBody({objPtr_type, int64_type, uint8_type});


	llvm::Function* numberError_func = llvm::Function::Create(
			llvm::FunctionType::get(void_type, {voidPtr_type, int32_type}, false),
			llvm::Function::ExternalLinkage, "numberError", module);
	numberError_func->setDoesNotThrow();
	numberError_func->setDoesNotRecurse();

	llvm::Function* variableError_func = llvm::Function::Create(
			llvm::FunctionType::get(void_type, {voidPtr_type, int32_type}, false),
			llvm::Function::ExternalLinkage, "variableError", module);
	variableError_func->setDoesNotThrow();
	variableError_func->setDoesNotRecurse();

	/*llvm::Function* equal_func = llvm::Function::Create(
			llvm::FunctionType::get(bool_type, {valutePtr_type, valutePtr_type}, false),
			llvm::Function::ExternalLinkage, "equal", module);
	equal_func->setOnlyReadsMemory();*/

	llvm::Function* concatenate_func = llvm::Function::Create(
			llvm::FunctionType::get(int32_type, {voidPtr_type, valutePtr_type, valutePtr_type, valutePtr_type}, false),
			llvm::Function::ExternalLinkage, "concatenate", module);
	concatenate_func->setOnlyAccessesArgMemory();
	concatenate_func->setDoesNotThrow();
	concatenate_func->setDoesNotRecurse();

	llvm::Function* print_func = llvm::Function::Create(
			llvm::FunctionType::get(void_type, {valutePtr_type}, false),
			llvm::Function::ExternalLinkage, "print", module);
	print_func->setOnlyReadsMemory();
	print_func->setOnlyAccessesArgMemory();

	llvm::Function* dump_memory_func = llvm::Function::Create(
			llvm::FunctionType::get(void_type, {llvm::Type::getInt8PtrTy(context)}, false),
			llvm::Function::ExternalLinkage, "dump_memory", module);

	llvm::Function* is_falsey = module->getFunction("_is_falsey");

	llvm::Function* equal_func = module->getFunction("_equal");

	// generate falsey function
	/*llvm::Function* is_falsey = generate_falsey(module, value_type, valutePtr_type);
	if (llvm::verifyFunction(*is_falsey, &llvm::errs()))
		DIE << "Error verifying function.";
	// generate equal function
	llvm::Function* equal_func = generate_equal(module, value_type, valutePtr_type);
	if (llvm::verifyFunction(*equal_func, &llvm::errs()))
		DIE << "Error verifying function.";*/


	// Create function signature and object. int (*)(VM*, Value*, Value*, Value*)
	llvm::FunctionType* jit_func_type =
			llvm::FunctionType::get(int32_type, {voidPtr_type, valutePtr_type, valutePtr_type, valutePtr_type}, false);
	llvm::Function* jit_func = llvm::Function::Create(
			jit_func_type, llvm::Function::ExternalLinkage, "_jit_func", module);

	llvm::Argument* vm_ = jit_func->arg_begin();
	//llvm::Argument* stack = jit_func->arg_begin() + 1;
	//llvm::Argument* constants = jit_func->arg_begin() + 2;
	llvm::Argument* globals = jit_func->arg_begin() + 3;

	llvm::BasicBlock* entry_bb =
			llvm::BasicBlock::Create(context, "entry", jit_func);
	llvm::IRBuilder<> builder(entry_bb);

	auto const_0 = builder.getInt32(0);
	auto const_1 = builder.getInt32(1);
	auto const_2 = builder.getInt32(2);

	auto type_number = builder.getInt8(static_cast<uint8_t>(ValueType::NUMBER));
	auto type_bool = builder.getInt8(static_cast<uint8_t>(ValueType::BOOL));
	auto type_obj = builder.getInt8(static_cast<uint8_t>(ValueType::OBJ));
	auto type_nil = builder.getInt8(static_cast<uint8_t>(ValueType::NIL));
	auto type_undefined = builder.getInt8(static_cast<uint8_t>(ValueType::UNDEFINED));
	auto value_size = builder.getInt64(sizeof(Value));

	llvm::AllocaInst* stack =
			builder.CreateAlloca(value_type, builder.getInt32(12500), "stack");

	llvm::AllocaInst* constants =
			builder.CreateAlloca(value_type, builder.getInt32(chunk->constantsSize()), "constants");
	for (size_t i = 0; i < chunk->constantsSize(); ++i)
	{
		auto &constant = chunk->constants()[i];
		llvm::Value* elem_addr = builder.CreateInBoundsGEP(constants, {builder.getInt32(i)}, "elem_addr");
		//llvm::Value* elem_type_ptr = builder.CreateStructGEP(value_type, elem_addr, 0, "type_ptr");
		//llvm::Value* elem_value_ptr = builder.CreateStructGEP(value_type, elem_addr, 1, "value_ptr");
		switch (constant.type())
		{
			default:
				DIE << "Cant happen\n";
				break;
			case ValueType::NUMBER: {
				builder.CreateStore(llvm::ConstantStruct::get(value_type,
						{type_number, llvm::ConstantFP::get(double_type, constant.asNumber())}), elem_addr);
				break;
			}
			case ValueType::OBJ: {
				auto addr = builder.getInt64((size_t)constant.asObj());
				auto ptr = llvm::ConstantExpr::getIntToPtr(addr, objPtr_type);
				llvm::Value* elem_type_ptr = builder.CreateStructGEP(value_type, elem_addr, 0, "type_ptr");
				builder.CreateStore(type_obj, elem_type_ptr);
				llvm::Value* elem_value_ptr = builder.CreateStructGEP(value_type, elem_addr, 1, "value_ptr");
				llvm::Value* elem_ptr = builder.CreateBitCast(elem_value_ptr, llvm::PointerType::get(objPtr_type, 0), "elem_ptr");
				builder.CreateStore(ptr, elem_ptr);
				break;
			}
		}
	}

	llvm::AllocaInst* stack_top =
			builder.CreateAlloca(int32_type, nullptr, "stack_top");
	builder.CreateStore(builder.getInt32(0), stack_top);

	llvm::AllocaInst* pc =
			builder.CreateAlloca(int32_type, nullptr, "pc");
	builder.CreateStore(builder.getInt32(0), stack_top);

	llvm::AllocaInst* alloc_temp_1 = builder.CreateAlloca(value_type, nullptr, "alloc_temp_1");
	llvm::AllocaInst* alloc_temp_2 = builder.CreateAlloca(value_type, nullptr, "alloc_temp_2");
	llvm::AllocaInst* alloc_temp_3 = builder.CreateAlloca(value_type, nullptr, "alloc_temp_3");


	auto size = chunk->size();
	auto jump_blocks = jumpBlocks(chunk);

	std::vector<llvm::BasicBlock*> blocks(size, nullptr);
	for (auto val: jump_blocks)
	{
		auto name = (std::to_string(val) + std::string("_bb")).c_str();
		blocks[val] = llvm::BasicBlock::Create(context, name, jit_func);
	}

	builder.CreateBr(blocks[0]);
	for (auto offset = 0u; offset < size;)
	{
		if (jump_blocks.find(offset) != jump_blocks.end()) {
			//set block
			builder.SetInsertPoint(blocks[offset]);
		}

		auto instruction = chunk->get(offset);
		switch (instruction)
		{
			case OpCode::CONSTANT:
			{
				// get index_
				auto index_ = chunk->get(offset + 1u);
				auto index_val = builder.getInt32(index_);

				// get constant
				llvm::Value* constant_addr = builder.CreateInBoundsGEP(constants, {index_val}, "constant_addr");
				llvm::Value* constant = builder.CreateLoad(constant_addr, "constant");


				// push constant to stack
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeStart(elem_addr, value_size);
				builder.CreateStore(constant, elem_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(2), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 2]);
				offset += 2;
				break;
			}
			case OpCode::CONSTANT_LONG:
			{
				// get index_
				uint32_t index_ = chunk->get(offset + 1u) |
									static_cast<uint32_t>(chunk->get(offset + 2u) << 8u) |
									static_cast<uint32_t>(chunk->get(offset + 3) << 16u);
				auto index_val = builder.getInt32(index_);

				// get constant
				llvm::Value* constant_addr = builder.CreateInBoundsGEP(constants, {index_val}, "constant_addr");
				llvm::Value* constant = builder.CreateLoad(constant_addr, "constant");


				// push constant to stack
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeStart(elem_addr, value_size);
				builder.CreateStore(constant, elem_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(4), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 4]);
				offset += 4;
				break;
			}
			case OpCode::NIL:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeStart(elem_addr, value_size);

				builder.CreateStore(llvm::ConstantStruct::get(value_type,
						{type_nil, llvm::ConstantFP::get(double_type, 0.0)}), elem_addr);;

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::TRUE:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeStart(elem_addr, value_size);

				llvm::Value* elem_type_ptr = builder.CreateStructGEP(value_type, elem_addr, 0, "type_ptr");
				builder.CreateStore(type_bool, elem_type_ptr);

				llvm::Value* elem_value_ptr = builder.CreateStructGEP(value_type, elem_addr, 1, "value_ptr");
				llvm::Value* elem_bool_ptr = builder.CreateBitCast(elem_value_ptr, boolPtr_type, "elem_bool_ptr");

				builder.CreateStore(builder.getTrue(), elem_bool_ptr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::FALSE:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeStart(elem_addr, value_size);

				llvm::Value* elem_type_ptr = builder.CreateStructGEP(value_type, elem_addr, 0, "type_ptr");
				builder.CreateStore(type_bool, elem_type_ptr);

				llvm::Value* elem_value_ptr = builder.CreateStructGEP(value_type, elem_addr, 1, "value_ptr");
				llvm::Value* elem_bool_ptr = builder.CreateBitCast(elem_value_ptr, boolPtr_type, "elem_bool_ptr");

				builder.CreateStore(builder.getFalse(), elem_bool_ptr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::POP:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* dec_stacktop = builder.CreateSub(stacktop, const_1, "dec_stacktop");
				builder.CreateStore(dec_stacktop, stack_top);
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeEnd(elem_addr, value_size);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::DUP:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp = builder.CreateSub(stacktop, const_1);

				llvm::Value* temp_addr = builder.CreateInBoundsGEP(stack, {temp}, "elem_addr");
				llvm::Value* temp_elem = builder.CreateLoad(temp_addr, "elem");

				llvm::Value* temp2_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem2_addr");
				//builder.CreateLifetimeStart(temp2_addr, value_size);
				builder.CreateStore(temp_elem, temp2_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::GET_LOCAL:
			{
				// get slot_
				auto slot_ = chunk->get(offset + 1u);
				auto slot_val = builder.getInt32(slot_);

				llvm::Value* slot_addr = builder.CreateInBoundsGEP(stack, {slot_val}, "slot_addr");
				llvm::Value* slot_elem = builder.CreateLoad(slot_addr, "slot_elem");

				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "temp_addr");
				//builder.CreateLifetimeStart(temp_addr, value_size);
				builder.CreateStore(slot_elem, temp_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(2), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 2]);
				offset += 2;
				break;
			}
			case OpCode::GET_LOCAL_SHORT:
			{
				// get const_
				uint16_t slot_ = chunk->get(offset + 1u) |
								static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);
				auto slot_val = builder.getInt32(slot_);

				llvm::Value* slot_addr = builder.CreateInBoundsGEP(stack, {slot_val}, "slot_addr");
				llvm::Value* slot_elem = builder.CreateLoad(slot_addr, "slot_elem");

				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "temp_addr");
				//builder.CreateLifetimeStart(temp_addr);
				builder.CreateStore(slot_elem, temp_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 3]);
				offset += 3;
				break;
			}
			case OpCode::SET_LOCAL:
			{
				// get const_
				auto slot_ = chunk->get(offset + 1u);
				auto slot_val = builder.getInt32(slot_);

				// get top elem
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp = builder.CreateSub(stacktop, const_1);

				llvm::Value* top_elem_addr = builder.CreateInBoundsGEP(stack, {temp}, "top_elem_addr");
				llvm::Value* top_elem = builder.CreateLoad(top_elem_addr, "top_elem");

				// store in slot
				llvm::Value* slot_addr = builder.CreateInBoundsGEP(stack, {slot_val}, "slot_addr");
				builder.CreateStore(top_elem, slot_addr);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(2), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 2]);
				offset += 2;
				break;
			}
			case OpCode::SET_LOCAL_SHORT:
			{
				// get const_
				uint16_t slot_ = chunk->get(offset + 1u) |
								  static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);
				auto slot_val = builder.getInt32(slot_);

				// get top elem
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp = builder.CreateSub(stacktop, const_1);

				llvm::Value* top_elem_addr = builder.CreateInBoundsGEP(stack, {temp}, "top_elem_addr");
				llvm::Value* top_elem = builder.CreateLoad(top_elem_addr, "top_elem");

				// store in slot
				llvm::Value* slot_addr = builder.CreateInBoundsGEP(stack, {slot_val}, "slot_addr");
				builder.CreateStore(top_elem, slot_addr);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 3]);
				offset += 3;
				break;
			}
			case OpCode::GET_GLOBAL:
			{
				// get const_
				auto index_ = chunk->get(offset + 1u);
				auto index_val = builder.getInt32(index_);

				// get value from globals
				llvm::Value* val_addr = builder.CreateInBoundsGEP(globals, {index_val}, "val_addr");
				llvm::Value* val = builder.CreateLoad(val_addr, "val");

				llvm::Value* val_type_ptr = builder.CreateStructGEP(value_type, val_addr, 0, "type_ptr");
				llvm::Value* val_type = builder.CreateLoad(val_type_ptr, false, "val_type");
				llvm::Value* comp_1 = builder.CreateICmpEQ(val_type, type_undefined, "is_undefined");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_1, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				// call variableError
				builder.CreateCall(variableError_func, {vm_, index_val});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);
				// push value
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeStart(elem_addr, value_size);
				builder.CreateStore(val, elem_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(2), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 2]);
				offset += 2;
				break;
			}
			case OpCode::GET_GLOBAL_LONG:
			{
				// get const_
				uint32_t index_ = chunk->get(offset + 1u) |
								static_cast<uint32_t>(chunk->get(offset + 2u) << 8u) |
								static_cast<uint32_t>(chunk->get(offset + 3u) << 16u);
				auto index_val = builder.getInt32(index_);

				// get value from globals
				llvm::Value* val_addr = builder.CreateInBoundsGEP(globals, {index_val}, "val_addr");
				llvm::Value* val = builder.CreateLoad(val_addr, "val");

				llvm::Value* val_type_ptr = builder.CreateStructGEP(value_type, val_addr, 0, "type_ptr");
				llvm::Value* val_type = builder.CreateLoad(val_type_ptr, false, "val_type");
				llvm::Value* comp_1 = builder.CreateICmpEQ(val_type, type_undefined, "is_undefined");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_1, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				// call variableError
				builder.CreateCall(variableError_func, {vm_, index_val});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);
				// push value
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				//builder.CreateLifetimeStart(elem_addr, value_size);
				builder.CreateStore(val, elem_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(4), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 4]);
				offset += 4;
				break;
			}
			case OpCode::DEFINE_GLOBAL:
			{
				// get index_
				auto index_ = chunk->get(offset + 1u);
				auto index_val = builder.getInt32(index_);

				// get element at stack_top - 1
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* stack_val_addr = builder.CreateInBoundsGEP(stack, {temp}, "stack_val_addr");
				llvm::Value* stack_val = builder.CreateLoad(stack_val_addr, "stack_val");

				// store element in globals at index
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(globals, {index_val}, "elem_addr");
				builder.CreateStore(stack_val, elem_addr);

				// pop value from stack
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(stack_val_addr, value_size);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(2), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 2]);
				offset += 2;
				break;
			}
			case OpCode::DEFINE_GLOBAL_LONG:
			{
				// get index_
				uint32_t index_ = chunk->get(offset + 1u) |
								  static_cast<uint32_t>(chunk->get(offset + 2u) << 8u) |
								  static_cast<uint32_t>(chunk->get(offset + 3u) << 16u);
				auto index_val = builder.getInt32(index_);

				// get element at stack_top - 1
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* stack_val_addr = builder.CreateInBoundsGEP(stack, {temp}, "stack_val_addr");
				llvm::Value* stack_val = builder.CreateLoad(stack_val_addr, "stack_val");

				// store element in globals at index
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(globals, {index_val}, "elem_addr");
				builder.CreateStore(stack_val, elem_addr);

				// pop value from stack
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(stack_val_addr, value_size);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(4), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 4]);
				offset += 4;
				break;
			}
			case OpCode::SET_GLOBAL:
			{
				// get index_
				auto index_ = chunk->get(offset + 1u);
				auto index_val = builder.getInt32(index_);

				llvm::Value* val_addr = builder.CreateInBoundsGEP(globals, {index_val}, "val_addr");

				llvm::Value* val_type_addr = builder.CreateStructGEP(value_type, val_addr, 0, "val_type_addr");
				llvm::Value* val_type = builder.CreateLoad(val_type_addr, "val_type");
				llvm::Value* comp_1 = builder.CreateICmpEQ(val_type, type_undefined, "is_undefined");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_1, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				// call variableError
				builder.CreateCall(variableError_func, {vm_, index_val});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);

				// get element at stack_top - 1
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* stack_val_addr = builder.CreateInBoundsGEP(stack, {temp}, "stack_val_addr");
				llvm::Value* stack_val = builder.CreateLoad(stack_val_addr, "stack_val");

				// store element in globals at index
				builder.CreateStore(stack_val, val_addr);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(2), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 2]);
				offset += 2;
				break;
			}
			case OpCode::SET_GLOBAL_LONG:
			{
				// get const_
				uint32_t index_ = chunk->get(offset + 1u) |
								  static_cast<uint32_t>(chunk->get(offset + 2u) << 8u) |
								  static_cast<uint32_t>(chunk->get(offset + 3u) << 16u);
				auto index_val = builder.getInt32(index_);

				llvm::Value* val_addr = builder.CreateInBoundsGEP(globals, {index_val}, "val_addr");

				llvm::Value* val_type_addr = builder.CreateStructGEP(value_type, val_addr, 0, "val_type_addr");
				llvm::Value* val_type = builder.CreateLoad(val_type_addr, "val_type");
				llvm::Value* comp_1 = builder.CreateICmpEQ(val_type, type_undefined, "is_undefined");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_1, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				// call variableError
				builder.CreateCall(variableError_func, {vm_, index_val});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);

				// get element at stack_top - 1
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* stack_val_addr = builder.CreateInBoundsGEP(stack, {temp}, "stack_val_addr");
				llvm::Value* stack_val = builder.CreateLoad(stack_val_addr, "stack_val");

				// store element in globals at index
				builder.CreateStore(stack_val, val_addr);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(4), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 4]);
				offset += 4;
				break;
			}
			case OpCode::EQUAL:
			{
				// call equal func
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");

				llvm::Value* res = builder.CreateCall(equal_func, {a_addr, b_addr});

				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_value_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_value_addr");
				llvm::Value* a_bool_addr = builder.CreateBitCast(a_value_addr, boolPtr_type, "a_bool_addr");

				builder.CreateStore(type_bool, a_type_addr);
				builder.CreateStore(res, a_bool_addr);

				// pop
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr, value_size);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::GREATER:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");


				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_type = builder.CreateLoad(a_type_addr, "a_type");
				llvm::Value* b_type_addr = builder.CreateStructGEP(value_type, b_addr, 0, "b_type_addr");
				llvm::Value* b_type = builder.CreateLoad(b_type_addr, "b_type");

				auto comp_1 = builder.CreateICmpNE(a_type, type_number, "comp_1");
				auto comp_2 = builder.CreateICmpNE(b_type, type_number, "comp_2");
				auto comp_3 = builder.CreateOr(comp_1, comp_2, "comp_3");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				builder.CreateCall(numberError_func, {vm_, pc_});

				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));


				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_number = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_number = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* less_cmp = builder.CreateFCmpOGT(a_number, b_number, "less_cmp");

				// store result
				builder.CreateStore(type_bool, a_type_addr);
				llvm::Value* a_bool_ptr = builder.CreateBitCast(a_number_addr, boolPtr_type, "a_bool_ptr");
				builder.CreateStore(less_cmp, a_bool_ptr);

				// pop value
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr, value_size);

				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::LESS:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");


				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_type = builder.CreateLoad(a_type_addr, "a_type");
				llvm::Value* b_type_addr = builder.CreateStructGEP(value_type, b_addr, 0, "b_type_addr");
				llvm::Value* b_type = builder.CreateLoad(b_type_addr, "b_type");

				auto comp_1 = builder.CreateICmpNE(a_type, type_number, "comp_1");
				auto comp_2 = builder.CreateICmpNE(b_type, type_number, "comp_2");
				auto comp_3 = builder.CreateOr(comp_1, comp_2, "comp_3");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				builder.CreateCall(numberError_func, {vm_, pc_});

				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));


				builder.SetInsertPoint(else_bb);
				builder.CreateStore(builder.CreateLoad(a_addr), alloc_temp_1);
				builder.CreateStore(builder.CreateLoad(b_addr), alloc_temp_2);
				llvm::Value* temp1_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_number = builder.CreateLoad(temp1_number_addr, "a_number");

				llvm::Value* temp2_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_number = builder.CreateLoad(temp2_number_addr, "b_number");

				llvm::Value* less_cmp = builder.CreateFCmpOLT(a_number, b_number, "less_cmp");

				// store result
				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				builder.CreateStore(type_bool, a_type_addr);
				llvm::Value* a_bool_ptr = builder.CreateBitCast(a_number_addr, boolPtr_type, "a_bool_ptr");
				builder.CreateStore(less_cmp, a_bool_ptr);

				// pop value
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr, value_size);

				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::ADD:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");


				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_type = builder.CreateLoad(a_type_addr, "a_type");
				llvm::Value* b_type_addr = builder.CreateStructGEP(value_type, b_addr, 0, "b_type_addr");
				llvm::Value* b_type = builder.CreateLoad(b_type_addr, "b_type");



				auto comp_1 = builder.CreateICmpNE(a_type, type_number, "comp_1");
				auto comp_2 = builder.CreateICmpNE(b_type, type_number, "comp_2");
				auto comp_3 = builder.CreateOr(comp_1, comp_2, "comp_3");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* error_bb = llvm::BasicBlock::Create(context, "error", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);
				llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(context, "end", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);

				builder.CreateStore(builder.CreateLoad(a_addr), alloc_temp_1);
				builder.CreateStore(builder.CreateLoad(b_addr), alloc_temp_2);

				llvm::Value* status = builder.CreateCall(concatenate_func, {vm_, alloc_temp_1, alloc_temp_2, alloc_temp_3}, "status");
				builder.CreateStore(builder.CreateLoad(alloc_temp_3), a_addr);

				llvm::Value* _ok = builder.getInt32(static_cast<int32_t>(InterpretResult::OK));
				llvm::Value* cmp_status = builder.CreateICmpEQ(status, _ok, "cmp_status");
				builder.CreateCondBr(cmp_status, end_bb, error_bb);

				builder.SetInsertPoint(error_bb);
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_number = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_number = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* res = builder.CreateFAdd(a_number, b_number);

				// store result
				builder.CreateStore(res, a_number_addr);
				builder.CreateBr(end_bb);

				builder.SetInsertPoint(end_bb);
				// pop value
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr, value_size);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::SUBTRACT:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");


				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_type = builder.CreateLoad(a_type_addr, "a_type");
				llvm::Value* b_type_addr = builder.CreateStructGEP(value_type, b_addr, 0, "b_type_addr");
				llvm::Value* b_type = builder.CreateLoad(b_type_addr, "b_type");

				auto comp_1 = builder.CreateICmpNE(a_type, type_number, "comp_1");
				auto comp_2 = builder.CreateICmpNE(b_type, type_number, "comp_2");
				auto comp_3 = builder.CreateOr(comp_1, comp_2, "comp_3");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				builder.CreateCall(numberError_func, {vm_, pc_});

				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));


				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_numer = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_numer = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* res = builder.CreateFSub(a_numer, b_numer);

				// store result
				builder.CreateStore(res, a_number_addr);

				// pop value
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr,value_size);

				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::MULTIPLY:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");


				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_type = builder.CreateLoad(a_type_addr, "a_type");
				llvm::Value* b_type_addr = builder.CreateStructGEP(value_type, b_addr, 0, "b_type_addr");
				llvm::Value* b_type = builder.CreateLoad(b_type_addr, "b_type");

				auto comp_1 = builder.CreateICmpNE(a_type, type_number, "comp_1");
				auto comp_2 = builder.CreateICmpNE(b_type, type_number, "comp_2");
				auto comp_3 = builder.CreateOr(comp_1, comp_2, "comp_3");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				builder.CreateCall(numberError_func, {vm_, pc_});

				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));


				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_numer = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_numer = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* res = builder.CreateFMul(a_numer, b_numer);

				// store result
				builder.CreateStore(res, a_number_addr);

				// pop value
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr, value_size);

				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::DIVIDE:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");


				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_type = builder.CreateLoad(a_type_addr, "a_type");
				llvm::Value* b_type_addr = builder.CreateStructGEP(value_type, b_addr, 0, "b_type_addr");
				llvm::Value* b_type = builder.CreateLoad(b_type_addr, "b_type");

				auto comp_1 = builder.CreateICmpNE(a_type, type_number, "comp_1");
				auto comp_2 = builder.CreateICmpNE(b_type, type_number, "comp_2");
				auto comp_3 = builder.CreateOr(comp_1, comp_2, "comp_3");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				builder.CreateCall(numberError_func, {vm_, pc_});

				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));


				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_numer = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_numer = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* res = builder.CreateFDiv(a_numer, b_numer);

				// store result
				builder.CreateStore(res, a_number_addr);

				// pop value
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr, value_size);

				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::MODULO:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* b_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				// get top - 1 element
				llvm::Value* temp2 = builder.CreateSub(stacktop, const_2, "temp2");
				llvm::Value* a_addr = builder.CreateInBoundsGEP(stack, {temp2}, "a_addr");


				llvm::Value* a_type_addr = builder.CreateStructGEP(value_type, a_addr, 0, "a_type_addr");
				llvm::Value* a_type = builder.CreateLoad(a_type_addr, "a_type");
				llvm::Value* b_type_addr = builder.CreateStructGEP(value_type, b_addr, 0, "b_type_addr");
				llvm::Value* b_type = builder.CreateLoad(b_type_addr, "b_type");

				auto comp_1 = builder.CreateICmpNE(a_type, type_number, "comp_1");
				auto comp_2 = builder.CreateICmpNE(b_type, type_number, "comp_2");
				auto comp_3 = builder.CreateOr(comp_1, comp_2, "comp_3");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				builder.CreateCall(numberError_func, {vm_, pc_});

				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));


				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_numer = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_numer = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* res = builder.CreateFRem(a_numer, b_numer);

				// store result
				builder.CreateStore(res, a_number_addr);

				// pop value
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(b_addr, value_size);

				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::NOT:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* val_addr = builder.CreateInBoundsGEP(stack, {temp}, "val_addr");

				// call is_falsey
				llvm::Value* result = builder.CreateCall(is_falsey, {val_addr}, "result");

				// store result
				llvm::Value* val_type_addr = builder.CreateStructGEP(value_type, val_addr, 0, "val_type_addr");
				llvm::Value* val_value_addr = builder.CreateStructGEP(value_type, val_addr, 1, "val_value_addr");
				llvm::Value* val_bool_ptr = builder.CreateBitCast(val_value_addr, boolPtr_type, "val_bool_ptr");

				builder.CreateStore(type_bool, val_type_addr);
				builder.CreateStore(result, val_bool_ptr);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::NEGATE:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* val_addr = builder.CreateInBoundsGEP(stack, {temp}, "val_addr");

				llvm::Value* val_type_addr = builder.CreateStructGEP(value_type, val_addr, 0, "val_type_addr");
				llvm::Value* val_type = builder.CreateLoad(val_type_addr, "val_type");

				llvm::Value* cmp = builder.CreateICmpNE(val_type, type_number);

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(cmp, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				builder.CreateCall(numberError_func, {vm_, pc_});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);
				llvm::Value* val_number_addr = builder.CreateStructGEP(value_type, val_addr, 1, "val_number_addr");
				llvm::Value* val_number = builder.CreateLoad(val_number_addr, "val_number");

				llvm::Value* res = builder.CreateFNeg(val_number, "res");

				// store result
				builder.CreateStore(res, val_number_addr);

				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::PRINT:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");

				llvm::Value* val_addr = builder.CreateInBoundsGEP(stack, {temp}, "val_addr");
				builder.CreateStore(builder.CreateLoad(val_addr), alloc_temp_1);

				builder.CreateCall(print_func, {alloc_temp_1});

				// pop element
				builder.CreateStore(temp, stack_top);
				//builder.CreateLifetimeEnd(val_addr, value_size);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::JUMP:
			{
				// jump
				uint16_t jump = chunk->get(offset + 1u) |
								static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 3 + jump]);
				offset += 3;
				break;
			}
			case OpCode::JUMP_IF_FALSE:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* val_addr = builder.CreateInBoundsGEP(stack, {temp}, "val_addr");

				llvm::Value* res = builder.CreateCall(is_falsey, {val_addr}, "res");

				// jump
				uint16_t jump = chunk->get(offset + 1u) |
								static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);

				llvm::Value* cmp = builder.CreateICmpEQ(res, builder.getTrue());

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateCondBr(cmp, blocks[offset + 3 + jump], blocks[offset + 3]);
				offset += 3;
				break;
			}
			case OpCode::JUMP_IF_TRUE:
			{
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");

				// get top element
				llvm::Value* temp = builder.CreateSub(stacktop, const_1, "temp");
				llvm::Value* val_addr = builder.CreateInBoundsGEP(stack, {temp}, "val_addr");

				llvm::Value* res = builder.CreateCall(is_falsey, {val_addr}, "res");

				// jump
				uint16_t jump = chunk->get(offset + 1u) |
				                static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);

				llvm::Value* cmp = builder.CreateICmpEQ(res, builder.getFalse());

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateCondBr(cmp, blocks[offset + 3 + jump], blocks[offset + 3]);
				offset += 3;
				break;
			}
			case OpCode::JUMP_BACK:
			{
				// jump
				uint16_t jump = chunk->get(offset + 1u) |
				                static_cast<uint16_t>(chunk->get(offset + 2u) << 8u);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 3 - jump]);
				offset += 3;
				break;
			}
			case OpCode::RETURN:
			{
				offset += 1;
				break;
			}
			default:
				offset += 1;
				break;
		}
	}

	builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::OK)));
	return jit_func;
}
//
// Created by juanb on 12/07/2019.
//

#include <cmath>
#include <vector>
#include <iomanip>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>

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

extern "C" __declspec(dllexport) void callError(VM* vm, uint32_t pc)
{
	using namespace std::string_literals;
	vm->runtimeError(pc, "Object is not callable."s);
}

extern "C" __declspec(dllexport) void numberError(VM* vm, uint32_t pc)
{
	using namespace std::string_literals;
	vm->runtimeError(pc, "Operands must be numbers."s);
}

extern "C" __declspec(dllexport) void variableError(VM *vm, uint32_t pos, uint32_t pc)
{
	using namespace std::string_literals;
	vm->runtimeError(pc, "Undefined variable "s, vm->m_globalNames[pos], "."s);
}

extern "C" __declspec(dllexport) void arityError(VM *vm, uint32_t arity, uint32_t arg_count, uint32_t pc)
{
	using namespace std::string_literals;
	vm->runtimeError(pc, "Expected "s, std::to_string(arity), " arguments but got "s, std::to_string(arg_count), "."s);
}

extern "C" __declspec(dllexport) bool equal(Value *a, Value *b)
{
	return *a == *b;
}

extern "C" __declspec(dllexport) int concatenate(VM *vm, Value *out, Value *a, Value *b, uint32_t pc)
{
	if (a->isObjString() && b->isObjString())
	{
		*out = Value::Object(Memory::createString(vm, a->asObjString()->value + b->asObjString()->value));
	}
	else if(a->isNumber() && b->isObjString())
	{
		auto b_str = b->asObjString()->value;
		auto a_num = a->asNumber();
		char buff[1024];
		std::sprintf(buff, "%g", a_num);
		*out = Value::Object(Memory::createString(vm, std::string(buff) + b_str));
	}
	else if(a->isObjString() && b->isNumber())
	{
		auto b_num = b->asNumber();
		auto a_str = a->asObjString()->value;
		char buff[1024];
		std::sprintf(buff, "%g", b_num);
		*out = Value::Object(Memory::createString(vm, a_str + std::string(buff)));
	}
	else
	{
		vm->runtimeError(pc, "Operands must be numbers or strings.");
		return (int)InterpretResult::RUNTIME_ERROR;
	}
	return (int)InterpretResult::OK;
}

extern "C" __declspec(dllexport) void print(Value* val)
{
	std::cout << *val << std::endl;
}

extern "C" __declspec(dllexport) void callNative(NativeFn fun, uint32_t argCount, Value *args, Value *out)
{
	*out = fun(argCount, args);
}

std::vector<uint32_t> jumpBlocks(Chunk* chunk)
{
	auto size = chunk->size();
	std::vector<uint32_t> labels;

	for (auto offset = 0u; offset < size;)
	{
		labels.push_back(offset);
		auto instruction = chunk->get(offset);
		switch (instruction)
		{
			case OpCode::CONSTANT:
			case OpCode::GET_LOCAL:
			case OpCode::SET_LOCAL:
			case OpCode::GET_GLOBAL:
			case OpCode::DEFINE_GLOBAL:
			case OpCode::SET_GLOBAL:
			case OpCode::CALL:
				offset += 2;
				break;
			case OpCode::GET_LOCAL_SHORT:
			case OpCode::SET_LOCAL_SHORT:
			case OpCode::JUMP:
			case OpCode::JUMP_IF_FALSE:
			case OpCode::JUMP_IF_TRUE:
			case OpCode::JUMP_BACK:
				offset += 3;
				break;
			case OpCode::CONSTANT_LONG:
			case OpCode::GET_GLOBAL_LONG:
			case OpCode::DEFINE_GLOBAL_LONG:
			case OpCode::SET_GLOBAL_LONG:
				offset += 4;
				break;
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

	llvm::Argument* a_ptr = jit_func->arg_begin();
	llvm::Argument* b_ptr = jit_func->arg_begin() + 1;

	llvm::Value* a_type_ptr = builder.CreateStructGEP(value_type, a_ptr, 0, "type_ptr");
	llvm::Value* a_type = builder.CreateLoad(a_type_ptr, false, "val_type");

	llvm::Value* b_type_ptr = builder.CreateStructGEP(value_type, b_ptr, 0, "type_ptr");
	llvm::Value* b_type = builder.CreateLoad(b_type_ptr, false, "val_type");

	llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", jit_func);
	llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);
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

void gen_if_else_jmp(llvm::Value* cmp, llvm::AllocaInst* pc, int then_off, int else_off, llvm::IRBuilder<>& builder, 
	llvm::LLVMContext& context, llvm::Function* jit_func, llvm::BasicBlock* rtn)
{
	llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then_jmp", jit_func);
	llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else_jmp", jit_func);
	llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(context, "end_jmp", jit_func);

	builder.CreateCondBr(cmp, then_bb, else_bb);

	builder.SetInsertPoint(then_bb);
	llvm::Value* pc_ = builder.CreateLoad(pc, "pc_jmp");
	llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(then_off), "inc_pc_jmp");
	builder.CreateStore(inc_pc, pc);
	builder.CreateBr(end_bb);
	
	builder.SetInsertPoint(else_bb);
	pc_ = builder.CreateLoad(pc, "pc_jmp");
	inc_pc = builder.CreateAdd(pc_, builder.getInt32(else_off), "inc_pc_jmp");
	builder.CreateStore(inc_pc, pc);
	builder.CreateBr(end_bb);

	builder.SetInsertPoint(end_bb);
	builder.CreateBr(rtn);
}

llvm::Function* generate_main(llvm::Module* module, const std::string& name, llvm::StructType* value_type, llvm::PointerType* valutePtr_type)
{
	llvm::LLVMContext& context = module->getContext();

	llvm::Type* int32_type = llvm::Type::getInt32Ty(context);
	llvm::Type* void_type = llvm::Type::getVoidTy(context);
	llvm::PointerType* voidPtr_type = llvm::Type::getInt8PtrTy(context);

	// Create function signature and object. int (*)(VM* vm, Value* globals)
	llvm::FunctionType* main_func_type =
		llvm::FunctionType::get(int32_type, {voidPtr_type, valutePtr_type, valutePtr_type}, false);
	llvm::Function* main_func = llvm::Function::Create(
		main_func_type, llvm::Function::ExternalLinkage, name, module);
	main_func->setDLLStorageClass(llvm::Function::DLLExportStorageClass);

	llvm::Argument* vm_ = main_func->arg_begin();
	llvm::Argument* globals = main_func->arg_begin() + 1;
	//llvm::Argument* stack = main_func->arg_begin() + 2;

	llvm::BasicBlock* entry_bb =
		llvm::BasicBlock::Create(context, "main_entry", main_func);
	llvm::IRBuilder<> builder(entry_bb);

	llvm::AllocaInst* stack =
		builder.CreateAlloca(value_type, builder.getInt32(12500), "stack");
	llvm::AllocaInst* stack_top =
		builder.CreateAlloca(int32_type, nullptr, "stack_top");
	builder.CreateStore(builder.getInt32(1), stack_top);

	llvm::Function* _jit_func = module->getFunction("_jit_func");
	llvm::Value* res = builder.CreateCall(_jit_func, {vm_, globals, stack, stack_top}, "jit_func");


	builder.CreateRet(res);
	return main_func;
}

llvm::Function* generade_code(llvm::Module* module, Chunk* chunk, const std::string& name, llvm::GlobalValue::LinkageTypes linkage, llvm::StructType* value_type, llvm::PointerType* valutePtr_type)
{
	llvm::LLVMContext& context = module->getContext();

	// Add a declaration for external functions used in the JITed code. We use
	llvm::Type* uint64_type = llvm::Type::getInt64Ty(context);
	llvm::Type* int32_type = llvm::Type::getInt32Ty(context);
	llvm::Type* void_type = llvm::Type::getVoidTy(context);
	llvm::Type* uint8_type = llvm::Type::getInt8Ty(context);
	llvm::Type* double_type = llvm::Type::getDoubleTy(context);
	llvm::Type* bool_type = llvm::Type::getInt1Ty(context);
	llvm::ArrayType* chunk_type = llvm::ArrayType::get(uint8_type, sizeof(Chunk));

	llvm::PointerType* voidPtr_type = llvm::Type::getInt8PtrTy(context);
	llvm::PointerType* in64Ptr_type = llvm::Type::getInt64PtrTy(context);
	llvm::PointerType* in32Ptr_type = llvm::Type::getInt32PtrTy(context);
	llvm::Type* uint8Ptr_type = llvm::Type::getInt8PtrTy(context);
	llvm::PointerType* boolPtr_type = llvm::Type::getInt1PtrTy(context);

	llvm::StructType* obj_type = llvm::StructType::create(context, {uint8Ptr_type, uint64_type, uint8_type}, "Obj");
	llvm::PointerType* objPtr_type = llvm::PointerType::get(obj_type, 0);
	llvm::PointerType* objPtrPtr_type = llvm::PointerType::get(objPtr_type, 0);

	llvm::StructType* objFunction_type = llvm::StructType::create(context,{uint8Ptr_type, uint64_type, uint8_type, int32_type, uint8Ptr_type, uint8Ptr_type, chunk_type}, "objFunction");
	llvm::PointerType* objFunctionPtr_type = llvm::PointerType::get(objFunction_type, 0);

	llvm::StructType* objNative_type = llvm::StructType::create(context,{uint8Ptr_type, uint64_type, uint8_type, uint8Ptr_type}, "objNative");
	llvm::PointerType* objNativePtr_type = llvm::PointerType::get(objNative_type, 0);

	llvm::Function* callError_func = module->getFunction("callError");
	llvm::Function* numberError_func = module->getFunction("numberError");
	llvm::Function* variableError_func = module->getFunction("variableError");
	llvm::Function* arityError_func = module->getFunction("arityError");
	llvm::Function* concatenate_func = module->getFunction("concatenate");
	llvm::Function* print_func = module->getFunction("print");
	llvm::Function* callNative_func = module->getFunction("callNative");

	llvm::Function* is_falsey = module->getFunction("_is_falsey");

	llvm::Function* equal_func = module->getFunction("_equal");

	// Create function signature and object. int (*)(VM*, Value*, Value*, int*)
	llvm::FunctionType* jit_func_type =
		llvm::FunctionType::get(int32_type, {voidPtr_type, valutePtr_type, valutePtr_type, in32Ptr_type}, false);
	llvm::Function* jit_func = llvm::Function::Create(
		jit_func_type, linkage, name, module);
	if (linkage != llvm::Function::InternalLinkage)
		jit_func->setDLLStorageClass(llvm::Function::DLLExportStorageClass);

	llvm::Argument* vm_ = jit_func->arg_begin();
	llvm::Argument* globals = jit_func->arg_begin() + 1;
	llvm::Argument* stack_ = jit_func->arg_begin() + 2;
	llvm::Argument* stack_top = jit_func->arg_begin() + 3;

	llvm::BasicBlock* entry_bb =
		llvm::BasicBlock::Create(context, "entry", jit_func);
	llvm::BasicBlock* return_bb =
		llvm::BasicBlock::Create(context, "return", jit_func);
	llvm::IRBuilder<> builder(entry_bb);

	auto const_0 = builder.getInt32(0);
	auto const_1 = builder.getInt32(1);
	auto const_2 = builder.getInt32(2);

	auto type_number = builder.getInt8(static_cast<uint8_t>(ValueType::NUMBER));
	auto type_bool = builder.getInt8(static_cast<uint8_t>(ValueType::BOOL));
	auto type_obj = builder.getInt8(static_cast<uint8_t>(ValueType::OBJ));
	auto type_nil = builder.getInt8(static_cast<uint8_t>(ValueType::NIL));
	auto type_undefined = builder.getInt8(static_cast<uint8_t>(ValueType::UNDEFINED));

	auto type_obj_function = builder.getInt8(static_cast<uint8_t>(ObjType::FUNCTION));
	auto type_obj_native = builder.getInt8(static_cast<uint8_t>(ObjType::NATIVE));
	auto type_obj_string = builder.getInt8(static_cast<uint8_t>(ObjType::STRING));

	llvm::AllocaInst* constants =
		builder.CreateAlloca(value_type, builder.getInt32(chunk->constantsSize()), "constants");
	for (size_t i = 0; i < chunk->constantsSize(); ++i)
	{
		auto &constant = chunk->constants()[i];
		llvm::Value* elem_addr = builder.CreateInBoundsGEP(constants, {builder.getInt32(i)}, "elem_addr");
		switch (constant.type())
		{
			default:
				DIE << "Cant happen\n";
				break;
			case ValueType::NUMBER: {
				builder.CreateStore(llvm::ConstantStruct::get(value_type,
															  {type_number, llvm::ConstantFP::get(double_type,
															  	constant.asNumber())}), elem_addr);
				break;
			}
			case ValueType::OBJ: {
				auto ptr = builder.getInt64(reinterpret_cast<size_t>(constant.asObj()));
				llvm::Value* elem_type_ptr = builder.CreateStructGEP(value_type, elem_addr, 0, "type_ptr");
				builder.CreateStore(type_obj, elem_type_ptr);
				llvm::Value* elem_value_ptr = builder.CreateStructGEP(value_type, elem_addr, 1, "value_ptr");
				llvm::Value* elem_ptr = builder.CreateBitCast(elem_value_ptr, in64Ptr_type, "elem_ptr");
				builder.CreateStore(ptr, elem_ptr);
				break;
			}
		}
	}
	builder.CreateInvariantStart(constants, builder.getInt64(sizeof(Value) * chunk->constantsSize()));

	llvm::AllocaInst* pc = builder.CreateAlloca(int32_type, nullptr, "pc");
	builder.CreateStore(const_0, pc);

	llvm::AllocaInst* stack_ptr = builder.CreateAlloca(valutePtr_type, nullptr, "stack_ptr");
	builder.CreateStore(stack_, stack_ptr);
	llvm::Value* stack = builder.CreateLoad(stack_ptr, "stack");

	llvm::AllocaInst* alloc_temp_1 = builder.CreateAlloca(value_type, nullptr, "alloc_temp_1");
	llvm::AllocaInst* alloc_temp_2 = builder.CreateAlloca(value_type, nullptr, "alloc_temp_2");
	llvm::AllocaInst* alloc_temp_3 = builder.CreateAlloca(value_type, nullptr, "alloc_temp_3");


	auto size = chunk->size();
	auto jump_blocks = jumpBlocks(chunk);

	std::vector<llvm::BasicBlock*> blocks(size, nullptr);
	for (auto val: jump_blocks)
	{
		auto name_ = (std::to_string(val) + std::string("_bb"));
		blocks[val] = llvm::BasicBlock::Create(context, name_, jit_func);
	}

	builder.CreateBr(blocks[0]);
	for (auto offset = 0u; offset < size;)
	{
		//set block
		builder.SetInsertPoint(blocks[offset]);

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

				builder.CreateStore(llvm::ConstantStruct::get(value_type,
															  {type_nil, llvm::ConstantFP::get(double_type, 0.0)}), elem_addr);

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
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				builder.CreateCall(variableError_func, {vm_, index_val, pc_});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);
				// push value
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				builder.CreateStore(val, elem_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				pc_ = builder.CreateLoad(pc, "pc_");
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
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				builder.CreateCall(variableError_func, {vm_, index_val, pc_});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);
				// push value
				llvm::Value* stacktop = builder.CreateLoad(stack_top, "stacktop");
				llvm::Value* elem_addr = builder.CreateInBoundsGEP(stack, {stacktop}, "elem_addr");
				builder.CreateStore(val, elem_addr);

				llvm::Value* inc_stacktop = builder.CreateAdd(stacktop, const_1, "inc_stacktop");
				builder.CreateStore(inc_stacktop, stack_top);

				pc_ = builder.CreateLoad(pc, "pc_");
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
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				builder.CreateCall(variableError_func, {vm_, index_val, pc_});
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

				pc_ = builder.CreateLoad(pc, "pc_");
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
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				builder.CreateCall(variableError_func, {vm_, index_val, pc_});
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

				pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(4), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 4]);
				offset += 4;
				break;
			}
			case OpCode::EQUAL:
			{
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
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
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

				pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::LESS:
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
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				builder.CreateCall(numberError_func, {vm_, pc_});

				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));


				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_number = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_number = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* less_cmp = builder.CreateFCmpOLT(a_number, b_number, "less_cmp");

				// store result
				builder.CreateStore(type_bool, a_type_addr);
				llvm::Value* a_bool_ptr = builder.CreateBitCast(a_number_addr, boolPtr_type, "a_bool_ptr");
				builder.CreateStore(less_cmp, a_bool_ptr);

				// pop value
				builder.CreateStore(temp, stack_top);

				pc_ = builder.CreateLoad(pc, "pc_");
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

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* status =
					builder.CreateCall(concatenate_func, {vm_, alloc_temp_3, alloc_temp_1, alloc_temp_2, pc_}, "status");
				builder.CreateStore(builder.CreateLoad(alloc_temp_3), a_addr);
				llvm::Value* _ok = builder.getInt32(static_cast<int32_t>(InterpretResult::OK));
				llvm::Value* cmp_status = builder.CreateICmpEQ(status, _ok, "cmp_status");
				builder.CreateCondBr(cmp_status, end_bb, error_bb);

				builder.SetInsertPoint(error_bb);
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);

				llvm::Value* a_number_addr = builder.CreateStructGEP(value_type, a_addr, 1, "a_number_addr");
				llvm::Value* a_numer = builder.CreateLoad(a_number_addr, "a_number");
				llvm::Value* b_number_addr = builder.CreateStructGEP(value_type, b_addr, 1, "b_number_addr");
				llvm::Value* b_numer = builder.CreateLoad(b_number_addr, "b_number");

				llvm::Value* res = builder.CreateFAdd(a_numer, b_numer);

				// store result
				builder.CreateStore(res, a_number_addr);
				builder.CreateBr(end_bb);

				builder.SetInsertPoint(end_bb);
				// pop value
				builder.CreateStore(temp, stack_top);

				pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::SUBTRACT:
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
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
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

				pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::MULTIPLY:
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
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
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

				pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::DIVIDE:
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
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
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

				pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 1]);
				offset += 1;
				break;
			}
			case OpCode::MODULO:
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
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else", jit_func);

				builder.CreateCondBr(comp_3, then_bb, else_bb);

				builder.SetInsertPoint(then_bb);
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
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

				pc_ = builder.CreateLoad(pc, "pc_");
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
				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				builder.CreateCall(numberError_func, {vm_, pc_});
				// return error code
				builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

				builder.SetInsertPoint(else_bb);
				llvm::Value* val_number_addr = builder.CreateStructGEP(value_type, val_addr, 1, "val_number_addr");
				llvm::Value* val_number = builder.CreateLoad(val_number_addr, "val_number");

				llvm::Value* res = builder.CreateFNeg(val_number, "res");

				// store result
				builder.CreateStore(res, val_number_addr);

				pc_ = builder.CreateLoad(pc, "pc_");
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
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3 + jump), "inc_pc");
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
				llvm::BasicBlock* rtn = llvm::BasicBlock::Create(context, "rtn_jmp", jit_func);

				gen_if_else_jmp(cmp, pc, 3 + jump, 3, builder, context, jit_func, rtn);

				builder.SetInsertPoint(rtn);
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
				llvm::BasicBlock* rtn = llvm::BasicBlock::Create(context, "rtn_jmp", jit_func);

				gen_if_else_jmp(cmp, pc, 3 + jump, 3, builder, context, jit_func, rtn);

				builder.SetInsertPoint(rtn);
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
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(3 - jump), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 3 - jump]);
				offset += 3;
				break;
			}
			case OpCode::CALL:
			{
				llvm::PointerType* funcPtr_type = llvm::PointerType::get(jit_func_type, 0);
				llvm::PointerType* funcPtrPtr_type = llvm::PointerType::get(funcPtr_type, 0);

				llvm::FunctionType* native_func_type =  llvm::FunctionType::get(value_type,{int32_type, valutePtr_type}, false);
				llvm::PointerType* nativePtr_type = llvm::PointerType::get(native_func_type, 0);
				llvm::PointerType* nativePtrPtr_type = llvm::PointerType::get(nativePtr_type, 0);

				auto arg_count = chunk->get(offset + 1u);
				auto argCount = builder.getInt32(arg_count);

				// get top - argCount element
				llvm::Value* temp = builder.CreateSub(builder.CreateLoad(stack_top, "stacktop"),builder.CreateAdd(argCount, const_1, "argcount_1"), "temp");
				llvm::Value* c_addr = builder.CreateInBoundsGEP(stack, {temp}, "b_addr");

				llvm::Value* c_type_addr = builder.CreateStructGEP(value_type, c_addr, 0, "c_type_addr");
				llvm::Value* c_type = builder.CreateLoad(c_type_addr, "c_type");
				llvm::Value* c_value_addr = builder.CreateStructGEP(value_type, c_addr, 1, "c_value_addr");

				auto comp_1 = builder.CreateICmpEQ(c_type, type_obj, "comp_1");

				llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then_obj", jit_func);
				llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else_obj", jit_func);
				llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(context, "end_obj", jit_func);

				builder.CreateCondBr(comp_1, then_bb, else_bb);
				builder.SetInsertPoint(then_bb);
				{
					llvm::Value* c_obj_ptr_addr = builder.CreateBitCast(c_value_addr, objPtrPtr_type, "c_obj_ptr_addr");
					llvm::Value* c_obj_addr = builder.CreateLoad(c_obj_ptr_addr, "c_obj_addr");

					llvm::Value* c_obj_type_addr = builder.CreateStructGEP(obj_type, c_obj_addr, 2, "c_obj_type_addr");
					llvm::Value* c_obj_type = builder.CreateLoad(c_obj_type_addr, "c_obj_type");
					auto comp_function = builder.CreateICmpEQ(c_obj_type, type_obj_function, "comp_function");
					auto comp_native = builder.CreateICmpEQ(c_obj_type, type_obj_native, "type_obj_native");

					llvm::BasicBlock* then_fun_bb = llvm::BasicBlock::Create(context, "then_fun_bb", jit_func);
					llvm::BasicBlock* else_fun_bb = llvm::BasicBlock::Create(context, "else_fun_bb", jit_func);

					llvm::BasicBlock* then_nat_bb = llvm::BasicBlock::Create(context, "then_nat_bb", jit_func);
					llvm::BasicBlock* else_nat_bb = llvm::BasicBlock::Create(context, "else_nat_bb", jit_func);


					builder.CreateCondBr(comp_function, then_fun_bb, else_fun_bb);
					builder.SetInsertPoint(then_fun_bb);
					{
						// IS FUNCTION
						llvm::Value* temp_top = builder.CreateAdd(argCount, const_1, "temp_top");
						//llvm::AllocaInst* temp_stack_top = builder.CreateAlloca(int32_type, nullptr, "temp_stack_top");
						builder.CreateStore(temp_top, stack_top);

						llvm::Value* callee_obj_addr = builder.CreateBitCast(c_obj_addr, objFunctionPtr_type, "callee_obj_addr");
						// CHECK ARITY
						llvm::Value* arity_addr = builder.CreateStructGEP(objFunction_type, callee_obj_addr, 3, "arity_addr");
						llvm::Value* arity = builder.CreateLoad(arity_addr, "arity");
						auto comp_arity = builder.CreateICmpNE(argCount, arity);
						llvm::BasicBlock* then_arity_bb = llvm::BasicBlock::Create(context, "then_arity_bb", jit_func);
						llvm::BasicBlock* else_arity_bb = llvm::BasicBlock::Create(context, "else_arity_bb", jit_func);
						builder.CreateCondBr(comp_arity, then_arity_bb, else_arity_bb);
						builder.SetInsertPoint(then_arity_bb);
						// INCORRECT NUMBER OF ARGUMENTS
						llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
						builder.CreateCall(arityError_func, {vm_, arity, argCount, pc_});
						builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));

						// CORRECT NUMBER OF ARGUMENTS
						builder.SetInsertPoint(else_arity_bb);
						llvm::Value* callee_ptr_raw_addr = builder.CreateStructGEP(objFunction_type, callee_obj_addr, 5, "callee_ptr_raw_addr");
						llvm::Value* callee_ptr_addr = builder.CreateBitCast(callee_ptr_raw_addr, funcPtrPtr_type, "callee_ptr_addr");
						llvm::Value* callee_addr = builder.CreateLoad(callee_ptr_addr, "callee_addr");
						llvm::Value* status = builder.CreateCall(callee_addr, {vm_, globals, c_addr, stack_top}, "status");

						// HANDLE RUNTIME ERROR
						auto status_comp = builder.CreateICmpNE(status, builder.getInt32(static_cast<int32_t>(InterpretResult::OK)));
						llvm::BasicBlock* then_status_bb = llvm::BasicBlock::Create(context, "then_status_bb", jit_func);
						llvm::BasicBlock* else_status_bb = llvm::BasicBlock::Create(context, "else_status_bb", jit_func);
						builder.CreateCondBr(status_comp, then_status_bb, else_status_bb);
						builder.SetInsertPoint(then_status_bb);
						builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));
						builder.SetInsertPoint(else_status_bb);
						// GET RESULT
						llvm::Value* temp_st = builder.CreateSub(builder.CreateLoad(stack_top), const_1, "temp_st");
						llvm::Value* val_addr = builder.CreateInBoundsGEP(c_addr, {temp_st}, "val_addr");
						// RECOVER STACK
						builder.CreateStore(temp, stack_top);
						// SAVE RESULT
						llvm::Value* stackTop = builder.CreateLoad(stack_top, "stackTop");
						llvm::Value* res_addr = builder.CreateInBoundsGEP(stack, {stackTop}, "res");
						builder.CreateStore(builder.CreateLoad(val_addr, "val"), res_addr);
						// INCREMENT STACK
						llvm::Value* inc_stack_top = builder.CreateAdd(stackTop, const_1, "inc_stack_top");
						builder.CreateStore(inc_stack_top, stack_top);

						builder.CreateBr(end_bb);
					}
					builder.SetInsertPoint(else_fun_bb);
					{
						builder.CreateCondBr(comp_native, then_nat_bb, else_nat_bb);
						builder.SetInsertPoint(then_nat_bb);
						{
							// IS NATIVE
							llvm::Value* c_obj_fun_addr = builder.CreateBitCast(c_obj_addr, objNativePtr_type, "c_obj_nat_addr");
							llvm::Value* callee_ptr_raw_addr = builder.CreateStructGEP(objNative_type, c_obj_fun_addr, 3, "callee_ptr_raw_addr");
							llvm::Value* callee_ptr_addr = builder.CreateBitCast(callee_ptr_raw_addr, nativePtrPtr_type, "callee_ptr_addr");
							llvm::Value* callee_addr = builder.CreateLoad(callee_ptr_addr, "callee_addr");

							builder.CreateCall(callNative_func, {callee_addr, const_1, c_addr, alloc_temp_3});

							// RECOVER STACK
							builder.CreateStore(temp, stack_top);
							// SAVE RESULT
							llvm::Value* stackTop = builder.CreateLoad(stack_top, "stackTop");
							llvm::Value* res_addr = builder.CreateInBoundsGEP(stack, {stackTop}, "res");
							builder.CreateStore(builder.CreateLoad(alloc_temp_3, "val"), res_addr);
							// INCREMENT STACK
							llvm::Value* inc_stack_top = builder.CreateAdd(stackTop, const_1, "inc_stack_top");
							builder.CreateStore(inc_stack_top, stack_top);

							// finish
							builder.CreateBr(end_bb);
						}
						builder.SetInsertPoint(else_nat_bb);
						{
							// OBJECT IS NOT CALLABLE
							llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
							builder.CreateCall(callError_func, {vm_, pc_});
							// return error code
							builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));
						}
					}
				}
				builder.SetInsertPoint(else_bb);
				{
					// VALUE IS NOT CALLABLE
					llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
					builder.CreateCall(callError_func, {vm_, pc_});
					// return error code
					builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::RUNTIME_ERROR)));
				}
				builder.SetInsertPoint(end_bb);

				llvm::Value* pc_ = builder.CreateLoad(pc, "pc_");
				llvm::Value* inc_pc = builder.CreateAdd(pc_, builder.getInt32(1), "inc_pc");
				builder.CreateStore(inc_pc, pc);
				builder.CreateBr(blocks[offset + 2]);
				offset += 2;
				break;
			}
			case OpCode::RETURN:
			{
				builder.CreateBr(return_bb);
				offset += 1;
				break;
			}
			default:
				offset += 1;
				break;
		}
	}

	builder.SetInsertPoint(return_bb);
	builder.CreateRet(builder.getInt32(static_cast<int32_t>(InterpretResult::OK)));
	return jit_func;
}
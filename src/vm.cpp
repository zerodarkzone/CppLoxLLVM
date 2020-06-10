//
// Created by juanb on 29/09/2018.
//

#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <fstream>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/IPO.h>

#include "llvm_jit_utils.hpp"
#include "utils.hpp"

#include "memory.hpp"
#include "common.hpp"
#include "vm.hpp"
#include "nativeFunctions.hpp"
#include "jit.hpp"

VM::VM()
{
	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmPrinter();
	m_jit = std::make_unique<SimpleOrcJIT>(true);

	defineNative("clock", clockNative);
}

VM::~VM()
{
	Mem::freeObjects(this);
}

InterpretResult VM::interpret(const std::string &source)
{
	auto function = m_compiler.compile(this, source.data());

	if (!function)
	{
		return InterpretResult::COMPILE_ERROR;
	}
	m_stack.push(Value::Object(function));
	callValue(Value::Object(function), 0);

	auto result = runJitted();

	return result;
}

HashTable<std::string, Value>& VM::globalsMap()
{
	return m_globals;
}

std::vector<std::string>& VM::globalNames()
{
	return m_globalNames;
}

std::vector<Value>& VM::globalValues()
{
	return m_globalValues;
}

inline bool VM::call(ObjFunction* function, int argCount)
{
	using namespace std::string_literals;

	if (argCount != function->arity) {
		runtimeError("Expected "s, std::to_string(function->arity), " arguments but got "s, std::to_string(argCount), "."s);
		return false;
	}
	if (m_frameCount == FRAMES_MAX)
	{
		runtimeError("Stack overflow.");
		return false;
	}
	CallFrame* frame = &m_frames[m_frameCount++];
	frame->function = function;
	frame->ip = function->chunk.code();

	frame->slots = m_stack.getTop() - argCount - 1;
	return true;
}

inline bool VM::callValue(Value callee, int argCount)
{
	if (callee.isObj())
	{
		switch (callee.objType()) {
			case ObjType::FUNCTION:
				return call(callee.asObjFunction(), argCount);
			case ObjType::NATIVE:
			{
				auto native = callee.asObjNative();
				auto result = native->function(argCount, m_stack.getTop() - argCount);
				m_stack.getTop() -= argCount + 1;
				m_stack.push(result);
				return true;
			}
			default:
				// Non-callable object type.
				break;
		}
	}
	runtimeError("Can only call functions and classes.");
	return false;
}

void VM::defineNative(std::string_view name, NativeFn function)
{
	m_stack.push(Value::Object(Memory::createNative(this, function)));

	m_globalValues.push_back(m_stack.get(0));
	m_globalNames.emplace_back(name);
	auto newIndex = m_globalValues.size() - 1;
	m_globals[std::string(name)] = Value::Number(newIndex);

	m_stack.pop();
}

// Helper function that prints the textual LLVM IR of module into a file.
void llvm_module_to_file(const llvm::Module& module, const char* filename) {
	std::string str;
	llvm::raw_string_ostream os(str);
	module.print(os, nullptr);

	std::ofstream of(filename);
	of << os.str();
}

int test_fn(void* vm, Value* glob, Value* stack, int *stack_top)
{
	std::cout << stack[*stack_top - 1] << std::endl;
	stack[*stack_top] = Value::Number(72);
	*stack_top = *stack_top + 1;
	return static_cast<int>(InterpretResult::OK);
}

void compileFunctions(llvm::Module* module, Chunk* chunk, const std::string& name, llvm::GlobalValue::LinkageTypes Linkage, llvm::StructType* value_type, llvm::PointerType* valutePtr_type, std::vector<llvm::Function*> &functions)
{
	functions.push_back(generade_code(module, chunk, name, Linkage, value_type, valutePtr_type));
	for (size_t i = 0; i < chunk->constantsSize(); ++i)
	{
		auto &constant = chunk->constants()[i];
		if (constant.isObjFunction())
		{
			compileFunctions(module, &constant.asObjFunction()->chunk, constant.asObjFunction()->name->value, llvm::Function::ExternalLinkage, value_type, valutePtr_type, functions);
		}
	}
}

void setCompiledFunctions(Chunk* chunk, SimpleOrcJIT* jit)
{
	for (size_t i = 0; i < chunk->constantsSize(); ++i)
	{
		auto &constant = chunk->constants()[i];
		if (constant.isObjFunction())
		{
			llvm::JITSymbol func_sym = jit->find_symbol(constant.asObjFunction()->name->value);
			if (!func_sym) {
				DIE << "Unable to find symbol " << "_main_func" << " in module";
			}
			JitFn func_ptr =
				reinterpret_cast<JitFn>(func_sym.getAddress().get());
			constant.asObjFunction()->function = func_ptr;
			setCompiledFunctions(&constant.asObjFunction()->chunk, jit);
		}
	}
}

InterpretResult VM::runJitted()
{
	m_frame = &m_frames[m_frameCount - 1];
	//auto start = std::chrono::steady_clock::now();
	auto verbose = true;

	if (verbose) {
		std::cout << "Host CPU name: " << llvm::sys::getHostCPUName().str() << "\n";
		std::cout << "CPU features:\n";
		llvm::StringMap<bool> host_features;
		if (llvm::sys::getHostCPUFeatures(host_features)) {
			int linecount = 0;
			for (auto& feature : host_features) {
				if (feature.second) {
					std::cout << "  " << feature.first().str();
					if (++linecount % 4 == 0) {
						std::cout << "\n";
					}
				}
			}
		}
		std::cout << "\n";
	}

	llvm::LLVMContext context;
	std::unique_ptr<llvm::Module> module(new llvm::Module("Loxmodule", context));

	llvm::Type* uint8_type = llvm::Type::getInt8Ty(context);
	llvm::Type* double_type = llvm::Type::getDoubleTy(context);
	llvm::Type* uint64_type = llvm::Type::getInt64Ty(context);
	llvm::Type* int32_type = llvm::Type::getInt32Ty(context);
	llvm::Type* void_type = llvm::Type::getVoidTy(context);

	llvm::PointerType* voidPtr_type = llvm::Type::getInt8PtrTy(context);

	llvm::StructType* value_type = llvm::StructType::create(context, {uint8_type, double_type}, "Value");
	llvm::PointerType* valutePtr_type = llvm::PointerType::get(value_type, 0);

	llvm::Function* callError_func = llvm::Function::Create(
		llvm::FunctionType::get(void_type, {voidPtr_type, int32_type}, false),
		llvm::Function::ExternalLinkage, "callError", module.get());
	//numberError_func->setOnlyReadsMemory();
	callError_func->setOnlyAccessesArgMemory();
	callError_func->setDoesNotThrow();

	llvm::Function* numberError_func = llvm::Function::Create(
		llvm::FunctionType::get(void_type, {voidPtr_type, int32_type}, false),
		llvm::Function::ExternalLinkage, "numberError", module.get());
	//numberError_func->setOnlyReadsMemory();
	numberError_func->setOnlyAccessesArgMemory();
	numberError_func->setDoesNotThrow();

	llvm::Function* variableError_func = llvm::Function::Create(
		llvm::FunctionType::get(void_type, {voidPtr_type, int32_type, int32_type}, false),
		llvm::Function::ExternalLinkage, "variableError", module.get());
	//variableError_func->setOnlyReadsMemory();
	variableError_func->setOnlyAccessesArgMemory();
	variableError_func->setDoesNotThrow();

	llvm::Function* arityError_func = llvm::Function::Create(
		llvm::FunctionType::get(void_type, {voidPtr_type, int32_type, int32_type, int32_type}, false),
		llvm::Function::ExternalLinkage, "arityError", module.get());
	//variableError_func->setOnlyReadsMemory();
	arityError_func->setOnlyAccessesArgMemory();
	arityError_func->setDoesNotThrow();

	llvm::Function* concatenate_func = llvm::Function::Create(
		llvm::FunctionType::get(int32_type,
								{voidPtr_type, valutePtr_type, valutePtr_type, valutePtr_type, int32_type}, false),
		llvm::Function::ExternalLinkage, "concatenate", module.get());
	concatenate_func->setOnlyAccessesArgMemory();
	concatenate_func->setDoesNotThrow();
	//concatenate_func->addAttribute(2, llvm::Attribute::StructRet);
	//concatenate_func->addAttribute(2, llvm::Attribute::NoAlias);
	//concatenate_func->addAttribute(3, llvm::Attribute::ByVal);
	//concatenate_func->addAttribute(4, llvm::Attribute::ByVal);

	llvm::Function* print_func = llvm::Function::Create(
		llvm::FunctionType::get(void_type, {valutePtr_type}, false),
		llvm::Function::ExternalLinkage, "print", module.get());
	print_func->setOnlyAccessesArgMemory();
	print_func->setDoesNotThrow();


	llvm::FunctionType* native_func_type =  llvm::FunctionType::get(value_type,{int32_type, valutePtr_type}, false);
	llvm::Function* callNative_func = llvm::Function::Create(
		llvm::FunctionType::get(void_type, {llvm::PointerType::get(native_func_type, 0), int32_type, valutePtr_type, valutePtr_type}, false),
		llvm::Function::ExternalLinkage, "callNative", module.get());
	callNative_func->setOnlyAccessesArgMemory();
	callNative_func->setDoesNotThrow();


	llvm::Function* falsey_func = generate_falsey(module.get(), value_type, valutePtr_type);
	llvm::Function* equal_func = generate_equal(module.get(), value_type, valutePtr_type);
	//llvm::Function* jit_func = generade_code(module.get(), &m_frame->function->chunk, "_jit_func", value_type, valutePtr_type);
	std::vector<llvm::Function*> functions;
	compileFunctions(module.get(), &m_frame->function->chunk, "_jit_func", llvm::Function::InternalLinkage, value_type, valutePtr_type, functions);
	static int m_ = 0;
	std::string main_name = "_main" + std::to_string(m_++);
	llvm::Function* main_func = generate_main(module.get(), main_name, value_type, valutePtr_type);

	if (verbose) {
		const char* pre_opt_file = "llvmjit-pre-opt.ll";
		llvm_module_to_file(*module, pre_opt_file);
		std::cout << "[Pre optimization module] dumped to " << pre_opt_file << "\n";
	}

	if (llvm::verifyFunction(*main_func, &llvm::errs()))
		DIE << "Error verifying function.";
	for(auto func: functions)
	{
		if (llvm::verifyFunction(*func, &llvm::errs()))
			DIE << "Error verifying function.";
	}
	if (llvm::verifyFunction(*falsey_func, &llvm::errs()))
		DIE << "Error verifying function.";
	if (llvm::verifyFunction(*equal_func, &llvm::errs()))
		DIE << "Error verifying function.";

	// Optimize the emitted LLVM IR.
	Timer topt;

	optimizeModule(&m_jit->get_target_machine(), module.get(), 3, 0);

	if (verbose) {
		std::cout << "[Optimization elapsed:] " << topt.elapsed() << "s\n";
		const char* post_opt_file = "llvmjit-post-opt.ll";
		llvm_module_to_file(*module, post_opt_file);
		std::cout << "[Post optimization module] dumped to " << post_opt_file
		          << "\n";
	}

	// JIT the optimized LLVM IR to native code and execute it.
	m_jit->add_module(std::move(module));

	llvm::JITSymbol main_func_sym = m_jit->find_symbol(main_name);
	if (!main_func_sym) {
		DIE << "Unable to find symbol " << "_main_func" << " in module";
	}

	using MainFuncType = int32_t (*)(void*, Value*,  Value*);
	MainFuncType main_func_ptr =
			reinterpret_cast<MainFuncType>(main_func_sym.getAddress().get());

	setCompiledFunctions(&m_frame->function->chunk, m_jit.get());

	auto vm_ = this;
	auto globals = m_globalValues.data();
	auto stack = &m_stack.get(0);

	auto result = main_func_ptr(vm_, globals, stack);

	return static_cast<InterpretResult>(result);
}

InterpretResult VM::run()
{
	m_frame = &m_frames[m_frameCount - 1];

#ifdef DEBUG_TRACE_EXECUTION
#define DEBUG_TRACE() debugTrace()
#else
#define DEBUG_TRACE()
#endif

#ifdef COMPUTED_GOTO

#define INTERPRET_LOOP BREAK;
#define CASE(name) OP_##name
#define BREAK DEBUG_TRACE(); goto *dispatchTable[instruction=readByte()]

	static void* dispatchTable[] = {
#define OPCODE(name) &&OP_##name,
#include "op_codes.hpp"
#undef OPCODE
	};

#else
#define INTERPRET_LOOP                          \
	using namespace OpCode;                     \
	loop:                                       \
		DEBUG_TRACE()                           \
		switch(instruction = readByte())

#define CASE(name) case name
#define BREAK goto loop
#endif

	uint8_t instruction;
	INTERPRET_LOOP
	{

		CASE(CONSTANT):
		{
			auto constant = readConstant();
			m_stack.push(constant);
			BREAK;
		}
		CASE(CONSTANT_LONG):
		{
			auto constant = readConstantLong();
			m_stack.push(constant);
			BREAK;
		}
		CASE(NIL):
		{
			m_stack.push(Value::Nil());
			BREAK;
		}
		CASE(TRUE):
		{
			m_stack.push(Value::Bool(true));
			BREAK;
		}
		CASE(FALSE):
		{
			m_stack.push(Value::Bool(false));
			BREAK;
		}
		CASE(POP):
		{
			m_stack.pop();
			BREAK;
		}
		CASE(DUP):
		{
			m_stack.push(m_stack.top());
			BREAK;
		}
		CASE(GET_LOCAL):
		{
			auto slot = readByte();
			m_stack.push(m_frame->slots[slot]);
			BREAK;
		}
		CASE(GET_LOCAL_SHORT):
		{
			auto slot = readShort();
			m_stack.push(m_frame->slots[slot]);
			BREAK;
		}
		CASE(SET_LOCAL):
		{
			auto slot = readByte();
			m_frame->slots[slot] = m_stack.top();
			BREAK;
		}
		CASE(SET_LOCAL_SHORT):
		{
			auto slot = readShort();
			m_frame->slots[slot] = m_stack.top();
			BREAK;
		}
		CASE(GET_GLOBAL):
		{
			using namespace std::string_literals;
			auto index = readByte();
			auto value = m_globalValues[index];
			if (value.isUndefined())
			{
				runtimeError("Undefined variable "s, m_globalNames[index], "."s);
				return InterpretResult::RUNTIME_ERROR;
			}
			m_stack.push(value);
			BREAK;
		}
		CASE(GET_GLOBAL_LONG):
		{
			using namespace std::string_literals;
			auto index = readLong();
			auto value = m_globalValues[index];
			if (value.isUndefined())
			{
				runtimeError("Undefined variable "s, m_globalNames[index], "."s);
				return InterpretResult::RUNTIME_ERROR;
			}
			m_stack.push(value);
			BREAK;
		}
		CASE(DEFINE_GLOBAL):
		{
			auto index = readByte();
			m_globalValues[index] = m_stack.pop();
			BREAK;
		}
		CASE(DEFINE_GLOBAL_LONG):
		{
			auto index = readLong();
			m_globalValues[index] = m_stack.pop();
			BREAK;
		}
		CASE(SET_GLOBAL):
		{
			using namespace std::string_literals;
			auto index = readByte();
			if (m_globalValues[index].isUndefined())
			{
				runtimeError("Undefined variable "s, m_globalNames[index], "."s);
				return InterpretResult::RUNTIME_ERROR;
			}
			m_globalValues[index] = m_stack.top();
			BREAK;
		}
		CASE(SET_GLOBAL_LONG):
		{
			using namespace std::string_literals;
			auto index = readLong();
			if (m_globalValues[index].isUndefined())
			{
				runtimeError("Undefined variable "s, m_globalNames[index], "."s);
				return InterpretResult::RUNTIME_ERROR;
			}
			m_globalValues[index] = m_stack.top();
			BREAK;
		}
		CASE(EQUAL):
		{
			auto b = m_stack.pop();
			auto a = m_stack.top();
			m_stack.top() = Value::Bool(a == b);
			BREAK;
		}
		CASE(GREATER):
		{
			auto status = binaryOp(Value::Bool, std::greater<>());
			if (status != InterpretResult::OK)
				return status;
			BREAK;
		}
		CASE(LESS):
		{
			auto status = binaryOp(Value::Bool, std::less<>());
			if (status != InterpretResult::OK)
				return status;
			BREAK;
		}
		CASE(ADD):
		{
			if(m_stack.top().isNumber() && m_stack.peek(1).isNumber())
			{
				auto b = m_stack.pop().asNumber();
				auto a = m_stack.top().asNumber();
				m_stack.top() = Value::Number(a + b);
			}
			else if (m_stack.top().isObjString() && m_stack.peek(1).isObjString())
			{
				concatenate<std::string, std::string>();
			}
			else if(m_stack.top().isObjString() && m_stack.peek(1).isNumber())
			{
				concatenate<double, std::string>();
			}
			else if(m_stack.top().isNumber() && m_stack.peek(1).isObjString())
			{
				concatenate<std::string, double>();
			}
			else
			{
				runtimeError("Operands must be numbers or strings.");
				return InterpretResult::RUNTIME_ERROR;
			}
			BREAK;
		}
		CASE(SUBTRACT):
		{
			auto status = binaryOp(Value::Number, std::minus<>());
			if (status != InterpretResult::OK)
				return status;
			BREAK;
		}
		CASE(MULTIPLY):
		{
			auto status = binaryOp(Value::Number, std::multiplies<>());
			if (status != InterpretResult::OK)
				return status;
			BREAK;
		}
		CASE(DIVIDE):
		{
			auto status = binaryOp(Value::Number, std::divides<>());
			if (status != InterpretResult::OK)
				return status;
			BREAK;
		}
		CASE(MODULO):
		{
			auto b = m_stack.pop();
			auto a = m_stack.top();
			if (!a.isNumber() || !b.isNumber())
			{
				runtimeError("Operands must be numbers.");
				return InterpretResult::RUNTIME_ERROR;
			}
			m_stack.top() = Value::Number(std::fmod(a.asNumber(), b.asNumber()));
			BREAK;
		}
		CASE(NOT):
		{
			m_stack.top() = Value::Bool(isFalsey(m_stack.top()));
			BREAK;
		}
		CASE(NEGATE):
		{
			if (!m_stack.top().isNumber())
			{
				runtimeError("Operand must be a number.");
				return InterpretResult::RUNTIME_ERROR;
			}
			m_stack.top() = Value::Number(-m_stack.top().asNumber());
			BREAK;
		}
		CASE(PRINT):
		{
			std::cout << m_stack.pop() << std::endl;
			BREAK;
		}
		CASE(JUMP):
		{
			auto offset = readShort();
			m_frame->ip += offset;
			BREAK;
		}
		CASE(JUMP_IF_FALSE):
		{
			auto offset = readShort();
			m_frame->ip += isFalsey(m_stack.top()) * offset;
			BREAK;
		}
		CASE(JUMP_IF_TRUE):
		{
			auto offset = readShort();
			m_frame->ip += !isFalsey(m_stack.top()) * offset;
			BREAK;
		}
		CASE(JUMP_BACK):
		{
			auto offset = readShort();
			m_frame->ip -= offset;
			BREAK;
		}
		CASE(CALL):
		{
			auto argCount = readByte();
			if (!callValue(m_stack.peek(argCount), argCount))
			{
				return InterpretResult::RUNTIME_ERROR;
			}
			m_frame = &m_frames[m_frameCount - 1];
			BREAK;
		}
		CASE(RETURN):
		{
			Value result = m_stack.pop();

			m_frameCount--;
			if (m_frameCount <= 0)
				return InterpretResult::OK;


			m_stack.getTop() = m_frame->slots;
			m_stack.push(result);

			m_frame = &m_frames[m_frameCount - 1];
			BREAK;
		}

	}

	return InterpretResult::RUNTIME_ERROR;

#undef BREAK
#undef CASE
#undef INTERPRET_LOOP
#undef DEBUG_TRACE
}
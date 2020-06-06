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
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include "llvm/Transforms/IPO.h"

#include "llvm_jit_utils.hpp"
#include "utils.hpp"

#include "memory.hpp"
#include "common.hpp"
#include "vm.hpp"
#include "nativeFunctions.hpp"
#include "jit.hpp"

VM::VM()
{
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

	auto result = run();//runJitted();

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
	auto newIdex = m_globalValues.size() - 1;
	m_globals[std::string(name)] = Value::Number(newIdex);

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

InterpretResult VM::runJitted()
{
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

	llvm::StructType* value_type = llvm::StructType::create(context, {uint8_type, double_type}, "Value");
	llvm::PointerType* valutePtr_type = llvm::PointerType::get(value_type, 0);

	llvm::Function* falsey_func = generate_falsey(module.get(), value_type, valutePtr_type);
	llvm::Function* equal_func = generate_equal(module.get(), value_type, valutePtr_type);
	//llvm::Function* jit_func = generade_code(module.get(), this->m_chunk, value_type, valutePtr_type);
	llvm::Function* jit_func = generade_code(module.get(), &m_frame->function->chunk, value_type, valutePtr_type);

	if (verbose) {
		const char* pre_opt_file = "llvmjit-pre-opt.ll";
		llvm_module_to_file(*module, pre_opt_file);
		std::cout << "[Pre optimization module] dumped to " << pre_opt_file << "\n";
	}

	if (llvm::verifyFunction(*jit_func, &llvm::errs()))
		DIE << "Error verifying function.";
	if (llvm::verifyFunction(*falsey_func, &llvm::errs()))
		DIE << "Error verifying function.";
	if (llvm::verifyFunction(*equal_func, &llvm::errs()))
		DIE << "Error verifying function.";

	// Optimize the emitted LLVM IR.
	Timer topt;
	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmPrinter();

	llvm::PassManagerBuilder pm_builder;
	pm_builder.OptLevel = 3;
	pm_builder.SizeLevel = 0;
	pm_builder.LoopVectorize = true;
	pm_builder.SLPVectorize = true;
	pm_builder.PerformThinLTO = true;
	pm_builder.Inliner = llvm::createFunctionInliningPass(3, 0, true);

	llvm::legacy::FunctionPassManager function_pm(module.get());
	llvm::legacy::PassManager module_pm;

	pm_builder.populateFunctionPassManager(function_pm);
	pm_builder.populateModulePassManager(module_pm);

	function_pm.doInitialization();
	function_pm.run(*equal_func);
	function_pm.run(*falsey_func);
	function_pm.run(*jit_func);
	module_pm.run(*module);

	function_pm.doInitialization();
	function_pm.run(*jit_func);
	module_pm.run(*module);

	if (verbose) {
		std::cout << "[Optimization elapsed:] " << topt.elapsed() << "s\n";
		const char* post_opt_file = "llvmjit-post-opt.ll";
		llvm_module_to_file(*module, post_opt_file);
		std::cout << "[Post optimization module] dumped to " << post_opt_file
		          << "\n";
	}

	// JIT the optimized LLVM IR to native code and execute it.
	SimpleOrcJIT jit(/*verbose=*/true);
	module->setDataLayout(jit.get_target_machine().createDataLayout());
	jit.add_module(std::move(module));

	llvm::JITSymbol jit_func_sym = jit.find_symbol("_jit_func");
	if (!jit_func_sym) {
		DIE << "Unable to find symbol " << "_jit_func" << " in module";
	}

	using JitFuncType = int32_t (*)(void*, Value*, Value*, Value*);
	JitFuncType jit_func_ptr =
			reinterpret_cast<JitFuncType>(jit_func_sym.getAddress().get());

	auto vm_ = this;
	auto stack = &m_stack.get(0);
	auto constants = m_frame->function->chunk.constants();//m_chunk->constants();
	auto globals = m_globalValues.data();

	auto result = jit_func_ptr(vm_, stack, constants, globals);

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
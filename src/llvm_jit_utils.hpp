// Helper utilities for launching LLVM-based JIts.
//
// Eli Bendersky [http://eli.thegreenplace.net]
// This code is in the public domain.
#ifndef LLVM_JIT_UTILS_H
#define LLVM_JIT_UTILS_H

#include <iostream>
#include <vector>

#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/LambdaResolver.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Mangler.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils.h>
//#include <llvm/Transforms/>

#include "utils.hpp"

void optimizeModule(llvm::TargetMachine *machine, llvm::Module *module, uint32_t opt, uint32_t size);

static inline void OptPass(const llvm::PassManagerBuilder &builder, llvm::legacy::PassManagerBase &pm) {
	//if (builder.OptLevel > 2 && builder.SizeLevel == 0)
	{
		pm.add(llvm::createPromoteMemoryToRegisterPass());
	}
}

static inline void addMyLoopPass(const llvm::PassManagerBuilder &builder, llvm::legacy::PassManagerBase &pm) {
	//if (builder.OptLevel > 2 && builder.SizeLevel == 0)
	{
		pm.add(llvm::createIndVarSimplifyPass());
		pm.add(llvm::createLoopInstSimplifyPass());
		pm.add(llvm::createLoopSimplifyCFGPass());
		pm.add(llvm::createLoopInterchangePass());
		pm.add(llvm::createLoopRotatePass());
		pm.add(llvm::createSimpleLoopUnrollPass(builder.OptLevel));
	}
}

static inline void addStrengthReductionPass(const llvm::PassManagerBuilder &builder, llvm::legacy::PassManagerBase &pm) {
	//if (builder.OptLevel > 2 && builder.SizeLevel == 0)
	{
		pm.add(llvm::createLoopStrengthReducePass());
		pm.add(llvm::createLoopDeletionPass());
	}
}


// ObjectDumpingCompiler is a copycat of Orc JIT's SimpleCompiler, with added
// dumping of the generated object file so we can inspect the final machine code
// produced by LLVM. Note that no IR-level optimizations are performed here.
// Dumping happens only in verbose mode (when verbose=true) in the constructor.
class ObjectDumpingCompiler {
	using CompileResult = llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>;
public:
	ObjectDumpingCompiler(llvm::TargetMachine& target_machine, bool verbose)
			: target_machine_(target_machine), verbose_(verbose) {}

	CompileResult operator()(llvm::Module& module) const {
		llvm::SmallVector<char, 0> obj_buffer_vec;
		llvm::raw_svector_ostream obj_stream(obj_buffer_vec);

		llvm::legacy::PassManager pass_manager;
		llvm::MCContext* context;
		if (target_machine_.addPassesToEmitMC(pass_manager, context, obj_stream)) {
			DIE << "Target does not support MC emission";
		}
		pass_manager.run(module);
		auto obj_buffer = std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(obj_buffer_vec));

		llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> obj =
				llvm::object::ObjectFile::createObjectFile(obj_buffer->getMemBufferRef());

		// Here we dump the emitted object to a raw binary file. This object is not
		// loaded (relocated) yet, so instructions like calls will have
		// placeholders.
		//
		// LLVM represents the object in memory as an ELF image. To dump the code,
		// we iterate to find the text section and emit its contents.
		if (verbose_) {
			bool found_text_section = false;
			for (auto& section : (*obj)->sections()) {
				if (section.isText()) {
					if (found_text_section) {
						auto section_name = section.getName();
						DIE << "Found text section already; also found "
						    << section_name->str();
					}
					found_text_section = true;

					auto sr = section.getContents();
					if (sr) {
						const char* filename = "llvmjit-out.bin";
						FILE* outfile = fopen(filename, "wb");
						if (outfile) {
							size_t n = sr->size();
							if (fwrite(sr->data(), 1, n, outfile) == n) {
								std::cout << "[*] emitted code to " << filename << "\n";
							}
							fclose(outfile);
						}
					}
				}
			}
		}

		if (obj) {
			return std::move(obj_buffer);
		}

		return obj.takeError();
	}

private:
	llvm::TargetMachine& target_machine_;
	bool verbose_;
};

// A type encapsulating simple Orc JIT functionality. Loosely based on the
// KaleidoscopeJIT example in the LLVM tree. Doesn't support cross-module
// symbol resolution; this JIT is best used with just a single module.
class SimpleOrcJIT {
public:
  // Initialize the JIT. In verbose mode extra information will be dumped. The
  // JIT is created with a default target machine.
  SimpleOrcJIT(bool verbose);

  // Get access to the target machine used by the JIT.
  llvm::TargetMachine& get_target_machine() {
    return *target_machine_;
  }

  // Add an LLVM module to the JIT. The JIT takes ownership.
  void add_module(std::unique_ptr<llvm::Module> module);

  // Find a symbol in JITed code. name is plain, unmangled. SimpleOrcJIT will
  // mangle it internally.
  llvm::JITSymbol find_symbol(const std::string& name);

private:
  // This sample doesn't implement on-request or lazy compilation. It therefore
  // uses Orc's eager compilation layer directly - IRCompileLayer. It also uses
  // the basis object layer - ObjectLinkingLayer - directly.
  using ObjLayerT = llvm::orc::LegacyRTDyldObjectLinkingLayer;
  using CompileLayerT = llvm::orc::LegacyIRCompileLayer<ObjLayerT, ObjectDumpingCompiler>;


  // Helper method to look for symbols that already have mangled names.
  llvm::JITSymbol find_mangled_symbol(const std::string& name, bool exported_symbols_only = true);

  bool verbose_;

  std::unique_ptr<llvm::TargetMachine> target_machine_;
  const llvm::DataLayout data_layout_;
  llvm::orc::ExecutionSession execution_session_;
  std::shared_ptr<llvm::orc::SymbolResolver> symbol_resolver_;
  ObjLayerT object_layer_;
  CompileLayerT compile_layer_;
  std::vector<llvm::orc::VModuleKey> module_keys_;
};

#endif /* LLVM_JIT_UTILS_H */

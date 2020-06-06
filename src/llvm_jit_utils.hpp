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
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Mangler.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/DynamicLibrary.h>

#include "utils.hpp"

// ObjectDumpingCompiler is a copycat of Orc JIT's SimpleCompiler, with added
// dumping of the generated object file so we can inspect the final machine code
// produced by LLVM. Note that no IR-level optimizations are performed here.
// Dumping happens only in verbose mode (when verbose=true) in the constructor.
class ObjectDumpingCompiler {
public:
	ObjectDumpingCompiler(llvm::TargetMachine& target_machine, bool verbose)
			: target_machine_(target_machine), verbose_(verbose) {}

	llvm::object::OwningBinary<llvm::object::ObjectFile> operator()(llvm::Module& module) const {
		llvm::SmallVector<char, 0> obj_buffer_vec;
		llvm::raw_svector_ostream obj_stream(obj_buffer_vec);

		llvm::legacy::PassManager pass_manager;
		llvm::MCContext* context;
		if (target_machine_.addPassesToEmitMC(pass_manager, context, obj_stream)) {
			DIE << "Target does not support MC emission";
		}
		pass_manager.run(module);
		std::unique_ptr<llvm::MemoryBuffer> obj_buffer(
				new llvm::ObjectMemoryBuffer(std::move(obj_buffer_vec)));

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
						llvm::StringRef section_name;
						section.getName(section_name);
						DIE << "Found text section already; also found "
						    << section_name.str();
					}
					found_text_section = true;

					llvm::StringRef sr;
					auto err = section.getContents(sr);
					if (!err) {
						const char* filename = "/tmp/llvmjit-out.bin";
						FILE* outfile = fopen(filename, "wb");
						if (outfile) {
							size_t n = sr.size();
							if (fwrite(sr.data(), 1, n, outfile) == n) {
								std::cout << "[*] emitted code to " << filename << "\n";
							}
							fclose(outfile);
						}
					}
				}
			}
		}

		typedef llvm::object::OwningBinary<llvm::object::ObjectFile> owning_obj;
		if (obj) {
			return owning_obj(std::move(*obj), std::move(obj_buffer));
		}
		consumeError(obj.takeError());
		return owning_obj(nullptr, nullptr);
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
  const llvm::TargetMachine& get_target_machine() {
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
  using ObjLayerT = llvm::orc::RTDyldObjectLinkingLayer;
  using CompileLayerT = llvm::orc::IRCompileLayer<ObjLayerT, ObjectDumpingCompiler>;
  using ModuleHandleT = CompileLayerT::ModuleHandleT;

  // Helper method to look for symbols that already have mangled names.
  llvm::JITSymbol find_mangled_symbol(const std::string& name);

  bool verbose_;

  std::unique_ptr<llvm::TargetMachine> target_machine_;
  const llvm::DataLayout data_layout_;
  ObjLayerT object_layer_;
  CompileLayerT compile_layer_;
  std::vector<ModuleHandleT> module_handles_;
};

#endif /* LLVM_JIT_UTILS_H */

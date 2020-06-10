// Helper utilities for launching LLVM-based JIts.
//
// Eli Bendersky [http://eli.thegreenplace.net]
// This code is in the public domain.
#include "llvm_jit_utils.hpp"
#include "utils.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Mangler.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/InitializePasses.h>

using namespace llvm;

namespace {

	void addOptPasses(
		llvm::legacy::PassManagerBase &passes,
		llvm::legacy::FunctionPassManager &fnPasses,
		llvm::TargetMachine *machine,
		uint32_t opt,
		uint32_t size
	) {
		llvm::PassManagerBuilder builder;
		builder.OptLevel = opt;
		builder.SizeLevel = size;
		if (opt > 1)
			builder.Inliner = llvm::createFunctionInliningPass(opt, size, false);
		else
			builder.Inliner = llvm::createAlwaysInlinerLegacyPass();
		builder.LoopVectorize = opt > 1 && size < 2;
		builder.SLPVectorize = opt > 1 && size < 2;
		machine->adjustPassManager(builder);

		builder.populateFunctionPassManager(fnPasses);
		fnPasses.add(llvm::createInstructionCombiningPass());
		fnPasses.add(llvm::createReassociatePass());
		fnPasses.add(llvm::createGVNPass());
		fnPasses.add(llvm::createCFGSimplificationPass());
		builder.populateModulePassManager(passes);
		passes.add(llvm::createIPConstantPropagationPass());
	}

	void addLinkPasses(llvm::legacy::PassManagerBase &passes, uint32_t opt, uint32_t size) {
		llvm::PassManagerBuilder builder;
		builder.OptLevel = opt;
		builder.SizeLevel = size;
		builder.VerifyInput = true;
		builder.Inliner = llvm::createFunctionInliningPass();
		builder.populateLTOPassManager(passes);
	}

	void initializePasses() {
		// Initialize passes
		PassRegistry &Registry = *PassRegistry::getPassRegistry();
		initializeCore(Registry);
		//initializeCoroutines(Registry);
		initializeScalarOpts(Registry);
		//initializeObjCARCOpts(Registry);
		initializeVectorization(Registry);
		initializeIPO(Registry);
		initializeAnalysis(Registry);
		initializeTransformUtils(Registry);
		initializeInstCombine(Registry);
		initializeAggressiveInstCombine(Registry);
		initializeInstrumentation(Registry);
		initializeTarget(Registry);
		// For codegen passes, only passes that do IR to IR transformation are
		// supported.
		initializeExpandMemCmpPassPass(Registry);
		initializeScalarizeMaskedMemIntrinPass(Registry);
		initializeCodeGenPreparePass(Registry);
		initializeAtomicExpandPass(Registry);
		initializeRewriteSymbolsLegacyPassPass(Registry);
		initializeWinEHPreparePass(Registry);
		initializeDwarfEHPreparePass(Registry);
		initializeSafeStackLegacyPassPass(Registry);
		initializeSjLjEHPreparePass(Registry);
		initializeStackProtectorPass(Registry);
		initializePreISelIntrinsicLoweringLegacyPassPass(Registry);
		initializeGlobalMergePass(Registry);
		initializeIndirectBrExpandPassPass(Registry);
		//initializeInterleavedLoadCombinePass(Registry);
		initializeInterleavedAccessPass(Registry);
		initializeEntryExitInstrumenterPass(Registry);
		initializePostInlineEntryExitInstrumenterPass(Registry);
		initializeUnreachableBlockElimLegacyPassPass(Registry);
		initializeExpandReductionsPass(Registry);
		initializeWasmEHPreparePass(Registry);
		initializeWriteBitcodePassPass(Registry);
		//initializeHardwareLoopsPass(Registry);
	}

}

void optimizeModule(llvm::TargetMachine *machine, llvm::Module *module, uint32_t opt, uint32_t size) {
	initializePasses();
	module->setTargetTriple(machine->getTargetTriple().str());
	module->setDataLayout(machine->createDataLayout());

	llvm::legacy::PassManager passes;
	passes.add(new llvm::TargetLibraryInfoWrapperPass(machine->getTargetTriple()));
	passes.add(llvm::createTargetTransformInfoWrapperPass(machine->getTargetIRAnalysis()));

	llvm::legacy::FunctionPassManager fnPasses(module);
	fnPasses.add(llvm::createTargetTransformInfoWrapperPass(machine->getTargetIRAnalysis()));

	//if (opt > 0)
		addLinkPasses(passes, opt, size);
	addOptPasses(passes, fnPasses, machine, opt, size);

	fnPasses.doInitialization();
	for (llvm::Function &func : *module) {
		fnPasses.run(func);
	}
	fnPasses.doFinalization();

	passes.add(llvm::createVerifierPass());
	passes.run(*module);
}

// namespace {
llvm::SmallVector<std::string, 0> DetectMachineAttributes() {
	llvm::SmallVector<std::string, 0> result;
	llvm::StringMap<bool> host_features;
	if (llvm::sys::getHostCPUFeatures(host_features)) {
		for (auto& feature : host_features) {
			if (feature.second) {
				llvm::StringRef feature_name = feature.first();
				// Skip avx512 for now, it isn't quite ready in LLVM.
				if (feature_name.startswith("avx512")) {
					continue;
				}
				result.push_back(feature_name);
			}
		}
	}
	return result;
}

SimpleOrcJIT::SimpleOrcJIT(bool verbose)
    : verbose_(verbose), target_machine_(EngineBuilder().selectTarget()),
      data_layout_(target_machine_->createDataLayout()),

      object_layer_(execution_session_, [this](llvm::orc::VModuleKey) {
		  llvm::orc::LegacyRTDyldObjectLinkingLayer::Resources result;
		  result.MemMgr = std::make_shared<llvm::SectionMemoryManager>();
		  result.Resolver = this->symbol_resolver_;
		  return result;
	  }),
      compile_layer_(object_layer_,
                     ObjectDumpingCompiler(*target_machine_, verbose_)) {
  std::string error_string;
  if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr,
                                                        &error_string)) {
    DIE << "Failed to LoadLibraryPermanently: " << error_string;
  }

  if (verbose_) {
    Triple triple = target_machine_->getTargetTriple();
    std::cout << "JIT target machine:\n";
    std::cout << "  triple: " << triple.str() << "\n";
    std::cout << "  target cpu: " << target_machine_->getTargetCPU().str()
              << "\n";
    std::cout << "  target features: "
              << target_machine_->getTargetFeatureString().str() << "\n";
  }
}

void SimpleOrcJIT::add_module(std::unique_ptr<llvm::Module> module) {
	auto key = execution_session_.allocateVModule();

	symbol_resolver_ = orc::createLegacyLookupResolver(execution_session_,
		[this](const std::string& name) -> JITSymbol {
			if (auto sym = find_mangled_symbol(name, false))
				return sym;
			else if (auto Err = sym.takeError())
				return std::move(Err);
			if (auto sym_addr =
				RTDyldMemoryManager::getSymbolAddressInProcess(name)) {
				return JITSymbol(sym_addr, JITSymbolFlags::Exported);
			}
			return JITSymbol(nullptr);
		}, [](llvm::Error Err) {
			cantFail(std::move(Err), "lookupFlags failed");
		});

	cantFail(compile_layer_.addModule(key, std::move(module)));

	module_keys_.push_back(key);
}


llvm::JITSymbol SimpleOrcJIT::find_symbol(const std::string& name) {
	std::string mangled_name;
	raw_string_ostream mangled_name_stream(mangled_name);
	Mangler::getNameWithPrefix(mangled_name_stream, name, data_layout_);
	return compile_layer_.findSymbol(name, true);
}

llvm::JITSymbol SimpleOrcJIT::find_mangled_symbol(const std::string& name, bool exported_symbols_only) {

  // Search modules in reverse order: from last added to first added.
  for (auto &h : make_range(module_keys_.rbegin(), module_keys_.rend())) {
    if (auto sym =
            compile_layer_.findSymbolIn(h, name, exported_symbols_only)) {
      return sym;
    }
  }
  return nullptr;
}


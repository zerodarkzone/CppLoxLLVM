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
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"


using namespace llvm;
// namespace {

SimpleOrcJIT::SimpleOrcJIT(bool verbose)
    : verbose_(verbose), target_machine_(EngineBuilder().selectTarget()),
      data_layout_(target_machine_->createDataLayout()),
      object_layer_([]() { return llvm::make_unique<SectionMemoryManager>(); }),
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
  // This resolver looks back into the host with dlsym to find symbols the
  // module calls but aren't defined in it.
  auto resolver = orc::createLambdaResolver(
      [this](const std::string& name) {
        if (auto sym = find_mangled_symbol(name)) {
          return sym;
        }
        return JITSymbol(nullptr);
      },
      [](const std::string& name) {
        if (auto sym_addr =
                RTDyldMemoryManager::getSymbolAddressInProcess(name)) {
          return JITSymbol(sym_addr, JITSymbolFlags::Exported);
        }
        return JITSymbol(nullptr);
      });
  auto handle = compile_layer_.addModule(std::move(module),
                                            std::move(resolver));

  module_handles_.push_back(handle.get());
}

llvm::JITSymbol SimpleOrcJIT::find_symbol(const std::string& name) {
  std::string mangled_name;
  {
    raw_string_ostream mangled_name_stream(mangled_name);
    Mangler::getNameWithPrefix(mangled_name_stream, name, data_layout_);
  }

  return find_mangled_symbol(mangled_name);
}

llvm::JITSymbol SimpleOrcJIT::find_mangled_symbol(const std::string& name) {
  const bool exported_symbols_only = true;

  // Search modules in reverse order: from last added to first added.
  for (auto &h : make_range(module_handles_.rbegin(), module_handles_.rend())) {
    if (auto sym =
            compile_layer_.findSymbolIn(h, name, exported_symbols_only)) {
      return sym;
    }
  }
  return nullptr;
}

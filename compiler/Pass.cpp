// This file is part of SymCC.
//
// SymCC is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// SymCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// SymCC. If not, see <https://www.gnu.org/licenses/>.

#include "Pass.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "Runtime.h"
#include "Symbolizer.h"

using namespace llvm;

#ifndef NDEBUG
#define DEBUG(X)                                                               \
  do {                                                                         \
    X;                                                                         \
  } while (false)
#else
#define DEBUG(X) ((void)0)
#endif

char SymbolizePass::ID = 0;

bool SymbolizePass::doInitialization(Module &M) {
  DEBUG(errs() << "Symbolizer module init\n");

  // Redirect calls to external functions to the corresponding wrappers and
  // rename internal functions.
  for (auto &function : M.functions()) {
    auto name = function.getName();
    if (isInterceptedFunction(function))
      function.setName(name + "_symbolized");
  }

  // Insert a constructor that initializes the runtime and any globals.
  Function *ctor;
  std::tie(ctor, std::ignore) = createSanitizerCtorAndInitFunctions(
      M, kSymCtorName, "_sym_initialize", {}, {});
  appendToGlobalCtors(M, ctor, 0);

  // Initialize id for each basic block
  unsigned int randSeed;
  char *seedStr = getenv("AFL_RAND_SEED");

  if (seedStr && sscanf(seedStr, "%u", &randSeed))
    srand(randSeed);
  
  for (auto &F : M)
    for (auto &BB : F) {
      unsigned int curLoc = (random() % AFL_MAP_SIZE);
      basicBlockMap.insert(std::pair<BasicBlock *, unsigned int>(&BB, curLoc));
    }

  // TODO: delete the log info
  for (auto &F : M)
    for (auto &BB : F) {
      if (basicBlockMap.find(&BB) == basicBlockMap.end())
        continue;
      // Start BB id
      unsigned int prevLoc = basicBlockMap[&BB];
      unsigned int curLoc, edgeId;

      for (BasicBlock *succBB : successors(&BB)) {
        if (basicBlockMap.find(succBB) == basicBlockMap.end())
          continue;
        // End BB id
        curLoc = basicBlockMap[succBB];
        edgeId = prevLoc >> 1 ^ curLoc;

        errs() << edgeId << ":" << prevLoc << "," << curLoc << "\n";
      }
    }

  return true;
}

bool SymbolizePass::runOnFunction(Function &F) {
  auto functionName = F.getName();
  if (functionName == kSymCtorName)
    return false;

  DEBUG(errs() << "Symbolizing function ");
  DEBUG(errs().write_escaped(functionName) << '\n');

  SmallVector<Instruction *, 0> allInstructions;
  allInstructions.reserve(F.getInstructionCount());
  for (auto &I : instructions(F))
    allInstructions.push_back(&I);

  Symbolizer symbolizer(*F.getParent());
  symbolizer.symbolizeFunctionArguments(F);

  for (auto &basicBlock : F)
    symbolizer.insertBasicBlockNotification(basicBlock);

  for (auto *instPtr : allInstructions)
    symbolizer.visit(instPtr);

  symbolizer.finalizePHINodes();
  symbolizer.shortCircuitExpressionUses();

  // DEBUG(errs() << F << '\n');
  assert(!verifyFunction(F, &errs()) &&
         "SymbolizePass produced invalid bitcode");

  return true;
}

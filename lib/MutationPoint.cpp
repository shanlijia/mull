#include "mull/MutationPoint.h"

#include "mull/BitcodeLoader.h"
#include "mull/Mutators/Mutator.h"
#include "mull/Reporters/SourceCodeReader.h"
#include "mull/Reporters/SourceManager.h"
#include "mull/Toolchain/Compiler.h"

#include <llvm/IR/Function.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cassert>
#include <sstream>
#include <utility>

using namespace llvm;
using namespace mull;
using namespace std;

#pragma mark - MutationPointAddress

Instruction &MutationPointAddress::findInstruction(Module *module) const {
  llvm::Function &function = *(std::next(module->begin(), getFnIndex()));
  llvm::BasicBlock &bb = *(std::next(function.begin(), getBBIndex()));
  llvm::Instruction &instruction = *(std::next(bb.begin(), getIIndex()));

  return instruction;
}

llvm::Instruction &MutationPointAddress::findInstruction(llvm::Function *function) const {
  llvm::BasicBlock &bb = *(std::next(function->begin(), getBBIndex()));
  llvm::Instruction &instruction = *(std::next(bb.begin(), getIIndex()));

  return instruction;
}

MutationPointAddress::MutationPointAddress(int FnIndex, int BBIndex, int IIndex)
    : functionIndex(FnIndex), basicBlockIndex(BBIndex), instructionIndex(IIndex),
      identifier(std::to_string(functionIndex) + "_" + std::to_string(basicBlockIndex) + "_" +
                 std::to_string(instructionIndex)) {}

int MutationPointAddress::getFnIndex() const {
  return functionIndex;
}

int MutationPointAddress::getBBIndex() const {
  return basicBlockIndex;
}

int MutationPointAddress::getIIndex() const {
  return instructionIndex;
}

template <typename Container, typename Value>
static size_t getIndex(Container &container, Value *value) {
  size_t index = 0;
  auto begin = container.begin();
  auto end = container.end();
  for (; begin != end; begin++, index++) {
    if (&*begin == value) {
      break;
    }
  }
  return index;
}

MutationPointAddress
MutationPointAddress::addressFromInstruction(const llvm::Instruction *instruction) {
  return MutationPointAddress(
      getIndex(instruction->getModule()->getFunctionList(), instruction->getFunction()),
      getIndex(instruction->getFunction()->getBasicBlockList(), instruction->getParent()),
      getIndex(instruction->getParent()->getInstList(), instruction));
}

#pragma mark - MutationPoint

MutationPoint::MutationPoint(Mutator *mutator, irm::IRMutation *irMutator,
                             llvm::Instruction *instruction, std::string replacement, Bitcode *m,
                             std::string diagnostics)
    : mutator(mutator), address(MutationPointAddress::addressFromInstruction(instruction)),
      bitcode(m), originalFunction(instruction->getFunction()), mutatedFunction(nullptr),
      diagnostics(std::move(diagnostics)), replacement(std::move(replacement)),
      sourceLocation(SourceLocation::locationFromInstruction(instruction)), irMutator(irMutator),
      covered(true) {
  userIdentifier = mutator->getUniqueIdentifier() + ':' + sourceLocation.filePath + ':' +
                   std::to_string(sourceLocation.line) + ':' +
                   std::to_string(sourceLocation.column);
}

void MutationPoint::setCovered(bool isCovered) {
  covered = isCovered;
}

bool MutationPoint::isCovered() {
  return covered;
}

Mutator *MutationPoint::getMutator() {
  return mutator;
}

Mutator *MutationPoint::getMutator() const {
  return mutator;
}

const MutationPointAddress &MutationPoint::getAddress() const {
  return address;
}

Value *MutationPoint::getOriginalValue() const {
  auto *function = originalFunction;
  if (mutatedFunction != nullptr) {
    function = function->getParent()->getFunction(getOriginalFunctionName());
  }
  assert(function && "The original function should be present");
  return &(address.findInstruction(function));
}

Bitcode *MutationPoint::getBitcode() const {
  return bitcode;
}

void MutationPoint::applyMutation() {
  assert(mutatedFunction != nullptr);
  mutator->applyMutation(mutatedFunction, address, irMutator);
}

std::string MutationPoint::getMutatorIdentifier() const {
  return mutator->getUniqueIdentifier();
}

const std::string &MutationPoint::getDiagnostics() {
  return diagnostics;
}

const std::string &MutationPoint::getDiagnostics() const {
  return diagnostics;
}

const std::string &MutationPoint::getReplacement() {
  return replacement;
}

const SourceLocation &MutationPoint::getSourceLocation() const {
  return sourceLocation;
}

Function *MutationPoint::getOriginalFunction() {
  return originalFunction;
}

llvm::Function *MutationPoint::getMutatedFunction() const {
  return mutatedFunction;
}

void MutationPoint::setMutatedFunction(llvm::Function *function) {
  function->setName(getMutatedFunctionName());
  this->mutatedFunction = function;
}

std::string MutationPoint::getMutatedFunctionName() {
  if (this->mutatedFunction) {
    return this->mutatedFunction->getName().str();
  }
  return getUserIdentifier();
}

std::string MutationPoint::getOriginalFunctionName() const {
  return "mull_"s + originalFunction->getName().str() + "_original";
}

std::string MutationPoint::dump() const {
  std::stringstream ss;
  ss << "Mutation Point: " << getMutator()->getUniqueIdentifier() << " "
     << getSourceLocation().filePath << ":" << getSourceLocation().line << ":"
     << getSourceLocation().column;
  return ss.str();
}

std::string MutationPoint::dumpSourceCodeContext() const {
  SourceCodeReader sourceCodeReader;
  return sourceCodeReader.getContext(sourceLocation);
}

const std::string &MutationPoint::getUserIdentifier() const {
  return userIdentifier;
}

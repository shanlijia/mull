#include "mull/Driver.h"

#include "mull/Config/Configuration.h"
#include "mull/Diagnostics/Diagnostics.h"
#include "mull/Filters/Filters.h"
#include "mull/Filters/FunctionFilter.h"
#include "mull/FunctionUnderTest.h"
#include "mull/Mutant.h"
#include "mull/MutationResult.h"
#include "mull/MutationsFinder.h"
#include "mull/Parallelization/Parallelization.h"
#include "mull/Program/Program.h"
#include "mull/Result.h"
#include "mull/Toolchain/Runner.h"

#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Path.h>

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;
using namespace llvm::object;
using namespace mull;
using namespace std;

Driver::~Driver() {
  delete this->ideDiagnostics;
}

std::unique_ptr<Result> Driver::run() {
  auto mutationPoints = findMutationPoints();
  auto filteredMutations = filterMutations(std::move(mutationPoints));

  std::vector<std::unique_ptr<Mutant>> mutants;
  singleTask.execute("Deduplicate mutants", [&]() {
    std::unordered_map<std::string, std::vector<MutationPoint *>> mapping;
    for (MutationPoint *point : filteredMutations) {
      mapping[point->getUserIdentifier()].push_back(point);
    }
    for (auto &pair : mapping) {
      mutants.push_back(std::make_unique<Mutant>(pair.first, pair.second));
    }
    std::sort(std::begin(mutants), std::end(mutants), MutantComparator());
  });

  auto mutationResults = runMutations(filteredMutations, mutants);

  return std::make_unique<Result>(
      std::move(mutants), std::move(mutationResults), std::move(filteredMutations));
}

std::vector<MutationPoint *> Driver::findMutationPoints() {
  if (!config.skipSanityCheckRun) {
    Runner runner(diagnostics);
    singleTask.execute("Sanity check run", [&]() {
      ExecutionResult result =
          runner.runProgram(config.executable, {}, {}, config.timeout, config.captureTestOutput);
      if (result.status != Passed) {
        std::stringstream failureMessage;
        failureMessage << "Original test failed\n";
        failureMessage << "test: ";
        failureMessage << "main"
                       << "\n";
        failureMessage << "status: ";
        failureMessage << result.getStatusAsString() << "\n";
        failureMessage << "stdout: '";
        failureMessage << result.stdoutOutput << "'\n";
        failureMessage << "stderr: '";
        failureMessage << result.stderrOutput << "'\n";
        diagnostics.warning(failureMessage.str());
      }
    });
  }

  std::vector<FunctionUnderTest> functionsUnderTest = getFunctionsUnderTest();
  std::vector<FunctionUnderTest> filteredFunctions = filterFunctions(functionsUnderTest);

  selectInstructions(filteredFunctions);

  std::vector<MutationPoint *> mutationPoints =
      mutationsFinder.getMutationPoints(diagnostics, program, filteredFunctions);

  return mutationPoints;
}

std::vector<MutationPoint *> Driver::filterMutations(std::vector<MutationPoint *> mutationPoints) {
  std::vector<MutationPoint *> mutations = std::move(mutationPoints);

  for (auto filter : filters.mutationFilters) {
    std::vector<MutationFilterTask> tasks;
    tasks.reserve(config.parallelization.workers);
    for (int i = 0; i < config.parallelization.workers; i++) {
      tasks.emplace_back(*filter);
    }

    std::string label = std::string("Applying filter: ") + filter->name();
    std::vector<MutationPoint *> tmp;
    TaskExecutor<MutationFilterTask> filterRunner(
        diagnostics, label, mutations, tmp, std::move(tasks));
    filterRunner.execute();
    mutations = std::move(tmp);
  }

  return mutations;
}

std::vector<FunctionUnderTest> Driver::filterFunctions(std::vector<FunctionUnderTest> functions) {
  std::vector<FunctionUnderTest> filteredFunctions(std::move(functions));

  for (auto filter : filters.functionFilters) {
    std::vector<FunctionFilterTask> tasks;
    tasks.reserve(config.parallelization.workers);
    for (int i = 0; i < config.parallelization.workers; i++) {
      tasks.emplace_back(*filter);
    }

    std::string label = std::string("Applying function filter: ") + filter->name();
    std::vector<FunctionUnderTest> tmp;
    TaskExecutor<FunctionFilterTask> filterRunner(
        diagnostics, label, filteredFunctions, tmp, std::move(tasks));
    filterRunner.execute();
    filteredFunctions = std::move(tmp);
  }

  return filteredFunctions;
}

void Driver::selectInstructions(std::vector<FunctionUnderTest> &functions) {
  std::vector<InstructionSelectionTask> tasks;
  tasks.reserve(config.parallelization.workers);
  for (int i = 0; i < config.parallelization.workers; i++) {
    tasks.emplace_back(filters.instructionFilters);
  }

  std::vector<int> Nothing;
  TaskExecutor<InstructionSelectionTask> filterRunner(
      diagnostics, "Instruction selection", functions, Nothing, std::move(tasks));
  filterRunner.execute();
}

std::vector<std::unique_ptr<MutationResult>>
Driver::runMutations(std::vector<MutationPoint *> &mutationPoints,
                     std::vector<std::unique_ptr<Mutant>> &mutants) {
  if (mutationPoints.empty()) {
    return std::vector<std::unique_ptr<MutationResult>>();
  }

  if (config.dryRunEnabled) {
    return dryRunMutations(mutationPoints, mutants);
  }

  return normalRunMutations(mutationPoints, mutants);
}

#pragma mark -

std::vector<std::unique_ptr<MutationResult>>
Driver::dryRunMutations(const std::vector<MutationPoint *> &mutationPoints,
                        std::vector<std::unique_ptr<Mutant>> &mutants) {
  std::vector<std::unique_ptr<MutationResult>> mutationResults;

  std::vector<DryRunMutantExecutionTask> tasks;
  tasks.reserve(config.parallelization.workers);
  for (int i = 0; i < config.parallelization.workers; i++) {
    tasks.emplace_back(DryRunMutantExecutionTask());
  }
  TaskExecutor<DryRunMutantExecutionTask> mutantRunner(
      diagnostics, "Running mutants (dry run)", mutants, mutationResults, std::move(tasks));
  mutantRunner.execute();

  return mutationResults;
}

std::vector<std::unique_ptr<MutationResult>>
Driver::normalRunMutations(const std::vector<MutationPoint *> &mutationPoints,
                           std::vector<std::unique_ptr<Mutant>> &mutants) {
  singleTask.execute("Prepare mutations", [&]() {
    for (auto point : mutationPoints) {
      point->getBitcode()->addMutation(point);
    }
  });

  auto workers = config.parallelization.workers;
  std::vector<int> devNull;
  TaskExecutor<CloneMutatedFunctionsTask> cloneFunctions(
      diagnostics,
      "Cloning functions for mutation",
      program.bitcode(),
      devNull,
      std::vector<CloneMutatedFunctionsTask>(workers));
  cloneFunctions.execute();

  std::vector<int> Nothing;
  TaskExecutor<DeleteOriginalFunctionsTask> deleteOriginalFunctions(
      diagnostics,
      "Removing original functions",
      program.bitcode(),
      Nothing,
      std::vector<DeleteOriginalFunctionsTask>(workers));
  deleteOriginalFunctions.execute();

  TaskExecutor<InsertMutationTrampolinesTask> redirectFunctions(
      diagnostics,
      "Redirect mutated functions",
      program.bitcode(),
      Nothing,
      std::vector<InsertMutationTrampolinesTask>(workers));
  redirectFunctions.execute();

  TaskExecutor<ApplyMutationTask> applyMutations(
      diagnostics, "Applying mutations", mutationPoints, Nothing, { ApplyMutationTask() });
  applyMutations.execute();

  std::vector<OriginalCompilationTask> compilationTasks;
  compilationTasks.reserve(workers);
  for (int i = 0; i < workers; i++) {
    compilationTasks.emplace_back(toolchain);
  }
  std::vector<std::string> objectFiles;
  TaskExecutor<OriginalCompilationTask> mutantCompiler(diagnostics,
                                                       "Compiling original code",
                                                       program.bitcode(),
                                                       objectFiles,
                                                       std::move(compilationTasks));
  mutantCompiler.execute();

  std::string executable;
  singleTask.execute("Link mutated program",
                     [&]() { executable = toolchain.linker().linkObjectFiles(objectFiles); });

  Runner runner(diagnostics);
  /// On macOS, sometimes newly compiled programs take more time to execute for the first run
  /// As we take the execution time as a baseline for timeout it makes sense to have an additional
  /// warm up run so that the next runs will be a bit faster
  singleTask.execute("Warm up run", [&]() {
    runner.runProgram(executable, {}, {}, config.timeout, config.captureMutantOutput);
  });

  ExecutionResult baseline;
  singleTask.execute("Baseline run", [&]() {
    baseline = runner.runProgram(executable, {}, {}, config.timeout, config.captureMutantOutput);
  });

  std::vector<std::unique_ptr<MutationResult>> mutationResults;

  std::vector<MutantExecutionTask> tasks;
  tasks.reserve(config.parallelization.mutantExecutionWorkers);
  for (int i = 0; i < config.parallelization.mutantExecutionWorkers; i++) {
    tasks.emplace_back(config, diagnostics, executable, baseline);
  }
  TaskExecutor<MutantExecutionTask> mutantRunner(
      diagnostics, "Running mutants", mutants, mutationResults, std::move(tasks));
  mutantRunner.execute();

  return mutationResults;
}

Driver::Driver(Diagnostics &diagnostics, const Configuration &config, Program &program,
               Toolchain &t, Filters &filters, MutationsFinder &mutationsFinder)
    : config(config), program(program), toolchain(t), mutationsFinder(mutationsFinder),
      diagnostics(diagnostics), filters(filters), singleTask(diagnostics) {

  if (config.diagnostics != IDEDiagnosticsKind::None) {
    this->ideDiagnostics = new NormalIDEDiagnostics(config.diagnostics);
  } else {
    this->ideDiagnostics = new NullIDEDiagnostics();
  }
}

static std::unique_ptr<llvm::coverage::CoverageMapping>
loadCoverage(const Configuration &configuration, Diagnostics &diagnostics) {
  if (configuration.coverageInfo.empty()) {
    return nullptr;
  }
  llvm::Expected<std::unique_ptr<llvm::coverage::CoverageMapping>> maybeMapping =
      llvm::coverage::CoverageMapping::load({ configuration.executable },
                                            configuration.coverageInfo);
  if (!maybeMapping) {
    std::string error;
    llvm::raw_string_ostream os(error);
    llvm::logAllUnhandledErrors(maybeMapping.takeError(), os, "Cannot read coverage info: ");
    diagnostics.warning(os.str());
    return nullptr;
  }
  return std::move(maybeMapping.get());
}

std::vector<FunctionUnderTest> Driver::getFunctionsUnderTest() {
  std::vector<FunctionUnderTest> functionsUnderTest;

  singleTask.execute("Gathering functions under test", [&]() {
    std::unique_ptr<llvm::coverage::CoverageMapping> coverage = loadCoverage(config, diagnostics);
    if (coverage) {
      /// Some of the function records contain just name, the others are prefixed with the filename
      /// to avoid collisions
      /// TODO: check case when filename:functionName is not enough to resolve collisions
      /// TODO: pick a proper data structure
      std::unordered_map<std::string, std::unordered_set<std::string>> scopedFunctions;
      std::unordered_set<std::string> unscopedFunctions;
      for (auto &it : coverage->getCoveredFunctions()) {
        if (!it.ExecutionCount) {
          continue;
        }
        std::string scope;
        std::string name = it.Name;
        size_t idx = name.find(':');
        if (idx != std::string::npos) {
          scope = name.substr(0, idx);
          name = name.substr(idx + 1);
        }
        if (scope.empty()) {
          unscopedFunctions.insert(name);
        } else {
          scopedFunctions[scope].insert(name);
        }
      }
      for (auto &bitcode : program.bitcode()) {
        for (llvm::Function &function : *bitcode->getModule()) {
          bool covered = false;
          std::string name = function.getName().str();
          if (unscopedFunctions.count(name)) {
            covered = true;
          } else {
            std::string filepath = SourceLocation::locationFromFunction(&function).unitFilePath;
            std::string scope = llvm::sys::path::filename(filepath).str();
            if (scopedFunctions[scope].count(name)) {
              covered = true;
            }
          }
          if (covered) {
            functionsUnderTest.emplace_back(&function, bitcode.get());
          } else if (config.includeNotCovered) {
            functionsUnderTest.emplace_back(&function, bitcode.get(), false);
          }
        }
      }
    } else {
      if (config.includeNotCovered) {
        diagnostics.warning("-include-not-covered is enabled, but there is no coverage info!");
      }
      for (auto &bitcode : program.bitcode()) {
        for (llvm::Function &function : *bitcode->getModule()) {
          functionsUnderTest.emplace_back(&function, bitcode.get());
        }
      }
    }
  });

  return functionsUnderTest;
}

#include <ebc/BitcodeContainer.h>
#include <ebc/BitcodeRetriever.h>
#include <ebc/EmbeddedFile.h>

#include <llvm/Support/TargetSelect.h>

#include "CLIOptions.h"
#include "mull/AST/ASTFinder.h"
#include "mull/AST/ASTMutationFilter.h"
#include "mull/BitcodeMetadataReader.h"
#include "mull/Config/Configuration.h"
#include "mull/Config/ConfigurationOptions.h"
#include "mull/Diagnostics/Diagnostics.h"
#include "mull/Driver.h"
#include "mull/Filters/FilePathFilter.h"
#include "mull/Filters/Filter.h"
#include "mull/Filters/Filters.h"
#include "mull/Filters/JunkMutationFilter.h"
#include "mull/Filters/NoDebugInfoFilter.h"
#include "mull/JunkDetection/CXX/CXXJunkDetector.h"
#include "mull/Metrics/MetricsMeasure.h"
#include "mull/MutationsFinder.h"
#include "mull/Mutators/MutatorKind.h"
#include "mull/Parallelization/Tasks/LoadBitcodeFromBinaryTask.h"
#include "mull/Program/Program.h"
#include "mull/Reporters/ASTSourceInfoProvider.h"
#include "mull/Result.h"
#include "mull/Version.h"

#include <iterator>
#include <memory>
#include <sstream>
#include <unistd.h>

static void validateInputFile() {
  if (access(tool::InputFile.getValue().c_str(), R_OK) != 0) {
    perror(tool::InputFile.getValue().c_str());
    exit(1);
  }
}

static std::vector<std::string> splitFlags(const std::string &flags) {
  std::istringstream s{flags};
  return std::vector<std::string>(std::istream_iterator<std::string>{s}, {});
}

int main(int argc, char **argv) {
  mull::Diagnostics diagnostics;
  llvm::cl::SetVersionPrinter(mull::printVersionInformation);

  tool::MutatorsCLIOptions mutatorsOptions(diagnostics, tool::Mutators);
  tool::ReportersCLIOptions reportersOption(diagnostics, tool::ReportersOption);

  llvm::cl::HideUnrelatedOptions(tool::MullCXXCategory);
  bool validOptions = llvm::cl::ParseCommandLineOptions(argc, argv, "", &llvm::errs());
  if (!validOptions) {
    if (tool::DumpCLIInterface) {
      tool::dumpCLIInterface(diagnostics);
      return 0;
    }
    if (tool::DumpMutators) {
      tool::dumpMutators(diagnostics);
      return 0;
    }
    return 1;
  }

  if (tool::DebugEnabled) {
    diagnostics.enableDebugMode();
    diagnostics.debug("Diagnostics: Debug Mode enabled. Debug-level messages will be printed.");
  }

  if (tool::StrictModeEnabled) {
    diagnostics.enableStrictMode();
    diagnostics.info(
        "Diagnostics: Strict Mode enabled. Warning messages will be treated as fatal errors.");
  }

  validateInputFile();

  mull::MetricsMeasure totalExecutionTime;
  totalExecutionTime.start();

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  mull::Configuration configuration;
  configuration.dryRunEnabled = tool::DryRunOption.getValue();

  configuration.linker = tool::Linker.getValue();
  configuration.linkerFlags = splitFlags(tool::LinkerFlags.getValue());
  configuration.linkerTimeout = tool::LinkerTimeout.getValue();

  configuration.debugEnabled = tool::DebugEnabled;
  configuration.timeout = tool::Timeout.getValue();

  configuration.executable = tool::InputFile.getValue();
  configuration.coverageInfo = tool::CoverageInfo.getValue();
  configuration.includeNotCovered = tool::IncludeNotCovered.getValue();

  if (tool::Workers) {
    mull::ParallelizationConfig parallelizationConfig;
    parallelizationConfig.workers = tool::Workers;
    parallelizationConfig.normalize();
    configuration.parallelization = parallelizationConfig;
  } else {
    configuration.parallelization = mull::ParallelizationConfig::defaultConfig();
  }

  if (tool::NoTestOutput.getValue() || tool::NoOutput.getValue()) {
    configuration.captureTestOutput = false;
  }
  if (tool::NoMutantOutput.getValue() || tool::NoOutput.getValue()) {
    configuration.captureMutantOutput = false;
  }

  std::vector<std::unique_ptr<ebc::EmbeddedFile>> embeddedFiles;
  mull::SingleTaskExecutor extractBitcodeBuffers(diagnostics);
  extractBitcodeBuffers.execute("Extracting bitcode from executable", [&] {
    ebc::BitcodeRetriever bitcodeRetriever(tool::InputFile.getValue());
    for (auto &bitcodeInfo : bitcodeRetriever.GetBitcodeInfo()) {
      auto &container = bitcodeInfo.bitcodeContainer;
      if (container) {
        for (auto &file : container->GetRawEmbeddedFiles()) {
          embeddedFiles.push_back(std::move(file));
        }
      } else {
        diagnostics.warning(std::string("No bitcode: ") + bitcodeInfo.arch);
      }
    }
  });

  std::vector<std::unique_ptr<llvm::LLVMContext>> contexts;
  std::vector<mull::LoadBitcodeFromBinaryTask> tasks;
  for (int i = 0; i < configuration.parallelization.workers; i++) {
    auto context = std::make_unique<llvm::LLVMContext>();
    tasks.emplace_back(mull::LoadBitcodeFromBinaryTask(diagnostics, *context));
    contexts.push_back(std::move(context));
  }
  std::vector<std::unique_ptr<mull::Bitcode>> bitcode;
  mull::TaskExecutor<mull::LoadBitcodeFromBinaryTask> executor(
      diagnostics, "Loading bitcode files", embeddedFiles, bitcode, std::move(tasks));
  executor.execute();

  mull::BitcodeMetadataReader bitcodeCompilationDatabaseLoader;
  std::map<std::string, std::string> bitcodeCompilationFlags =
      bitcodeCompilationDatabaseLoader.getCompilationDatabase(bitcode);

  mull::Program program(std::move(bitcode));

  mull::Toolchain toolchain(diagnostics, configuration);

  bool bitcodeCompilationDatabaseAvailable = false;
  bool compilationDatabasePathAvailable = false;
  bool bitcodeCompilationFlagsAvailable = false;
  std::string cxxCompilationFlags;
  std::string cxxCompilationDatabasePath;
  if (!bitcodeCompilationFlags.empty()) {
    diagnostics.info(std::string("Found compilation flags in the input bitcode"));
    bitcodeCompilationDatabaseAvailable = true;
  }
  if (!tool::CompilationFlags.empty()) {
    cxxCompilationFlags = tool::CompilationFlags.getValue();
    bitcodeCompilationFlagsAvailable = true;
  }
  if (!tool::CompilationDatabasePath.empty()) {
    cxxCompilationDatabasePath = tool::CompilationDatabasePath.getValue();
    compilationDatabasePathAvailable = true;
  }
  bool compilationDatabaseInfoAvailable = bitcodeCompilationDatabaseAvailable ||
                                          compilationDatabasePathAvailable ||
                                          bitcodeCompilationFlagsAvailable;

  mull::ASTStorage astStorage(
      diagnostics, cxxCompilationDatabasePath, cxxCompilationFlags, bitcodeCompilationFlags);

  mull::ASTSourceInfoProvider sourceInfoProvider(astStorage);
  tool::ReporterParameters params{
      .reporterName = tool::ReportName.getValue(),
      .reporterDirectory = tool::ReportDirectory.getValue(),
      .sourceInfoProvider = sourceInfoProvider,
      .compilationDatabaseAvailable = compilationDatabaseInfoAvailable
  };
  std::vector<std::unique_ptr<mull::Reporter>> reporters = reportersOption.reporters(params);

  mull::CXXJunkDetector junkDetector(astStorage);

  mull::MutationsFinder mutationsFinder(mutatorsOptions.mutators(), configuration);

  std::vector<std::unique_ptr<mull::Filter>> filterStorage;
  mull::Filters filters;

  auto *noDebugInfoFilter = new mull::NoDebugInfoFilter;
  auto *filePathFilter = new mull::FilePathFilter;

  filterStorage.emplace_back(noDebugInfoFilter);
  filterStorage.emplace_back(filePathFilter);

  filters.mutationFilters.push_back(noDebugInfoFilter);
  filters.functionFilters.push_back(noDebugInfoFilter);
  filters.instructionFilters.push_back(noDebugInfoFilter);

  filters.mutationFilters.push_back(filePathFilter);
  filters.functionFilters.push_back(filePathFilter);

  for (const auto &regex : tool::ExcludePaths) {
    filePathFilter->exclude(regex);
  }
  for (const auto &regex : tool::IncludePaths) {
    filePathFilter->include(regex);
  }

  if (tool::EnableAST && compilationDatabaseInfoAvailable) {
    std::vector<mull::MutatorKind> mutationKinds;
    for (auto &mutator : mutatorsOptions.mutators()) {
      mutationKinds.push_back(mutator->mutatorKind());
    }

    mull::MutatorKindSet mutatorKindSet = mull::MutatorKindSet::create(mutationKinds);

    mull::ASTFinder astFinder(mutatorKindSet);
    astFinder.findMutations(diagnostics, configuration, program, *filePathFilter, astStorage);

    auto *astMutationFilter = new mull::ASTMutationFilter(diagnostics, astStorage);
    filters.mutationFilters.push_back(astMutationFilter);
  }

  if (!tool::DisableJunkDetection.getValue()) {
    auto *junkFilter = new mull::JunkMutationFilter(junkDetector);
    filters.mutationFilters.push_back(junkFilter);
    filterStorage.emplace_back(junkFilter);
  }

  mull::Driver driver(diagnostics, configuration, program, toolchain, filters, mutationsFinder);
  auto result = driver.run();

  for (auto &reporter : reporters) {
    reporter->reportResults(*result);
  }

  llvm::llvm_shutdown();

  totalExecutionTime.finish();
  std::stringstream stringstream;
  stringstream << "Total execution time: " << totalExecutionTime.duration()
               << mull::MetricsMeasure::precision();
  diagnostics.info(stringstream.str());

  return 0;
}

//===- gccld.cpp - LLVM 'ld' compatible linker ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This utility is intended to be compatible with GCC, and follows standard
// system 'ld' conventions.  As such, the default output file is ./a.out.
// Additionally, this program outputs a shell script that is used to invoke LLI
// to execute the program.  In this manner, the generated executable (a.out for
// example), is directly executable, whereas the bytecode file actually lives in
// the a.out.bc file generated by this program.  Also, Force is on by default.
//
// Note that if someone (or a script) deletes the executable program generated,
// the .bc file will be left around.  Considering that this is a temporary hack,
// I'm not too worried about this.
//
//===----------------------------------------------------------------------===//

#include "gccld.h"
#include "llvm/Linker.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Bytecode/Reader.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Streams.h"
#include "llvm/System/Signals.h"
#include "llvm/Support/SystemUtils.h"
#include <fstream>
#include <memory>
using namespace llvm;

namespace {
  cl::list<std::string>
  InputFilenames(cl::Positional, cl::desc("<input bytecode files>"),
                 cl::OneOrMore);

  cl::opt<std::string>
  OutputFilename("o", cl::desc("Override output filename"), cl::init("a.out"),
                 cl::value_desc("filename"));

  cl::opt<bool>
  Verbose("v", cl::desc("Print information about actions taken"));

  cl::list<std::string>
  LibPaths("L", cl::desc("Specify a library search path"), cl::Prefix,
           cl::value_desc("directory"));

  cl::list<std::string>
  Libraries("l", cl::desc("Specify libraries to link to"), cl::Prefix,
            cl::value_desc("library prefix"));

  cl::opt<bool>
  Strip("strip-all", cl::desc("Strip all symbol info from executable"));
  cl::opt<bool>
  StripDebug("strip-debug",
             cl::desc("Strip debugger symbol info from executable"));

  cl::opt<bool>
  NoInternalize("disable-internalize",
                cl::desc("Do not mark all symbols as internal"));
  cl::alias
  ExportDynamic("export-dynamic", cl::desc("Alias for -disable-internalize"),
                cl::aliasopt(NoInternalize));

  cl::opt<bool>
  LinkAsLibrary("link-as-library", cl::desc("Link the .bc files together as a"
                                            " library, not an executable"));
  cl::alias
  Relink("r", cl::desc("Alias for -link-as-library"),
         cl::aliasopt(LinkAsLibrary));

  cl::opt<bool>
  Native("native", cl::ZeroOrMore,
         cl::desc("Generate a native binary instead of a shell script"));
  cl::opt<bool>
  NativeCBE("native-cbe", cl::ZeroOrMore,
            cl::desc("Generate a native binary with the C backend and GCC"));

  cl::opt<bool>
  SaveTemps("save-temps",
         cl::desc("Do not delete temporary files"));

  cl::list<std::string>
  RPath("rpath",
        cl::desc("Set runtime shared library search path (requires -native or"
                 " -native-cbe)"),
        cl::Prefix, cl::value_desc("directory"));

  cl::opt<std::string>
  SOName("soname",
         cl::desc("Set internal name of shared library (requires -native or"
                 " -native-cbe)"),
         cl::Prefix, cl::value_desc("name"));

  // Compatibility options that are ignored but supported by LD
  cl::opt<std::string>
  CO4("version-script", cl::Hidden, cl::desc("Compatibility option: ignored"));
  cl::opt<bool>
  CO5("eh-frame-hdr", cl::Hidden, cl::desc("Compatibility option: ignored"));
  cl::opt<std::string>
  CO6("h", cl::Hidden, cl::desc("Compatibility option: ignored"));
  cl::opt<bool>
  CO7("start-group", cl::Hidden, cl::desc("Compatibility option: ignored"));
  cl::opt<bool>
  CO8("end-group", cl::Hidden, cl::desc("Compatibility option: ignored"));

  cl::alias A0("s", cl::desc("Alias for --strip-all"),
               cl::aliasopt(Strip));
  cl::alias A1("S", cl::desc("Alias for --strip-debug"),
               cl::aliasopt(StripDebug));

}

/// PrintAndReturn - Prints a message to standard error and returns true.
///
/// Inputs:
///  progname - The name of the program (i.e. argv[0]).
///  Message  - The message to print to standard error.
///
static int PrintAndReturn(const char *progname, const std::string &Message) {
  cerr << progname << ": " << Message << "\n";
  return 1;
}

/// EmitShellScript - Output the wrapper file that invokes the JIT on the LLVM
/// bytecode file for the program.
static void EmitShellScript(char **argv) {
#if defined(_WIN32) || defined(__CYGWIN__)  
  // Windows doesn't support #!/bin/sh style shell scripts in .exe files.  To
  // support windows systems, we copy the llvm-stub.exe executable from the
  // build tree to the destination file.
  std::string ErrMsg;  
  sys::Path llvmstub = FindExecutable("llvm-stub.exe", argv[0]);
  if (llvmstub.isEmpty()) {
    cerr << "Could not find llvm-stub.exe executable!\n";
    exit(1);
  }
  if (0 != sys::CopyFile(sys::Path(OutputFilename), llvmstub, &ErrMsg)) {
    cerr << argv[0] << ": " << ErrMsg << "\n";
    exit(1);    
  }

  return;  
#endif

  // Output the script to start the program...
  std::ofstream Out2(OutputFilename.c_str());
  if (!Out2.good())
    exit(PrintAndReturn(argv[0], "error opening '" + OutputFilename +
                                 "' for writing!"));

  Out2 << "#!/bin/sh\n";
  // Allow user to setenv LLVMINTERP if lli is not in their PATH.
  Out2 << "lli=${LLVMINTERP-lli}\n";
  Out2 << "exec $lli \\\n";

  // We don't need to link in libc! In fact, /usr/lib/libc.so may not be a
  // shared object at all! See RH 8: plain text.
  std::vector<std::string>::iterator libc =
    std::find(Libraries.begin(), Libraries.end(), "c");
  if (libc != Libraries.end()) Libraries.erase(libc);
  // List all the shared object (native) libraries this executable will need
  // on the command line, so that we don't have to do this manually!
  for (std::vector<std::string>::iterator i = Libraries.begin(),
         e = Libraries.end(); i != e; ++i) {
    sys::Path FullLibraryPath = sys::Path::FindLibrary(*i);
    if (!FullLibraryPath.isEmpty() && FullLibraryPath.isDynamicLibrary())
      Out2 << "    -load=" << FullLibraryPath.toString() << " \\\n";
  }
  Out2 << "    $0.bc ${1+\"$@\"}\n";
  Out2.close();
}

// BuildLinkItems -- This function generates a LinkItemList for the LinkItems
// linker function by combining the Files and Libraries in the order they were
// declared on the command line.
static void BuildLinkItems(
  Linker::ItemList& Items,
  const cl::list<std::string>& Files,
  const cl::list<std::string>& Libraries) {

  // Build the list of linkage items for LinkItems.

  cl::list<std::string>::const_iterator fileIt = Files.begin();
  cl::list<std::string>::const_iterator libIt  = Libraries.begin();

  int libPos = -1, filePos = -1;
  while ( libIt != Libraries.end() || fileIt != Files.end() ) {
    if (libIt != Libraries.end())
      libPos = Libraries.getPosition(libIt - Libraries.begin());
    else
      libPos = -1;
    if (fileIt != Files.end())
      filePos = Files.getPosition(fileIt - Files.begin());
    else
      filePos = -1;

    if (filePos != -1 && (libPos == -1 || filePos < libPos)) {
      // Add a source file
      Items.push_back(std::make_pair(*fileIt++, false));
    } else if (libPos != -1 && (filePos == -1 || libPos < filePos)) {
      // Add a library
      Items.push_back(std::make_pair(*libIt++, true));
    }
  }
}

int main(int argc, char **argv, char **envp ) {
  llvm_shutdown_obj X;  // Call llvm_shutdown() on exit.
  cl::ParseCommandLineOptions(argc, argv, " llvm linker for GCC\n");
  sys::PrintStackTraceOnErrorSignal();

  int exitCode = 0;

  std::string ProgName = sys::Path(argv[0]).getBasename();
  Linker TheLinker(ProgName, OutputFilename, Verbose);

  try {
    // Remove any consecutive duplicates of the same library...
    Libraries.erase(std::unique(Libraries.begin(), Libraries.end()),
                    Libraries.end());

    TheLinker.addPaths(LibPaths);
    TheLinker.addSystemPaths();

    if (LinkAsLibrary) {
      std::vector<sys::Path> Files;
      for (unsigned i = 0; i < InputFilenames.size(); ++i )
        Files.push_back(sys::Path(InputFilenames[i]));

      if (TheLinker.LinkInFiles(Files))
        return 1; // Error already printed by linker

      // The libraries aren't linked in but are noted as "dependent" in the
      // module.
      for (cl::list<std::string>::const_iterator I = Libraries.begin(),
           E = Libraries.end(); I != E ; ++I) {
        TheLinker.getModule()->addLibrary(*I);
      }

    } else {
      // Build a list of the items from our command line
      Linker::ItemList Items;
      Linker::ItemList NativeItems;
      BuildLinkItems(Items, InputFilenames, Libraries);

      // Link all the items together
      if (TheLinker.LinkInItems(Items,NativeItems))
        return 1; // Error already printed

      // Revise the Libraries based on the remaining (native) libraries that
      // were not linked in to the bytecode. This ensures that we don't attempt
      // to pass a bytecode library to the native linker
      Libraries.clear(); // we've consumed the libraries except for native
      if ((Native || NativeCBE) && !NativeItems.empty()) {
        for (Linker::ItemList::const_iterator I = NativeItems.begin(), 
             E = NativeItems.end(); I != E; ++I) {
          Libraries.push_back(I->first);
        }
      }
    }

    // We're done with the Linker, so tell it to release its module
    std::auto_ptr<Module> Composite(TheLinker.releaseModule());

    // Create the output file.
    std::string RealBytecodeOutput = OutputFilename;
    if (!LinkAsLibrary || Native || NativeCBE) RealBytecodeOutput += ".bc";
    std::ios::openmode io_mode = std::ios::out | std::ios::trunc |
                                 std::ios::binary;
    std::ofstream Out(RealBytecodeOutput.c_str(), io_mode);
    if (!Out.good())
      return PrintAndReturn(argv[0], "error opening '" + RealBytecodeOutput +
                                     "' for writing!");

    // Ensure that the bytecode file gets removed from the disk if we get a
    // SIGINT signal.
    sys::RemoveFileOnSignal(sys::Path(RealBytecodeOutput));

    // Strip everything if Strip is set, otherwise if stripdebug is set, just
    // strip debug info.
    int StripLevel = Strip ? 2 : (StripDebug ? 1 : 0);

    // Internalize the module if neither -disable-internalize nor
    // -link-as-library are passed in.
    bool ShouldInternalize = !NoInternalize & !LinkAsLibrary;

    // Generate the bytecode file.
    if (GenerateBytecode(Composite.get(), StripLevel, ShouldInternalize, &Out)){
      Out.close();
      return PrintAndReturn(argv[0], "error generating bytecode");
    }

    // Close the bytecode file.
    Out.close();

    // Generate either a native file or a JIT shell script.  If the user wants
    // to generate a native file, compile it from the bytecode file. Otherwise,
    // if the target is not a library, create a script that will run the
    // bytecode through the JIT.
    if (Native) {
      // Name of the Assembly Language output file
      sys::Path AssemblyFile (OutputFilename);
      AssemblyFile.appendSuffix("s");

      // Mark the output files for removal if we get an interrupt.
      sys::RemoveFileOnSignal(AssemblyFile);
      sys::RemoveFileOnSignal(sys::Path(OutputFilename));

      // Determine the locations of the llc and gcc programs.
      sys::Path llc = FindExecutable("llc", argv[0]);
      if (llc.isEmpty())
        return PrintAndReturn(argv[0], "Failed to find llc");

      sys::Path gcc = FindExecutable("gcc", argv[0]);
      if (gcc.isEmpty())
        return PrintAndReturn(argv[0], "Failed to find gcc");

      // Generate an assembly language file for the bytecode.
      if (Verbose) cout << "Generating Assembly Code\n";
      std::string ErrMsg;
      if (0 != GenerateAssembly(
          AssemblyFile.toString(), RealBytecodeOutput, llc, ErrMsg, Verbose)) {
        cerr << argv[0] << ": " << ErrMsg << "\n";
        return 2;
      }
      if (Verbose) cout << "Generating Native Code\n";
      if (0 != GenerateNative(OutputFilename, AssemblyFile.toString(),
                     LibPaths, Libraries, gcc, envp, LinkAsLibrary,
                     NoInternalize, RPath, SOName, ErrMsg, Verbose) ) {
        cerr << argv[0] << ": " << ErrMsg << "\n";
        return 2;
      }

      if (!SaveTemps) {
        // Remove the assembly language file.
        AssemblyFile.eraseFromDisk();
        // Remove the bytecode language file.
        sys::Path(RealBytecodeOutput).eraseFromDisk();
      }

    } else if (NativeCBE) {
      sys::Path CFile (OutputFilename);
      CFile.appendSuffix("cbe.c");

      // Mark the output files for removal if we get an interrupt.
      sys::RemoveFileOnSignal(CFile);
      sys::RemoveFileOnSignal(sys::Path(OutputFilename));

      // Determine the locations of the llc and gcc programs.
      sys::Path llc = FindExecutable("llc", argv[0]);
      if (llc.isEmpty())
        return PrintAndReturn(argv[0], "Failed to find llc");

      sys::Path gcc = FindExecutable("gcc", argv[0]);
      if (gcc.isEmpty())
        return PrintAndReturn(argv[0], "Failed to find gcc");

      // Generate an assembly language file for the bytecode.
      if (Verbose) cout << "Generating C Source Code\n";
      std::string ErrMsg;
      if (0 != GenerateCFile(
          CFile.toString(), RealBytecodeOutput, llc, ErrMsg, Verbose)) {
        cerr << argv[0] << ": " << ErrMsg << "\n";
        return 2;
      }
      if (Verbose) cout << "Generating Native Code\n";
      if (0 != GenerateNative(OutputFilename, CFile.toString(),
                     LibPaths, Libraries, gcc, envp, LinkAsLibrary,
                     NoInternalize, RPath, SOName, ErrMsg, Verbose)) {
        cerr << argv[0] << ": " << ErrMsg << "\n";
        return 2;
      }

      if (!SaveTemps) {
        // Remove the assembly language file.
        CFile.eraseFromDisk();
        // Remove the bytecode language file.
        sys::Path(RealBytecodeOutput).eraseFromDisk();
      }

    } else if (!LinkAsLibrary) {
      EmitShellScript(argv);

      // Make the bytecode file readable and directly executable in LLEE
      std::string ErrMsg;
      if (sys::Path(RealBytecodeOutput).makeExecutableOnDisk(&ErrMsg)) {
        cerr << argv[0] << ": " << ErrMsg << "\n";
        return 1;
      }
      if (sys::Path(RealBytecodeOutput).makeReadableOnDisk(&ErrMsg)) {
        cerr << argv[0] << ": " << ErrMsg << "\n";
        return 1;
      }
    }

    // Make the output, whether native or script, executable as well...
    std::string ErrMsg;
    if (sys::Path(OutputFilename).makeExecutableOnDisk(&ErrMsg)) {
      cerr << argv[0] << ": " << ErrMsg << "\n";
      return 1;
    }
  } catch (const char*msg) {
    cerr << argv[0] << ": " << msg << "\n";
    exitCode = 1;
  } catch (const std::string& msg) {
    cerr << argv[0] << ": " << msg << "\n";
    exitCode = 2;
  } catch (...) {
    // This really shouldn't happen, but just in case ....
    cerr << argv[0] << ": An unexpected unknown exception occurred.\n";
    exitCode = 3;
  }

  return exitCode;
}

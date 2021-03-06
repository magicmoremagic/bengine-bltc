#include "bltc_app.hpp"
#include "version.hpp"
#include <be/core/version.hpp>
#include <be/blt/version.hpp>
#include <be/blt/blt.hpp>
#include <be/cli/cli.hpp>
#include <be/core/logging.hpp>
#include <be/core/log_exception.hpp>
#include <be/util/get_file_contents.hpp>
#include <be/util/path_glob.hpp>
#include <be/core/alg.hpp>
#include <iostream>
#include <fstream>

namespace be {
namespace bltc {
namespace {

///////////////////////////////////////////////////////////////////////////////
S process_stdin() {
   std::ostringstream oss;
   oss << std::cin.rdbuf();

   if (std::cin.fail() || std::cin.bad()) {
      throw std::ios::failure("Error while reading from stdin!");
   }

   return oss.str();
}

///////////////////////////////////////////////////////////////////////////////
S get_stdin() {
   static S input = process_stdin();
   return input;
}

} // be::bltc::()

///////////////////////////////////////////////////////////////////////////////
BltcApp::BltcApp(int argc, char** argv) {
   default_log().verbosity_mask(v::info_or_worse);
   try {
      using namespace cli;
      using namespace color;
      using namespace ct;
      Processor proc;

      S dest;
      DestType dest_type = DestType::path;

      bool show_version = false;
      bool show_help = false;
      bool verbose = false;
      S help_query;

      proc
         (prologue (Table() << header << "BLT COMPILER").query())

         (synopsis (Cell() << fg_dark_gray << "[ " << fg_cyan << "OPTIONS"
                           << fg_dark_gray << " ] [ " << fg_cyan << "INPUT"
                           << fg_dark_gray << " [ " << fg_cyan << "INPUT"
                           << fg_dark_gray << " ...]]"))

         (abstract ("BLTC compiles Backtick Lua Template (BLT) files to Lua source code."))

         (abstract ("By default file inputs will be compiled to a file of the same name with extension '.lua'. "
                    "When processing non-file inputs, the output will be sent to stdout by default.").verbose())

         (param ({ "o" },{ "output" }, "PATH", [&](const S& str) {
               dest = str;
               dest_type = DestType::path;
            }).desc("Specifies an output path where the next compiled input should be saved.")
              .extra(Cell() << nl << "Must be specified before the input it affects.  Only a single input will be affected.  "
                                     "Relative paths will be resolved based on the path specified by "
                            << fg_yellow << "--output-dir" << reset
                            << " or the working directory.  If the specified file does not exist, it will be created; "
                               "otherwise it will be overwritten."))

         (flag ({ },{ "stdout" }, dest_type, DestType::console)
            .desc("Outputs the next compiled input to standard output.")
            .extra(Cell() << nl << "Must be specified before the input it affects.  Only a single input will be affected."))

         (flag ({ },{ "debug" }, debug_mode_)
            .desc("Outputs parse trees instead of the compiled output.")
            .extra(Cell() << nl << "Applies to all inputs, including those that were specified "
                                   "earlier on the command line."))

         (param ({ "I" },{ "input" }, "STRING", [&](const S& str) {
               if (dest.empty()) {
                  dest_type = DestType::console;
               }
               jobs_.push_back({ str, dest, SourceType::raw, dest_type });
               dest.clear();
               dest_type = DestType::path;
            }).desc(Cell() << "Treats " << fg_cyan << "STRING" << reset << " as a raw BLT template instead of a filename.")
              .extra(Cell() << nl << "If no output file is specified, it will be directed to standard output."))

         (flag ({ },{ "stdin" }, [&]() {
               if (dest.empty()) {
                  dest_type = DestType::console;
               }
               jobs_.push_back({ S(), dest, SourceType::console, dest_type });
               dest.clear();
               dest_type = DestType::path;
            }).desc("Reads data from standard input and treats it as an input.")
              .extra(Cell() << nl << "If no output file is specified, it will be directed to standard output.  "
                                     "Input ends when the first EOF character is encountered.  If multiple "
                            << fg_yellow << "--stdin" << reset << " flags are provided, the same input will be used for each."))

         (any ([&](const S& str) {
               jobs_.push_back({ str, dest, SourceType::path, dest_type });
               dest.clear();
               dest_type = DestType::path;
               return true;
            }))

         (param ({ "D" },{ "input-dir" }, "PATH", [&](const S& str) {
               util::parse_multi_path(str, search_paths_);
            }).desc("Specifies a search path in which to search for input files.")
              .extra(Cell() << nl << "Multiple input directories may be specified by separating them with ';' or ':', or by using multiple "
                            << fg_yellow << "--input-dir" << reset
                            << " options.  Directories will be searched in the order they are specified.  If no input directories "
                               "are specified, the working directory is implicitly searched.  The search path applies to all "
                               "input files, including ones specified earlier on the command line."))

         (param ({ "d" },{ "output-dir" }, "PATH", [&](const S& str) {
               if (!output_path_.empty()) {
                  throw std::runtime_error("An output directory has already been specified");
               }
               output_path_ = util::parse_path(str);
            }).desc("Specifies a directory to resolve relative output paths.")
              .extra(Cell() << nl << "If no output directory or filename is specified files will be saved in the same directory as "
                                     "the input file.  If an output filename is specified but not an output directory, the working "
                                     "directory will be used.  Only one output directory may be specified, and it applies to all "
                                     "inputs, including those specified earlier on the command line."))

         (end_of_options ())

         (verbosity_param ({ "v" },{ "verbosity" }, "LEVEL", default_log().verbosity_mask()))

         (flag ({ "V" },{ "version" }, show_version).desc("Prints version information to standard output."))

         (param ({ "?" },{ "help" }, "OPTION", [&](const S& value) {
               show_help = true;
               help_query = value;
            }).default_value(S())
              .allow_options_as_values(true)
              .desc(Cell() << "Outputs this help message.  For more verbose help, use " << fg_yellow << "--help")
              .extra(Cell() << nl << "If " << fg_cyan << "OPTION" << reset
                            << " is provided, the options list will be filtered to show only options that contain that string."))

         (flag ({ },{ "help" }, verbose).ignore_values(true))

         (exit_code (0, "There were no errors."))
         (exit_code (1, "An unknown error occurred."))
         (exit_code (2, "There was a problem parsing the command line arguments."))
         (exit_code (3, "An input file does not exist or is a directory."))
         (exit_code (4, "An I/O error occurred while reading an input file."))
         (exit_code (5, "An I/O error occurred while writing an output file."))
         (exit_code (6, "A BLT lexer or parser error occurred."))

         (example (Cell() << fg_gray << "foo.blt",
            "Compiles a file named 'foo.blt' in the working directory and saves the output to 'foo.lua'."))
         (example (Cell() << fg_yellow << "-d " << fg_cyan << "out/" << fg_gray << " bar.blt",
            "Compiles a file named 'bar.blt' in the working directory and saves the output to 'out/bar.lua'."))
          (example (Cell() << fg_yellow << "--output " << fg_cyan << "asdf" << fg_yellow << " --stdin -o "
                           << fg_cyan << "bar_out" << fg_gray << " bar.blt",
            "Compiles a template read from stdin and saves the output to a file called 'asdf' in the working directory, "
            "then compiles a file named 'bar.blt' in the working directory and saves the output to 'bar_out'."))

         ;

      proc.process(argc, argv);

      if (!show_help && !show_version && jobs_.empty()) {
         show_help = true;
         show_version = true;
         status_ = 1;
      }

      if (show_version) {
         proc
            (prologue (BE_BLTC_VERSION_STRING).query())
            (prologue (BE_BLT_VERSION_STRING).query())
            (license (BE_LICENSE).query())
            (license (BE_COPYRIGHT).query())
            ;
      }

      if (show_help) {
         proc.describe(std::cout, verbose, help_query);
      } else if (show_version) {
         proc.describe(std::cout, verbose, ids::cli_describe_section_prologue);
         proc.describe(std::cout, verbose, ids::cli_describe_section_license);
      }

   } catch (const cli::OptionError& e) {
      status_ = 2;
      cli::log_exception(e);
   } catch (const cli::ArgumentError& e) {
      status_ = 2;
      cli::log_exception(e);
   } catch (const FatalTrace& e) {
      status_ = 2;
      log_exception(e);
   } catch (const RecoverableTrace& e) {
      status_ = 2;
      log_exception(e);
   } catch (const fs::filesystem_error& e) {
      status_ = 2;
      log_exception(e);
   } catch (const std::system_error& e) {
      status_ = 2;
      log_exception(e);
   } catch (const std::exception& e) {
      status_ = 2;
      log_exception(e);
   }
}

///////////////////////////////////////////////////////////////////////////////
int BltcApp::operator()() {
   if (status_ != 0) {
      return status_;
   }

   try {
      if (search_paths_.empty()) {
         search_paths_.push_back(util::cwd());
      }

      for (const Path& p : search_paths_) {
         be_short_verbose() << "Search path: " << color::fg_gray << p.generic_string() | default_log();
      }

      if (!output_path_.empty()) {
         output_path_ = fs::absolute(output_path_);
         if (!fs::exists(output_path_)) {
            fs::create_directories(output_path_);
         }
         if (!fs::is_directory(output_path_)) {
            status_ = 5;
            be_error() << "Output path is not a directory"
               & attr(ids::log_attr_path) << output_path_
               | default_log();
            return status_;
         }

         be_short_verbose() << "Output path: " << color::fg_gray << output_path_.generic_string() | default_log();
      }
   } catch (const FatalTrace& e) {
      status_ = 1;
      log_exception(e);
   } catch (const RecoverableTrace& e) {
      status_ = 1;
      log_exception(e);
   } catch (const fs::filesystem_error& e) {
      status_ = 1;
      log_exception(e);
   } catch (const std::system_error& e) {
      status_ = 1;
      log_exception(e);
   } catch (const std::exception& e) {
      status_ = 1;
      log_exception(e);
   }

   if (status_ != 0) {
      return status_;
   }

   try {
      for (auto& job : jobs_) {
         process_(job);
      }
   } catch (const FatalTrace& e) {
      status_ = std::max(status_, (I8)1);
      log_exception(e);
   } catch (const RecoverableTrace& e) {
      status_ = std::max(status_, (I8)1);
      log_exception(e);
   } catch (const fs::filesystem_error& e) {
      status_ = std::max(status_, (I8)1);
      log_exception(e);
   } catch (const std::system_error& e) {
      status_ = std::max(status_, (I8)1);
      log_exception(e);
   } catch (const std::exception& e) {
      status_ = std::max(status_, (I8)1);
      log_exception(e);
   }

   return status_;
}

///////////////////////////////////////////////////////////////////////////////
void BltcApp::process_(Job& job) {
   try {
      if (job.source_type == SourceType::path) {
         Path source = util::parse_path(job.source);

         be_short_verbose() << "Processing input path: " << color::fg_gray << S(job.source) | default_log();

         if (source.is_absolute() && fs::exists(source)) {
            process_path_(source, job);
            return;
         }

         std::vector<Path> paths = util::glob(job.source, search_paths_, util::PathMatchType::files_and_misc);
         if (!paths.empty()) {

            if (paths.size() > 1) {
               for (const Path& p : paths) {
                  be_short_verbose() << "Expanded input path match: " << color::fg_gray << p.generic_string() | default_log();
               }
            }

            for (Path& p : paths) {
               Job copy = job;
               process_path_(p, copy);
            }
            return;
         }

         status_ = std::max(status_, I8(3));

         LogRecord rec;
         be_warn() << "No files found matching " << color::fg_gray << source.generic_string() || rec;

         for (Path& p : search_paths_) {
            log_nil() & attr(ids::log_attr_search_path) << p.generic_string() || rec;
         }

         rec | default_log();

      } else if (job.source_type == SourceType::console) {
         be_short_verbose() << "Processing stdin"
            | default_log();

         process_non_path_(get_stdin(), job);
      } else {
         be_short_verbose() << "Processing template from command line"
            | default_log();

         process_non_path_(job.source, job);
      }

   } catch (const FatalTrace& e) {
      status_ = 1;
      log_exception(e);
   } catch (const RecoverableTrace& e) {
      status_ = 1;
      log_exception(e);
   } catch (const fs::filesystem_error& e) {
      status_ = 4;
      log_exception(e);
   } catch (const std::system_error& e) {
      status_ = 1;
      log_exception(e);
   } catch (const std::exception& e) {
      status_ = 1;
      log_exception(e);
   }
}

void BltcApp::process_path_(const Path& path, Job& job) {
   S data;
   try {
      if (job.dest_type == DestType::path) {
         Path dest;
         if (job.dest.empty()) {
            if (output_path_.empty()) {
               dest = path;
            } else {
               dest = output_path_;
               dest /= path;
            }

            dest.replace_extension("lua");

         } else {
            dest = job.dest;
            if (dest.is_relative() && !output_path_.empty()) {
               dest = output_path_;
               dest /= job.dest;
            }
         }
         job.dest = dest.string();
      }

      be_short_verbose() << "Loading file: " << color::fg_gray << path.generic_string() | default_log();

      data = util::get_file_contents_string(path);

   } catch (const FatalTrace& e) {
      status_ = std::max(status_, (I8)4);
      log_exception(e);
   } catch (const RecoverableTrace& e) {
      status_ = std::max(status_, (I8)4);
      log_exception(e);
   } catch (const fs::filesystem_error& e) {
      status_ = std::max(status_, (I8)4);
      log_exception(e);
   } catch (const std::system_error& e) {
      status_ = std::max(status_, (I8)4);
      log_exception(e);
   } catch (const std::exception& e) {
      status_ = std::max(status_, (I8)4);
      log_exception(e);
   }

   process_raw_(data, job);
}

void BltcApp::process_non_path_(const S& data, Job& job) {
   if (job.dest_type == DestType::path) {
      if (job.dest.empty()) {
         job.dest_type = DestType::console;
      } else {
         Path dest = job.dest;
         if (dest.is_relative() && !output_path_.empty()) {
            dest = output_path_;
            dest /= job.dest;
         }
         job.dest = dest.string();
      }
   }

   process_raw_(data, job);
}

void BltcApp::process_raw_(const S& data, Job& job) {
   std::ofstream ofs;
   std::ostream* os = nullptr;
   if (job.dest_type == DestType::path) {
      try {
         be_short_verbose() << "Opening output file: " << color::fg_gray << S(job.dest) | default_log();

         ofs.open(Path(job.dest).native(), std::ios::binary);
         os = &ofs;
      } catch (const FatalTrace& e) {
         status_ = std::max(status_, (I8)5);
         log_exception(e);
      } catch (const RecoverableTrace& e) {
         status_ = std::max(status_, (I8)5);
         log_exception(e);
      } catch (const fs::filesystem_error& e) {
         status_ = std::max(status_, (I8)5);
         log_exception(e);
      } catch (const std::system_error& e) {
         status_ = std::max(status_, (I8)5);
         log_exception(e);
      } catch (const std::exception& e) {
         status_ = std::max(status_, (I8)5);
         log_exception(e);
      }
   } else {
      be_short_verbose() << "Outputting to stdout"
         | default_log();

      os = &std::cout;
   }

   if (!os || !(*os)) {
      return;
   }

   try {
      if (debug_mode_) {
         blt::debug_blt(data, *os);
      } else {
         blt::compile_blt(data, *os);
      }
   } catch (const FatalTrace& e) {
         status_ = std::max(status_, (I8)6);
      log_exception(e);
   } catch (const RecoverableTrace& e) {
      status_ = std::max(status_, (I8)6);
      log_exception(e);
   } catch (const fs::filesystem_error& e) {
      status_ = std::max(status_, (I8)6);
      log_exception(e);
   } catch (const std::system_error& e) {
      status_ = std::max(status_, (I8)6);
      log_exception(e);
   } catch (const std::exception& e) {
      status_ = std::max(status_, (I8)6);
      log_exception(e);
   }
}

} // be::bltc
} // be

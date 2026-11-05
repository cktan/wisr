/*
 * main.cc -- driver entry point for the rel relational interpreter.
 */

#include "rel.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

/* Print usage instructions for the rel driver. */
static void print_usage(const char *prog) {
  std::cout
      << "Usage: " << prog << " [options] [script.rel]\n\n"
      << "Options:\n"
      << "  -e <code>    Execute inline Janet/rel code string\n"
      << "  -h, --help   Show this help message\n\n"
      << "If no script or code is provided, starts an interactive REPL.\n";
}

/* Run interactive REPL or evaluate script. */
int main(int argc, char **argv) {
  // Initialize Janet runtime.
  janet_init();
  JanetTable *env = janet_core_env(nullptr);

  // Register rel functions, macros and types.
  rel::register_rel_cfuns(env);

  int exit_code = 0;

  if (argc > 1) {
    std::string arg1 = argv[1];
    if (arg1 == "-h" || arg1 == "--help") {
      print_usage(argv[0]);
      janet_deinit();
      return 0;
    } else if (arg1 == "-e") {
      if (argc < 3) {
        std::cerr << "rel: -e requires an argument\n";
        janet_deinit();
        return 1;
      }
      Janet result;
      int status = janet_dostring(env, argv[2], "command_line", &result);
      if (status != 0) {
        JanetBuffer *buf = janet_buffer(64);
        janet_description_b(buf, result);
        std::cerr << "rel error: "
                  << std::string(reinterpret_cast<char *>(buf->data),
                                 buf->count)
                  << "\n";
        exit_code = 1;
      }
    } else {
      // Read and execute script file.
      std::ifstream file(arg1);
      if (!file.is_open()) {
        std::cerr << "rel: cannot open file: " << arg1 << "\n";
        janet_deinit();
        return 1;
      }
      std::stringstream buffer;
      buffer << file.rdbuf();
      std::string code = buffer.str();

      Janet result;
      int status =
          janet_dobytes(env, reinterpret_cast<const uint8_t *>(code.data()),
                        code.size(), arg1.c_str(), &result);
      if (status != 0) {
        JanetBuffer *buf = janet_buffer(64);
        janet_description_b(buf, result);
        std::cerr << "rel error in " << arg1 << ": "
                  << std::string(reinterpret_cast<char *>(buf->data),
                                 buf->count)
                  << "\n";
        exit_code = 1;
      }
    }
  } else {
    // Interactive REPL mode.
    std::cout << "rel -- wisr relational test driver\n";
    std::cout << "CK's SQL-less relational interpreter\n";
    std::cout << "Type (exit) or Ctrl+D to quit.\n\n";

    Janet result;
    int status = janet_dostring(env, "(repl)", "repl", &result);
    if (status != 0) {
      exit_code = 1;
    }
  }

  // Clean up catalog and detach environment before shutdown.
  rel::get_catalog().reset();

  // Deinitialize Janet runtime.
  janet_deinit();
  return exit_code;
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * main.cc -- the driver: run one scenario script and print its
 * transcript (mvcc/DRIVER.md).
 *
 *   driver [--block-ms N] [--chained] script.t
 *
 * The transcript goes to stdout, so a caller redirects it into
 * out/ and diffs it against good/. The exit status is 0 only
 * if every check in the script was satisfied.
 */

#include "driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

/* Parse the arguments, run the script, print the transcript. */
int main(int argc, char **argv) {
  int block_ms = 250;
  bool chained = false;
  const char *path = nullptr;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--block-ms") == 0 && i + 1 < argc) {
      block_ms = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--chained") == 0) {
      chained = true;
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "usage: driver [--block-ms N] [--chained] script.t\n");
      return 2;
    } else {
      path = argv[i];
    }
  }
  if (!path) {
    fprintf(stderr, "usage: driver [--block-ms N] [--chained] script.t\n");
    return 2;
  }

  // Parse first: a malformed script is an error, not a run.
  script_t script;
  std::string err;
  if (!script_parse(path, &script, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }

  // Run every selected pairing, then print what happened.
  std::string transcript;
  int failures = script_run(script, block_ms, chained, &transcript);
  fputs(transcript.c_str(), stdout);
  if (failures < 0) {
    fprintf(stderr, "%s: the database could not be created\n", path);
    return 2;
  }
  if (failures > 0) {
    fprintf(stderr, "%s: %d check(s) failed\n", path, failures);
    return 1;
  }
  return 0;
}

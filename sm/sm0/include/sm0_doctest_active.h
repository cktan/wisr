/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * sm0_doctest_active.h -- a doctest listener that names the test case
 * running now in a file beside the test binary, BINARY.active, so a
 * run whose output goes to a log can be asked which test it is in:
 * the case's name, then its file and line. The file goes when the run
 * ends; a run that crashes or hangs leaves it naming its test.
 *
 * A test binary's main.cc includes it once, after doctest.h with its
 * implementation.
 */
#ifndef SM0_DOCTEST_ACTIVE_H
#define SM0_DOCTEST_ACTIVE_H

#include <cstdio>
#include <string>
#include <unistd.h>

namespace sm0_doctest {

/* The listener: every hook but a test case's start and the run's end
 * does nothing. */
struct active_t : public doctest::IReporter {
  /* Name the file beside the binary, from /proc/self/exe. */
  explicit active_t(const doctest::ContextOptions &) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = '\0';
      m_path = std::string(exe) + ".active";
    }
  }

  /* Write the case's name, file and line, whole: a reader sees the
   * last case or this one, never half of one. */
  void test_case_start(const doctest::TestCaseData &tc) override {
    if (m_path.empty()) {
      return;
    }
    std::string tmp = m_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (nullptr == f) {
      return;
    }
    std::fprintf(f, "%s\n%s:%u\n", tc.m_name, tc.m_file.c_str(), tc.m_line);
    std::fclose(f);
    std::rename(tmp.c_str(), m_path.c_str());
  }

  /* The run ended: no case is active. */
  void test_run_end(const doctest::TestRunStats &) override {
    if (!m_path.empty()) {
      std::remove(m_path.c_str());
    }
  }

  void report_query(const doctest::QueryData &) override {}
  void test_run_start() override {}
  void test_case_reenter(const doctest::TestCaseData &) override {}
  void test_case_end(const doctest::CurrentTestCaseStats &) override {}
  void test_case_exception(const doctest::TestCaseException &) override {}
  void subcase_start(const doctest::SubcaseSignature &) override {}
  void subcase_end() override {}
  void log_assert(const doctest::AssertData &) override {}
  void log_message(const doctest::MessageData &) override {}
  void test_case_skipped(const doctest::TestCaseData &) override {}

private:
  std::string m_path; /* BINARY.active, or empty if unknown */
};

REGISTER_LISTENER("active", 1, active_t);

} // namespace sm0_doctest

#endif

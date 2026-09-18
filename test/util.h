/*
 * util.h -- helpers shared by the test files.
 */
#ifndef WISR_TEST_UTIL_H
#define WISR_TEST_UTIL_H

#include <stdlib.h>

#include <filesystem>
#include <string>

/* A fresh directory under the system temp dir, removed at scope exit. */
class tmpdir_t
{
  public:
    /* Create the directory. */
    tmpdir_t()
    {
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "wisr-test-XXXXXX")
                .string();
        m_path = mkdtemp(tmpl.data());
    }

    /* Remove the directory and everything in it. */
    ~tmpdir_t()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    /* The directory's path. */
    const char* path() const
    {
        return m_path.c_str();
    }

  private:
    std::string m_path;
};

#endif /* WISR_TEST_UTIL_H */

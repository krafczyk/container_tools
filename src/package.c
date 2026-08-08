/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "package.h"

#include "package_identity.h"

#include <ctype.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CT_PACKAGE_PATH_MAX 4096U
#define CT_PACKAGE_SCRIPT_MAX 4096U
#define CT_PACKAGE_FAILURE 78

static const char *const ct_compatibility_scripts[] = {
  "ct_exec.sh", "ct_shell.sh", "ct_instance_exec.sh", "ct_mount_detector.sh",
  "ct_args.sh",
};

static int ct_package_write_failure(FILE *diagnostics, const char *message)
{
  if (diagnostics != NULL) {
    (void)fprintf(diagnostics, "container-tools: package verification failed: %s\n",
                  message);
  }
  return CT_PACKAGE_FAILURE;
}

const char *ct_package_release_json(void)
{
  return "{\"schema\":\"container-tools.release/v1\",\"product_version\":\"" CT_PRODUCT_VERSION
         "\",\"product_major\":" CT_PRODUCT_MAJOR ",\"package_epoch\":\"" CT_PACKAGE_EPOCH
         "\",\"architecture\":\"" CT_ARCHITECTURE "\",\"source_commit\":\"" CT_SOURCE_COMMIT
         "\",\"build_identity\":\"" CT_BUILD_IDENTITY "\",\"manifest_grammar\":\""
         CT_MANIFEST_GRAMMAR "\",\"component_digests\":{\"executable\":\""
         CT_EXECUTABLE_DIGEST "\",\"compatibility_scripts\":\"" CT_SCRIPTS_DIGEST
         "\",\"metadata\":\"" CT_METADATA_DIGEST "\",\"documentation\":\""
         CT_DOCUMENTATION_DIGEST "\",\"licenses\":\"" CT_LICENSES_DIGEST
         "\",\"protocols\":\"" CT_PROTOCOLS_DIGEST "\"}}";
}

bool ct_package_release_json_is_current(const char *release_json)
{
  return release_json != NULL && strcmp(release_json, ct_package_release_json()) == 0;
}

bool ct_package_normalize_architecture(const char *input, char *output,
                                       size_t output_size)
{
  size_t index;

  if (input == NULL || output == NULL || output_size < 2U) {
    return false;
  }
  for (index = 0U; input[index] != '\0'; ++index) {
    const unsigned char byte = (unsigned char)input[index];

    if (index + 1U >= output_size ||
        !(isalnum(byte) != 0 || byte == (unsigned char)'_' ||
          byte == (unsigned char)'.' || byte == (unsigned char)'-')) {
      return false;
    }
    output[index] = (char)tolower(byte);
  }
  if (index == 0U) {
    return false;
  }
  output[index] = '\0';
  return true;
}

static bool ct_package_read_file(const char *path, char *contents, size_t capacity,
                                 size_t *length)
{
  int descriptor;
  size_t total = 0U;

  if (path == NULL || contents == NULL || capacity < 2U || length == NULL) {
    return false;
  }
  descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  while (total < capacity - 1U) {
    const ssize_t bytes_read = read(descriptor, contents + total,
                                    capacity - 1U - total);

    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)close(descriptor);
      return false;
    }
    if (bytes_read == 0) {
      contents[total] = '\0';
      *length = total;
      (void)close(descriptor);
      return true;
    }
    total += (size_t)bytes_read;
  }
  (void)close(descriptor);
  return false;
}

static bool ct_package_self_path(char *path, size_t path_size)
{
  const ssize_t count = readlink("/proc/self/exe", path, path_size - 1U);

  if (count <= 0 || (size_t)count >= path_size - 1U) {
    return false;
  }
  path[(size_t)count] = '\0';
  return true;
}

static bool ct_package_prefix_path(const char *self_path, const char *relative,
                                   char *path, size_t path_size)
{
  static const char suffix[] = "/bin/container-tools";
  size_t self_length;
  size_t suffix_length;
  size_t root_length;
  int written;

  self_length = strlen(self_path);
  suffix_length = sizeof(suffix) - 1U;
  if (self_length < suffix_length ||
      strcmp(self_path + self_length - suffix_length, suffix) != 0) {
    return false;
  }
  root_length = self_length - suffix_length;
  written = snprintf(path, path_size, "%.*s/%s", (int)root_length, self_path,
                     relative);
  return written > 0 && (size_t)written < path_size;
}

static bool ct_package_is_static_elf(const char *path)
{
  int descriptor;
  unsigned char ident[EI_NIDENT];
  struct stat status;

  descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    if (descriptor >= 0) {
      (void)close(descriptor);
    }
    return false;
  }
  if (read(descriptor, ident, sizeof(ident)) != (ssize_t)sizeof(ident) ||
      memcmp(ident, ELFMAG, SELFMAG) != 0 || ident[EI_DATA] != ELFDATA2LSB) {
    (void)close(descriptor);
    return false;
  }
  if (ident[EI_CLASS] == ELFCLASS64) {
    Elf64_Ehdr header;
    Elf64_Phdr program_header;
    Elf64_Half index;

    if (lseek(descriptor, 0, SEEK_SET) < 0 ||
        read(descriptor, &header, sizeof(header)) != (ssize_t)sizeof(header) ||
        header.e_phentsize != sizeof(program_header)) {
      (void)close(descriptor);
      return false;
    }
    for (index = 0U; index < header.e_phnum; ++index) {
      const off_t offset = (off_t)header.e_phoff +
                           (off_t)index * (off_t)sizeof(program_header);

      if (pread(descriptor, &program_header, sizeof(program_header), offset) !=
          (ssize_t)sizeof(program_header)) {
        (void)close(descriptor);
        return false;
      }
#if !defined(CT_PACKAGE_TEST_DYNAMIC)
      if (program_header.p_type == PT_INTERP || program_header.p_type == PT_DYNAMIC) {
        (void)close(descriptor);
        return false;
      }
#endif
    }
  } else if (ident[EI_CLASS] == ELFCLASS32) {
    Elf32_Ehdr header;
    Elf32_Phdr program_header;
    Elf32_Half index;

    if (lseek(descriptor, 0, SEEK_SET) < 0 ||
        read(descriptor, &header, sizeof(header)) != (ssize_t)sizeof(header) ||
        header.e_phentsize != sizeof(program_header)) {
      (void)close(descriptor);
      return false;
    }
    for (index = 0U; index < header.e_phnum; ++index) {
      const off_t offset = (off_t)header.e_phoff +
                           (off_t)index * (off_t)sizeof(program_header);

      if (pread(descriptor, &program_header, sizeof(program_header), offset) !=
          (ssize_t)sizeof(program_header)) {
        (void)close(descriptor);
        return false;
      }
#if !defined(CT_PACKAGE_TEST_DYNAMIC)
      if (program_header.p_type == PT_INTERP || program_header.p_type == PT_DYNAMIC) {
        (void)close(descriptor);
        return false;
      }
#endif
    }
  } else {
    (void)close(descriptor);
    return false;
  }
  (void)close(descriptor);
  return true;
}

static bool ct_package_script_is_current(const char *path, const char *script_name)
{
  char contents[CT_PACKAGE_SCRIPT_MAX];
  char expected[CT_PACKAGE_SCRIPT_MAX];
  size_t length;
  const int expected_length = snprintf(
      expected, sizeof(expected),
      "#!/bin/sh\n"
      "# Generated package-relative compatibility trampoline. SPDX-License-Identifier: Apache-2.0 OR MIT\n"
      "CT_PACKAGE_IDENTITY='%s'\n"
      "CT_COMPATIBILITY_SCRIPT='%s'\n"
      "\n"
      "script_dir=$(CDPATH='' cd -- \"$(dirname -- \"$0\")\" && pwd -P) || exit 78\n"
      "exec \"$script_dir/container-tools\" --internal-compat \"$CT_PACKAGE_IDENTITY\" \\\n"
      "  \"$CT_COMPATIBILITY_SCRIPT\" \"$@\"\n",
      CT_BUILD_IDENTITY, script_name);

  return expected_length > 0 && (size_t)expected_length < sizeof(expected) &&
         ct_package_read_file(path, contents, sizeof(contents), &length) &&
         length == (size_t)expected_length &&
         memcmp(contents, expected, length) == 0;
}

int ct_package_verify_compatibility_identity(const char *identity,
                                             const char *script_name)
{
  size_t index;

  if (identity == NULL || script_name == NULL || strcmp(identity, CT_BUILD_IDENTITY) != 0) {
    return CT_PACKAGE_FAILURE;
  }
  for (index = 0U; index < sizeof(ct_compatibility_scripts) /
                                  sizeof(ct_compatibility_scripts[0]);
       ++index) {
    if (strcmp(script_name, ct_compatibility_scripts[index]) == 0) {
      return 0;
    }
  }
  return CT_PACKAGE_FAILURE;
}

static int ct_package_verify_impl(FILE *diagnostics, bool json, bool report_success)
{
  char self_path[CT_PACKAGE_PATH_MAX];
  char metadata_path[CT_PACKAGE_PATH_MAX];
  char script_path[CT_PACKAGE_PATH_MAX];
  char metadata[CT_PACKAGE_RELEASE_JSON_MAX + 2U];
  size_t metadata_length;
  size_t index;
  const size_t expected_length = strlen(ct_package_release_json());

  if (!ct_package_self_path(self_path, sizeof(self_path)) ||
      !ct_package_is_static_elf(self_path)) {
    return ct_package_write_failure(diagnostics, "executable is not a static package binary");
  }
  if (!ct_package_prefix_path(self_path, "share/container-tools/release.json", metadata_path,
                              sizeof(metadata_path)) ||
      !ct_package_read_file(metadata_path, metadata, sizeof(metadata), &metadata_length) ||
      metadata_length != expected_length + 1U || metadata[expected_length] != '\n' ||
      memcmp(metadata, ct_package_release_json(), expected_length) != 0) {
    return ct_package_write_failure(diagnostics, "release metadata does not match executable");
  }
  for (index = 0U; index < sizeof(ct_compatibility_scripts) /
                                  sizeof(ct_compatibility_scripts[0]);
       ++index) {
    if (!ct_package_prefix_path(self_path, "bin", script_path, sizeof(script_path))) {
      return ct_package_write_failure(diagnostics, "package bin directory is invalid");
    }
    if (snprintf(script_path + strlen(script_path),
                 sizeof(script_path) - strlen(script_path), "/%s",
                 ct_compatibility_scripts[index]) < 0 ||
        !ct_package_script_is_current(script_path, ct_compatibility_scripts[index])) {
      return ct_package_write_failure(diagnostics, "compatibility script does not match executable");
    }
  }
  if (diagnostics != NULL && report_success) {
    if (json) {
      (void)fprintf(diagnostics, "%s\n", ct_package_release_json());
    } else {
      (void)fprintf(diagnostics, "container-tools package is coherent: %s\n",
                    CT_BUILD_IDENTITY);
    }
  }
  return 0;
}

int ct_package_verify(FILE *diagnostics, bool json)
{
  return ct_package_verify_impl(diagnostics, json, true);
}

int ct_package_validate(FILE *diagnostics)
{
  return ct_package_verify_impl(diagnostics, false, false);
}

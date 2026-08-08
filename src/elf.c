/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "elf.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int ct_elf_read(int descriptor, void *buffer, size_t length, off_t offset)
{
  const ssize_t read_count = pread(descriptor, buffer, length, offset);
  return read_count == (ssize_t)length ? 0 : 1;
}

static int ct_elf_native(unsigned char *elf_class, unsigned char *data,
                         unsigned int *machine)
{
  unsigned char ident[EI_NIDENT];
  int descriptor = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  int result = 1;
  if (descriptor < 0 || ct_elf_read(descriptor, ident, sizeof(ident), 0) != 0 ||
      memcmp(ident, ELFMAG, SELFMAG) != 0 || ident[EI_VERSION] != EV_CURRENT) {
    if (descriptor >= 0) (void)close(descriptor);
    return 1;
  }
  if (ident[EI_CLASS] == ELFCLASS64) {
    Elf64_Ehdr header;
    if (ct_elf_read(descriptor, &header, sizeof(header), 0) == 0) {
      *machine = header.e_machine;
      result = 0;
    }
  } else if (ident[EI_CLASS] == ELFCLASS32) {
    Elf32_Ehdr header;
    if (ct_elf_read(descriptor, &header, sizeof(header), 0) == 0) {
      *machine = header.e_machine;
      result = 0;
    }
  }
  if (close(descriptor) != 0) return 1;
  if (result == 0) {
    *elf_class = ident[EI_CLASS];
    *data = ident[EI_DATA];
  }
  return result;
}

int ct_elf_machine_is_native(unsigned int machine)
{
  unsigned char elf_class, data;
  unsigned int native_machine;
  return ct_elf_native(&elf_class, &data, &native_machine) == 0 &&
         machine == native_machine;
}

static int ct_elf_interpreter(int descriptor, const struct stat *status,
                              uintmax_t offset, uintmax_t size,
                              struct ct_elf_info *info)
{
  if (size < 2U || size > sizeof(info->interpreter) ||
      offset + size < offset || offset + size > (uintmax_t)status->st_size ||
      ct_elf_read(descriptor, info->interpreter, (size_t)size,
                  (off_t)offset) != 0 ||
      info->interpreter[size - 1U] != '\0' || info->interpreter[0] != '/' ||
      strlen(info->interpreter) != (size_t)size - 1U ||
      info->kind == CT_ELF_DYNAMIC) return 1;
  info->kind = CT_ELF_DYNAMIC;
  return 0;
}

static int ct_elf_inspect64(int descriptor, const struct stat *status,
                            unsigned int native_machine,
                            struct ct_elf_info *info)
{
  Elf64_Ehdr header;
  Elf64_Phdr program;
  uint16_t index;
  bool saw_load = false;
  if (status->st_size < (off_t)sizeof(header) ||
      ct_elf_read(descriptor, &header, sizeof(header), 0) != 0 ||
      (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
      header.e_version != EV_CURRENT ||
      header.e_machine != native_machine || header.e_ehsize != sizeof(header) ||
      header.e_phentsize != sizeof(program) || header.e_phnum == 0U ||
      (uintmax_t)header.e_phoff +
              (uintmax_t)header.e_phnum * sizeof(program) <
          (uintmax_t)header.e_phoff ||
      (uintmax_t)header.e_phoff +
              (uintmax_t)header.e_phnum * sizeof(program) >
          (uintmax_t)status->st_size) return 1;
  info->machine = header.e_machine;
  for (index = 0U; index < header.e_phnum; ++index) {
    if (ct_elf_read(descriptor, &program, sizeof(program),
                    (off_t)header.e_phoff +
                        (off_t)index * (off_t)sizeof(program)) != 0) return 1;
    if ((uintmax_t)program.p_offset + (uintmax_t)program.p_filesz <
            (uintmax_t)program.p_offset ||
        (uintmax_t)program.p_offset + (uintmax_t)program.p_filesz >
            (uintmax_t)status->st_size) return 1;
    if (program.p_type == PT_LOAD) {
      if (program.p_filesz > program.p_memsz) return 1;
      saw_load = true;
    }
    if (program.p_type == PT_INTERP &&
        ct_elf_interpreter(descriptor, status, program.p_offset,
                           program.p_filesz, info) != 0) return 1;
  }
  return saw_load ? 0 : 1;
}

static int ct_elf_inspect32(int descriptor, const struct stat *status,
                            unsigned int native_machine,
                            struct ct_elf_info *info)
{
  Elf32_Ehdr header;
  Elf32_Phdr program;
  uint16_t index;
  bool saw_load = false;
  if (status->st_size < (off_t)sizeof(header) ||
      ct_elf_read(descriptor, &header, sizeof(header), 0) != 0 ||
      (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
      header.e_version != EV_CURRENT ||
      header.e_machine != native_machine || header.e_ehsize != sizeof(header) ||
      header.e_phentsize != sizeof(program) || header.e_phnum == 0U ||
      (uintmax_t)header.e_phoff +
              (uintmax_t)header.e_phnum * sizeof(program) <
          (uintmax_t)header.e_phoff ||
      (uintmax_t)header.e_phoff +
              (uintmax_t)header.e_phnum * sizeof(program) >
          (uintmax_t)status->st_size) return 1;
  info->machine = header.e_machine;
  for (index = 0U; index < header.e_phnum; ++index) {
    if (ct_elf_read(descriptor, &program, sizeof(program),
                    (off_t)header.e_phoff +
                        (off_t)index * (off_t)sizeof(program)) != 0) return 1;
    if ((uintmax_t)program.p_offset + (uintmax_t)program.p_filesz <
            (uintmax_t)program.p_offset ||
        (uintmax_t)program.p_offset + (uintmax_t)program.p_filesz >
            (uintmax_t)status->st_size) return 1;
    if (program.p_type == PT_LOAD) {
      if (program.p_filesz > program.p_memsz) return 1;
      saw_load = true;
    }
    if (program.p_type == PT_INTERP &&
        ct_elf_interpreter(descriptor, status, program.p_offset,
                           program.p_filesz, info) != 0) return 1;
  }
  return saw_load ? 0 : 1;
}

int ct_elf_inspect(int descriptor, struct ct_elf_info *info)
{
  unsigned char ident[EI_NIDENT], native_class, native_data;
  unsigned int native_machine;
  struct stat status, final_status;
  int result;
  if (descriptor < 0 || info == NULL || fstat(descriptor, &status) != 0 ||
      !S_ISREG(status.st_mode) ||
      ct_elf_read(descriptor, ident, sizeof(ident), 0) != 0 ||
      memcmp(ident, ELFMAG, SELFMAG) != 0 ||
      ident[EI_VERSION] != EV_CURRENT ||
      ct_elf_native(&native_class, &native_data, &native_machine) != 0 ||
      ident[EI_CLASS] != native_class || ident[EI_DATA] != native_data) return 1;
  memset(info, 0, sizeof(*info));
  info->kind = CT_ELF_STATIC;
  result = ident[EI_CLASS] == ELFCLASS64
               ? ct_elf_inspect64(descriptor, &status, native_machine, info)
               : ident[EI_CLASS] == ELFCLASS32
                     ? ct_elf_inspect32(descriptor, &status, native_machine,
                                        info)
                     : 1;
  if (result != 0 || fstat(descriptor, &final_status) != 0 ||
      final_status.st_dev != status.st_dev ||
      final_status.st_ino != status.st_ino ||
      final_status.st_size != status.st_size ||
      final_status.st_mtim.tv_sec != status.st_mtim.tv_sec ||
      final_status.st_mtim.tv_nsec != status.st_mtim.tv_nsec) return 1;
  return 0;
}

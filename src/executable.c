/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#define _GNU_SOURCE
#include "executable.h"

#include "path_map.h"

#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CT_EXECUTABLE_MAX_SYMLINKS 40U

static int ct_executable_normalize(const char *path,
                                   char normalized[CT_HOST_PATH_MAX])
{
  const char *cursor = path;
  size_t used = 1U;
  if (path == NULL || path[0] != '/') return 1;
  normalized[0] = '/';
  normalized[1] = '\0';
  while (*cursor != '\0') {
    const char *end;
    size_t length;
    while (*cursor == '/') ++cursor;
    if (*cursor == '\0') break;
    end = strchr(cursor, '/');
    length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
    if (length == 1U && cursor[0] == '.') {
      cursor += length;
      continue;
    }
    if (length == 2U && cursor[0] == '.' && cursor[1] == '.') {
      if (used > 1U) {
        while (used > 1U && normalized[used - 1U] != '/') --used;
        if (used > 1U) --used;
        normalized[used] = '\0';
      }
      cursor += length;
      continue;
    }
    if (used > 1U) {
      if (used + 1U >= CT_HOST_PATH_MAX) return 1;
      normalized[used++] = '/';
    }
    if (length >= CT_HOST_PATH_MAX - used) return 1;
    memcpy(normalized + used, cursor, length);
    used += length;
    normalized[used] = '\0';
    cursor += length;
  }
  return 0;
}

static enum ct_executable_status ct_executable_visible(
    const struct ct_path_map *map, const char *target,
    char visible[CT_HOST_PATH_MAX])
{
  char current[CT_HOST_PATH_MAX];
  size_t followed = 0U;
  if (ct_host_copy_bounded(current, sizeof(current), target) != 0) {
    return CT_EXECUTABLE_INCOMPATIBLE;
  }
  for (;;) {
    const char *cursor = current + 1;
    if (*cursor == '\0') {
      return ct_path_map_visible(map, current, visible) == 0
                 ? CT_EXECUTABLE_OK
                 : CT_EXECUTABLE_INCOMPATIBLE;
    }
    while (*cursor != '\0') {
      const char *end = strchr(cursor, '/');
      const size_t prefix_length =
          end == NULL ? strlen(current) : (size_t)(end - current);
      char prefix[CT_HOST_PATH_MAX];
      char component_visible[CT_HOST_PATH_MAX];
      struct stat status;
      if (prefix_length >= sizeof(prefix)) return CT_EXECUTABLE_INCOMPATIBLE;
      memcpy(prefix, current, prefix_length);
      prefix[prefix_length] = '\0';
      if (ct_path_map_visible(map, prefix, component_visible) != 0) {
        return CT_EXECUTABLE_INCOMPATIBLE;
      }
      if (lstat(component_visible, &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return CT_EXECUTABLE_NOT_FOUND;
        return errno == EACCES || errno == EPERM ? CT_EXECUTABLE_INACCESSIBLE
                                                  : CT_EXECUTABLE_IO;
      }
      if (S_ISLNK(status.st_mode)) {
        char link[CT_HOST_PATH_MAX + 1U];
        char joined[CT_HOST_PATH_MAX * 2U];
        char parent[CT_HOST_PATH_MAX];
        const char *suffix = end == NULL ? "" : end;
        ssize_t length;
        int rendered;
        if (++followed > CT_EXECUTABLE_MAX_SYMLINKS) {
          return CT_EXECUTABLE_INCOMPATIBLE;
        }
        length = readlink(component_visible, link, CT_HOST_PATH_MAX);
        if (length < 0) return CT_EXECUTABLE_IO;
        if ((size_t)length >= CT_HOST_PATH_MAX) {
          return CT_EXECUTABLE_INCOMPATIBLE;
        }
        link[length] = '\0';
        if (link[0] == '/') {
          rendered = snprintf(joined, sizeof(joined), "%s%s", link, suffix);
        } else {
          const char *slash = strrchr(prefix, '/');
          const size_t parent_length = slash == prefix ? 1U : (size_t)(slash - prefix);
          memcpy(parent, prefix, parent_length);
          parent[parent_length] = '\0';
          rendered = snprintf(joined, sizeof(joined), "%s%s%s%s", parent,
                              strcmp(parent, "/") == 0 ? "" : "/", link,
                              suffix);
        }
        if (rendered < 0 || (size_t)rendered >= sizeof(joined) ||
            ct_executable_normalize(joined, current) != 0) {
          return CT_EXECUTABLE_INCOMPATIBLE;
        }
        break;
      }
      if (end == NULL) {
        return ct_host_copy_bounded(visible, CT_HOST_PATH_MAX,
                                    component_visible) == 0
                   ? CT_EXECUTABLE_OK
                   : CT_EXECUTABLE_INCOMPATIBLE;
      }
      cursor = end + 1;
    }
  }
}

static enum ct_executable_status ct_executable_open(
    const struct ct_path_map *map, const char *target, int *descriptor,
    char visible[CT_HOST_PATH_MAX])
{
  enum ct_executable_status result;
  char resolved_visible[CT_HOST_PATH_MAX];
  struct stat status;
  result = ct_executable_visible(map, target, resolved_visible);
  if (result != CT_EXECUTABLE_OK) return result;
  if (ct_path_map_visible(map, target, visible) != 0) {
    return CT_EXECUTABLE_INCOMPATIBLE;
  }
  *descriptor = open(resolved_visible, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (*descriptor < 0) {
    if (errno == ENOENT || errno == ENOTDIR) return CT_EXECUTABLE_NOT_FOUND;
    return errno == EACCES || errno == EPERM ? CT_EXECUTABLE_INACCESSIBLE
                                              : CT_EXECUTABLE_IO;
  }
  if (fstat(*descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    (void)close(*descriptor);
    *descriptor = -1;
    return CT_EXECUTABLE_INCOMPATIBLE;
  }
  if ((status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0 ||
      access(resolved_visible, X_OK) != 0) {
    (void)close(*descriptor);
    *descriptor = -1;
    return CT_EXECUTABLE_INACCESSIBLE;
  }
  {
    struct stat final;
    if (stat(resolved_visible, &final) != 0 || final.st_dev != status.st_dev ||
        final.st_ino != status.st_ino) {
      (void)close(*descriptor);
      *descriptor = -1;
      return CT_EXECUTABLE_IO;
    }
  }
  return CT_EXECUTABLE_OK;
}

static enum ct_executable_status ct_executable_find(
    const struct ct_host_profile *profile, const struct ct_path_map *map,
    const char *path_override, const char *command, int *descriptor,
    char target[CT_HOST_PATH_MAX], char visible[CT_HOST_PATH_MAX])
{
  size_t index;
  if (strchr(command, '/') != NULL) {
    if (ct_host_path_validate(command) != 0 ||
        ct_host_copy_bounded(target, CT_HOST_PATH_MAX, command) != 0) {
      return CT_EXECUTABLE_INCOMPATIBLE;
    }
    return ct_executable_open(map, target, descriptor, visible);
  }
  if (path_override != NULL) {
    const char *cursor = path_override;
    while (*cursor != '\0') {
      const char *end = strchr(cursor, ':');
      const size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
      char directory[CT_HOST_PATH_MAX];
      enum ct_executable_status result;
      if (length == 0U || length >= sizeof(directory)) {
        return CT_EXECUTABLE_INCOMPATIBLE;
      }
      memcpy(directory, cursor, length);
      directory[length] = '\0';
      if (ct_host_path_validate(directory) != 0 ||
          snprintf(target, CT_HOST_PATH_MAX, "%s/%s", directory, command) >=
              (int)CT_HOST_PATH_MAX) return CT_EXECUTABLE_INCOMPATIBLE;
      result = ct_executable_open(map, target, descriptor, visible);
      if (result == CT_EXECUTABLE_OK) return result;
      if (result != CT_EXECUTABLE_NOT_FOUND) return result;
      if (end == NULL) break;
      cursor = end + 1;
    }
    return CT_EXECUTABLE_NOT_FOUND;
  }
  for (index = 0U; index < profile->path_count; ++index) {
    enum ct_executable_status result;
    if (snprintf(target, CT_HOST_PATH_MAX, "%s/%s", profile->path[index],
                 command) >= (int)CT_HOST_PATH_MAX) {
      return CT_EXECUTABLE_INCOMPATIBLE;
    }
    result = ct_executable_open(map, target, descriptor, visible);
    if (result == CT_EXECUTABLE_OK) return result;
    if (result != CT_EXECUTABLE_NOT_FOUND) return result;
  }
  return CT_EXECUTABLE_NOT_FOUND;
}

struct ct_executable_env_command {
  char command[CT_HOST_PATH_MAX];
  char path[CT_HOST_PATH_MAX];
  char arguments[CT_EXECUTABLE_ENV_ARGUMENT_MAX][256];
  size_t argument_count;
  int path_override;
};

static int ct_executable_env_token(const char **position,
                                   char token[CT_HOST_PATH_MAX])
{
  const char *cursor = *position;
  size_t used = 0U;
  char quote = '\0';
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  if (*cursor == '\0') {
    *position = cursor;
    return 1;
  }
  while (*cursor != '\0' &&
         (quote != '\0' || (*cursor != ' ' && *cursor != '\t'))) {
    const char byte = *cursor++;
    if (quote == '\0' && (byte == '\'' || byte == '"')) {
      quote = byte;
      continue;
    }
    if (quote != '\0' && byte == quote) {
      quote = '\0';
      continue;
    }
    if (byte == '\\' || byte == '$' || used + 1U >= CT_HOST_PATH_MAX) return 1;
    token[used++] = byte;
  }
  if (quote != '\0' || used == 0U) return 1;
  token[used] = '\0';
  *position = cursor;
  return 0;
}

static int ct_executable_env_assignment(const char *token, const char **value)
{
  const char *equals = strchr(token, '=');
  const char *cursor;
  if (equals == NULL || equals == token ||
      !((token[0] >= 'A' && token[0] <= 'Z') ||
        (token[0] >= 'a' && token[0] <= 'z') || token[0] == '_')) return 0;
  for (cursor = token + 1; cursor < equals; ++cursor) {
    if (!((*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= 'a' && *cursor <= 'z') ||
          (*cursor >= '0' && *cursor <= '9') || *cursor == '_')) return 0;
  }
  *value = equals + 1;
  return 1;
}

static int ct_executable_env_parse(const char *argument,
                                   struct ct_executable_env_command *result)
{
  const char *cursor = argument;
  int options = 1;
  memset(result, 0, sizeof(*result));
  if (strncmp(cursor, "-S", 2U) != 0 ||
      (cursor[2] != ' ' && cursor[2] != '\t')) {
    const char *assignment;
    return cursor[0] == '-' || strchr(cursor, ' ') != NULL ||
                   strchr(cursor, '\t') != NULL ||
                   ct_executable_env_assignment(cursor, &assignment) != 0 ||
                   ct_host_copy_bounded(result->command,
                                        sizeof(result->command), cursor) != 0
               ? 1
               : 0;
  }
  cursor += 2;
  for (;;) {
    char token[CT_HOST_PATH_MAX];
    const char *assignment;
    if (ct_executable_env_token(&cursor, token) != 0) return 1;
    if (options && strcmp(token, "--") == 0) {
      options = 0;
      continue;
    }
    if (options && (strcmp(token, "-i") == 0 ||
                    strcmp(token, "--ignore-environment") == 0)) {
      if (ct_host_copy_bounded(result->path, sizeof(result->path),
                               "/bin:/usr/bin") != 0) return 1;
      result->path_override = 1;
      continue;
    }
    if (options && (strcmp(token, "-u") == 0 ||
                    strcmp(token, "--unset") == 0 ||
                    strcmp(token, "-C") == 0 ||
                    strcmp(token, "--chdir") == 0 ||
                    strcmp(token, "-a") == 0 ||
                    strcmp(token, "--argv0") == 0)) {
      char operand[CT_HOST_PATH_MAX];
      if (ct_executable_env_token(&cursor, operand) != 0) return 1;
      if ((strcmp(token, "-u") == 0 || strcmp(token, "--unset") == 0) &&
          strcmp(operand, "PATH") == 0) {
        if (ct_host_copy_bounded(result->path, sizeof(result->path),
                                 "/bin:/usr/bin") != 0) return 1;
        result->path_override = 1;
      }
      continue;
    }
    if (options && strncmp(token, "--unset=", 8U) == 0) {
      if (strcmp(token + 8U, "PATH") == 0) {
        if (ct_host_copy_bounded(result->path, sizeof(result->path),
                                 "/bin:/usr/bin") != 0) return 1;
        result->path_override = 1;
      }
      continue;
    }
    if (options && (strncmp(token, "--chdir=", 8U) == 0 ||
                    strncmp(token, "--argv0=", 8U) == 0)) continue;
    if (ct_executable_env_assignment(token, &assignment) != 0) {
      if ((size_t)(assignment - token) == 5U &&
          strncmp(token, "PATH=", 5U) == 0) {
        if (ct_host_copy_bounded(result->path, sizeof(result->path),
                                 assignment) != 0 || result->path[0] == '\0') {
          return 1;
        }
        result->path_override = 1;
      }
      continue;
    }
    if (token[0] == '-' ||
        ct_host_copy_bounded(result->command, sizeof(result->command),
                              token) != 0) return 1;
    while (*cursor != '\0') {
      if (result->argument_count == CT_EXECUTABLE_ENV_ARGUMENT_MAX ||
          ct_executable_env_token(&cursor, token) != 0 ||
          ct_host_copy_bounded(result->arguments[result->argument_count],
                               sizeof(result->arguments[0]), token) != 0) {
        return 1;
      }
      ++result->argument_count;
    }
    return 0;
  }
}

static int ct_executable_duplicate(int descriptor)
{
  return descriptor < 0 ? -1 : fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
}

static enum ct_executable_status ct_executable_loader(
    const struct ct_path_map *map, const struct ct_executable_stage *stage,
    struct ct_executable *executable)
{
  enum ct_executable_status result;
  struct ct_elf_info loader_elf;
  if (stage == NULL || executable == NULL ||
      stage->elf.kind != CT_ELF_DYNAMIC ||
      ct_host_copy_bounded(executable->loader_target_path,
                           sizeof(executable->loader_target_path),
                           stage->elf.interpreter) != 0) {
    return CT_EXECUTABLE_INCOMPATIBLE;
  }
  result = ct_executable_open(map, executable->loader_target_path,
                              &executable->loader_descriptor,
                              executable->loader_visible_path);
  if (result != CT_EXECUTABLE_OK ||
      ct_elf_inspect(executable->loader_descriptor, &loader_elf) != 0) {
    if (executable->loader_descriptor >= 0) {
      (void)close(executable->loader_descriptor);
      executable->loader_descriptor = -1;
    }
    return CT_EXECUTABLE_LOADER;
  }
  return CT_EXECUTABLE_OK;
}

enum ct_executable_status ct_executable_resolve(
    const struct ct_host_profile *profile, const struct ct_path_map *map,
    const char *command,
    struct ct_executable *executable)
{
  enum ct_executable_status result;
  char current[CT_HOST_PATH_MAX], visible[CT_HOST_PATH_MAX];
  char pending_env_command[CT_HOST_PATH_MAX] = "";
  char pending_env_path[CT_HOST_PATH_MAX] = "";
  int pending_env_path_override = 0;
  int descriptor = -1;
  size_t depth;
  if (profile == NULL || map == NULL || command == NULL || command[0] == '\0' ||
      executable == NULL) return CT_EXECUTABLE_INCOMPATIBLE;
  memset(executable, 0, sizeof(*executable));
  executable->descriptor = -1;
  executable->loader_descriptor = -1;
  for (depth = 0U; depth < CT_EXECUTABLE_MAX_STAGES; ++depth) {
    executable->stages[depth].descriptor = -1;
  }
  result = ct_executable_find(profile, map, NULL, command, &descriptor, current,
                              visible);
  if (result != CT_EXECUTABLE_OK) return result;
  executable->descriptor = descriptor;
  if (ct_host_copy_bounded(executable->target_path,
                           sizeof(executable->target_path), current) != 0 ||
      ct_host_copy_bounded(executable->visible_path,
                           sizeof(executable->visible_path), visible) != 0) goto invalid;
  for (depth = 0U; depth < CT_EXECUTABLE_MAX_STAGES; ++depth) {
    struct ct_executable_stage *stage = &executable->stages[depth];
    size_t previous;
    if (ct_host_copy_bounded(stage->target_path, sizeof(stage->target_path),
                             current) != 0 ||
        ct_host_copy_bounded(stage->visible_path, sizeof(stage->visible_path),
                             visible) != 0 ||
        (stage->descriptor = ct_executable_duplicate(descriptor)) < 0) {
      goto invalid;
    }
    for (previous = 0U; previous < depth; ++previous) {
      if (strcmp(executable->stages[previous].target_path, current) == 0) {
        result = CT_EXECUTABLE_SHEBANG;
        goto failed;
      }
    }
    executable->stage_count = depth + 1U;
    if (ct_elf_inspect(stage->descriptor, &stage->elf) == 0) {
      if (pending_env_command[0] != '\0') {
        if (descriptor != executable->descriptor) (void)close(descriptor);
        descriptor = -1;
        result = ct_executable_find(
            profile, map,
            pending_env_path_override != 0 ? pending_env_path : NULL,
            pending_env_command, &descriptor, current, visible);
        pending_env_command[0] = '\0';
        pending_env_path[0] = '\0';
        pending_env_path_override = 0;
        if (result != CT_EXECUTABLE_OK) {
          result = CT_EXECUTABLE_SHEBANG;
          goto failed;
        }
        continue;
      }
      result = ct_executable_loader(map, stage, executable);
      if (result != CT_EXECUTABLE_OK) {
        goto failed;
      }
      if (descriptor != executable->descriptor) (void)close(descriptor);
      return CT_EXECUTABLE_OK;
    }
    if (depth + 1U == CT_EXECUTABLE_MAX_STAGES ||
        ct_shebang_parse(descriptor, &stage->shebang) != 0) {
      result = CT_EXECUTABLE_SHEBANG;
      goto failed;
    }
    stage->is_shebang = 1;
    if (descriptor != executable->descriptor) (void)close(descriptor);
    descriptor = -1;
    if (stage->shebang.uses_env != 0) {
      struct ct_executable_env_command env;
      if (ct_executable_env_parse(stage->shebang.argument, &env) != 0 ||
          ct_host_copy_bounded(pending_env_command,
                               sizeof(pending_env_command), env.command) != 0 ||
          env.argument_count > CT_EXECUTABLE_ENV_ARGUMENT_MAX ||
          (env.path_override != 0 &&
            ct_host_copy_bounded(pending_env_path, sizeof(pending_env_path),
                                 env.path) != 0)) {
        result = CT_EXECUTABLE_SHEBANG;
        goto failed;
      }
      stage->env_argument_count = env.argument_count;
      for (previous = 0U; previous < env.argument_count; ++previous) {
        if (ct_host_copy_bounded(stage->env_arguments[previous],
                                 sizeof(stage->env_arguments[previous]),
                                 env.arguments[previous]) != 0) {
          result = CT_EXECUTABLE_SHEBANG;
          goto failed;
        }
      }
      pending_env_path_override = env.path_override;
      result = ct_executable_find(profile, map, NULL,
                                  stage->shebang.interpreter, &descriptor,
                                  current, visible);
    } else {
      result = ct_executable_find(profile, map, NULL,
                                  stage->shebang.interpreter, &descriptor,
                                  current, visible);
    }
    if (result != CT_EXECUTABLE_OK) {
      result = CT_EXECUTABLE_SHEBANG;
      goto failed;
    }
  }
invalid:
  result = CT_EXECUTABLE_INCOMPATIBLE;
failed:
  if (descriptor >= 0 && descriptor != executable->descriptor) {
    (void)close(descriptor);
  }
  ct_executable_close(executable);
  return result;
}

void ct_executable_close(struct ct_executable *executable)
{
  size_t index;
  if (executable == NULL) return;
  if (executable->descriptor >= 0) {
    (void)close(executable->descriptor);
    executable->descriptor = -1;
  }
  if (executable->loader_descriptor >= 0) {
    (void)close(executable->loader_descriptor);
    executable->loader_descriptor = -1;
  }
  for (index = 0U; index < CT_EXECUTABLE_MAX_STAGES; ++index) {
    if (executable->stages[index].descriptor >= 0) {
      (void)close(executable->stages[index].descriptor);
      executable->stages[index].descriptor = -1;
    }
  }
}

int ct_executable_trampoline_open(void)
{
  char path[CT_HOST_PATH_MAX];
  struct stat running;
  struct stat selected;
  struct stat final;
  int running_descriptor = -1;
  int selected_descriptor = -1;
  int descriptor_flags;
  const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1U);

  if (length <= 0 || (size_t)length >= sizeof(path) - 1U) return -1;
  path[length] = '\0';
  if (path[0] != '/') return -1;
  running_descriptor = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  selected_descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (running_descriptor < 0 || selected_descriptor < 0 ||
      fstat(running_descriptor, &running) != 0 ||
      fstat(selected_descriptor, &selected) != 0 ||
      stat(path, &final) != 0 || !S_ISREG(running.st_mode) ||
      !S_ISREG(selected.st_mode) || !S_ISREG(final.st_mode) ||
      running.st_dev != selected.st_dev || running.st_ino != selected.st_ino ||
      selected.st_dev != final.st_dev || selected.st_ino != final.st_ino ||
      (descriptor_flags = fcntl(selected_descriptor, F_GETFD)) < 0 ||
      fcntl(selected_descriptor, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0) {
    if (running_descriptor >= 0) (void)close(running_descriptor);
    if (selected_descriptor >= 0) (void)close(selected_descriptor);
    return -1;
  }
  if (close(running_descriptor) != 0) {
    (void)close(selected_descriptor);
    return -1;
  }
  return selected_descriptor;
}

static int ct_executable_stable(int descriptor, struct stat *before)
{
  struct stat after;
  return fstat(descriptor, &after) != 0 || after.st_dev != before->st_dev || after.st_ino != before->st_ino || after.st_size != before->st_size || after.st_mtim.tv_sec != before->st_mtim.tv_sec || after.st_mtim.tv_nsec != before->st_mtim.tv_nsec;
}

int ct_executable_admit_trampoline(int executable_descriptor, int control_descriptor, const char *build_identity)
{
  struct stat executable, current, control;
  struct ct_elf_info elf;
  char record[512];
  const char prefix[] = "container-tools-control-v1\n";
  const size_t identity_length = build_identity == NULL ? 0U : strlen(build_identity);
  const size_t expected = sizeof(prefix) - 1U + identity_length + 1U;
  ssize_t received;
  if (executable_descriptor < 0 || control_descriptor < 0 ||
      build_identity == NULL || identity_length == 0U ||
      expected >= sizeof(record) ||
      fstat(executable_descriptor, &executable) != 0 ||
      fstat(control_descriptor, &control) != 0 ||
      stat("/proc/self/exe", &current) != 0 ||
      executable.st_dev != current.st_dev || executable.st_ino != current.st_ino ||
      ct_elf_inspect(executable_descriptor, &elf) != 0 ||
      elf.kind != CT_ELF_STATIC ||
      !S_ISREG(executable.st_mode) || !S_ISREG(control.st_mode) ||
      control.st_size != (off_t)expected) return 1;
  received = pread(control_descriptor, record, sizeof(record), 0);
  if (received != (ssize_t)expected ||
      memcmp(record, prefix, sizeof(prefix) - 1U) != 0 ||
      memcmp(record + sizeof(prefix) - 1U, build_identity, identity_length) != 0 ||
      record[expected - 1U] != '\n' ||
      ct_executable_stable(executable_descriptor, &executable) != 0 ||
      ct_executable_stable(control_descriptor, &control) != 0) return 1;
  return 0;
}

int ct_executable_control_open(const char *build_identity)
{
  const char prefix[] = "container-tools-control-v1\n";
  char record[512];
  size_t length;
  size_t written = 0U;
  int descriptor;
  const int rendered = build_identity == NULL
                           ? -1
                           : snprintf(record, sizeof(record), "%s%s\n", prefix,
                                      build_identity);
  if (rendered < 0 || (size_t)rendered >= sizeof(record)) return -1;
  length = (size_t)rendered;
#ifdef SYS_memfd_create
  descriptor = (int)syscall(SYS_memfd_create, "container-tools-control", 0U);
#else
  descriptor = -1;
#endif
  if (descriptor < 0) return -1;
  while (written < length) {
    const ssize_t result = write(descriptor, record + written, length - written);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) {
      (void)close(descriptor);
      return -1;
    }
    written += (size_t)result;
  }
  if (lseek(descriptor, 0, SEEK_SET) < 0) {
    (void)close(descriptor);
    return -1;
  }
  return descriptor;
}

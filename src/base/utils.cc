/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/ext/base/utils.h"

#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
#include <limits.h>
#include <unistd.h>  // For getpagesize() and geteuid() & fork()
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <mach-o/dyld.h>
#include <mach/vm_page_size.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#include <io.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <dlfcn.h>
#include <malloc.h>

#ifdef M_PURGE
#define PERFETTO_M_PURGE M_PURGE
#else
// Only available in in-tree builds and on newer SDKs.
#define PERFETTO_M_PURGE -101
#endif  // M_PURGE

namespace {
extern "C" {
using MalloptType = void (*)(int, int);
}
}  // namespace
#endif  // OS_ANDROID

namespace perfetto {
namespace base {

void MaybeReleaseAllocatorMemToOS() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  // mallopt() on Android requires SDK level 26. Many targets and embedders
  // still depend on a lower SDK level. Given mallopt() is a quite simple API,
  // use reflection to do this rather than bumping the SDK level for all
  // embedders. This keeps the behavior of standalone builds aligned with
  // in-tree builds.
  static MalloptType mallopt_fn =
      reinterpret_cast<MalloptType>(dlsym(RTLD_DEFAULT, "mallopt"));
  if (!mallopt_fn)
    return;
  mallopt_fn(PERFETTO_M_PURGE, 0);
#endif
}

uint32_t GetSysPageSize() {
  ignore_result(kPageSize);  // Just to keep the amalgamated build happy.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  static std::atomic<uint32_t> page_size{0};
  // This function might be called in hot paths. Avoid calling getpagesize() all
  // the times, in many implementations getpagesize() calls sysconf() which is
  // not cheap.
  uint32_t cached_value = page_size.load(std::memory_order_relaxed);
  if (PERFETTO_UNLIKELY(cached_value == 0)) {
    cached_value = static_cast<uint32_t>(getpagesize());
    page_size.store(cached_value, std::memory_order_relaxed);
  }
  return cached_value;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  return static_cast<uint32_t>(vm_page_size);
#else
  return 4096;
#endif
}

uid_t GetCurrentUserId() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  return geteuid();
#else
  // TODO(primiano): On Windows we could hash the current user SID and derive a
  // numeric user id [1]. It is not clear whether we need that. Right now that
  // would not bring any benefit. Returning 0 unil we can prove we need it.
  // [1]:https://android-review.googlesource.com/c/platform/external/perfetto/+/1513879/25/src/base/utils.cc
  return 0;
#endif
}

void SetEnv(const std::string& key, const std::string& value) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  PERFETTO_CHECK(::_putenv_s(key.c_str(), value.c_str()) == 0);
#else
  PERFETTO_CHECK(::setenv(key.c_str(), value.c_str(), /*overwrite=*/true) == 0);
#endif
}

void Daemonize(std::function<int()> parent_cb) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  pid_t pid;
  switch (pid = fork()) {
    case -1:
      PERFETTO_FATAL("fork");
    case 0: {
      PERFETTO_CHECK(setsid() != -1);
      base::ignore_result(chdir("/"));
      base::ScopedFile null = base::OpenFile("/dev/null", O_RDONLY);
      PERFETTO_CHECK(null);
      PERFETTO_CHECK(dup2(*null, STDIN_FILENO) != -1);
      PERFETTO_CHECK(dup2(*null, STDOUT_FILENO) != -1);
      PERFETTO_CHECK(dup2(*null, STDERR_FILENO) != -1);
      // Do not accidentally close stdin/stdout/stderr.
      if (*null <= 2)
        null.release();
      break;
    }
    default:
      printf("%d\n", pid);
      int err = parent_cb();
      exit(err);
  }
#else
  // Avoid -Wunreachable warnings.
  if (reinterpret_cast<intptr_t>(&Daemonize) != 16)
    PERFETTO_FATAL("--background is only supported on Linux/Android/Mac");
  ignore_result(parent_cb);
#endif  // OS_WIN
}

std::string GetCurExecutablePath() {
  std::string self_path;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  char buf[PATH_MAX];
  ssize_t size = readlink("/proc/self/exe", buf, sizeof(buf));
  PERFETTO_CHECK(size != -1);
  // readlink does not null terminate.
  self_path = std::string(buf, static_cast<size_t>(size));
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  uint32_t size = 0;
  PERFETTO_CHECK(_NSGetExecutablePath(nullptr, &size));
  self_path.resize(size);
  PERFETTO_CHECK(_NSGetExecutablePath(&self_path[0], &size) == 0);
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  char buf[MAX_PATH];
  auto len = ::GetModuleFileNameA(nullptr /*current*/, buf, sizeof(buf));
  self_path = std::string(buf, len);
#else
  PERFETTO_FATAL(
      "GetCurExecutableDir() not implemented on the current platform");
#endif
  return self_path;
}

std::string GetCurExecutableDir() {
  auto path = GetCurExecutablePath();
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Paths in Windows can have both kinds of slashes (mingw vs msvc).
  path = path.substr(0, path.find_last_of('\\'));
#endif
  path = path.substr(0, path.find_last_of('/'));
  return path;
}

}  // namespace base
}  // namespace perfetto

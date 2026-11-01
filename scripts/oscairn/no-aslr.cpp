// SPDX-License-Identifier: BSD-2-Clause
//
// Runs a program with address space layout randomization off (macOS), so that hash tables
// seeded from addresses (abseil's) iterate in the same order on every run.
// usage: no-aslr <program> [args...]
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, 0x100); // _POSIX_SPAWN_DISABLE_ASLR
    pid_t pid;
    if (posix_spawn(&pid, argv[1], nullptr, &attr, argv + 1, environ) != 0)
        return 127;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/*
 * htmm_ctl - start/stop MEMTIS access sampling (the ksamplingd kthread).
 *
 * Writing "enabled" to a cgroup's memory.htmm_enabled only starts kmigraterd
 * (demotion/promotion daemon). The PEBS sampler that produces the hotness the
 * migrater needs is started separately, by the htmm_start syscall. Without it
 * there are no samples, so nothing is ranked hot and nothing is promoted.
 *
 * We sample in nopid mode (pid = -1): membench's corun is one process with two
 * pinned worker threads, and a pid-attached perf event (no inherit) would only
 * see the main thread. -1 samples every task on the watched cores; the
 * memcg->htmm_enabled flag still scopes hotness accounting to our cgroup.
 *
 *   htmm_ctl start   -> htmm_start(-1, 0)
 *   htmm_ctl stop    -> htmm_end(-1)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_htmm_start 449
#define SYS_htmm_end   450

int main(int argc, char **argv)
{
    long ret;

    if (argc < 2) {
        fprintf(stderr, "usage: %s start|stop\n", argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "start")) {
        ret = syscall(SYS_htmm_start, -1, 0);
        if (ret) {
            perror("htmm_start");
            return 1;
        }
        return 0;
    }
    if (!strcmp(argv[1], "stop")) {
        ret = syscall(SYS_htmm_end, -1);
        if (ret) {
            perror("htmm_end");
            return 1;
        }
        return 0;
    }

    fprintf(stderr, "%s: unknown command '%s' (want start|stop)\n", argv[0], argv[1]);
    return 2;
}

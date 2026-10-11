/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Regression test for the Linux ProcessStatisticsReader's CPU-time accounting.
//
// ProcessModel derives a thread's CPU% as (thread_time_delta /
// total_time_scheduled_delta) * 100 and sums it per CPU, so
// total_time_scheduled must advance at ~wall-clock rate (one CPU's worth), not
// at ncpu * wall-clock. Reading /proc/stat naively made every percentage
// 1/ncpu of the truth (a busy core showed ~12% on an 8-core machine).

#include <AK/Time.h>
#include <LibCore/ProcessStatisticsReader.h>
#include <LibMain/Main.h>
#include <stdio.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments)
{
    auto before = TRY(Core::ProcessStatisticsReader::get_all(false));
    auto start = MonotonicTime::now();
    usleep(500 * 1000);
    auto after = TRY(Core::ProcessStatisticsReader::get_all(false));
    auto elapsed_ns = (MonotonicTime::now() - start).to_nanoseconds();

    if (elapsed_ns == 0) {
        printf("PROCSTAT: FAIL (no elapsed time)\n");
        return 1;
    }

    u64 const total_delta = after.total_time_scheduled - before.total_time_scheduled;
    double const ratio = static_cast<double>(total_delta) / static_cast<double>(elapsed_ns);

    printf("PROCSTAT: total_time_scheduled delta %llu ns over %llu ns -> ratio %.2f (want ~1.0; the ncpu bug gave ~ncpu)\n",
        static_cast<unsigned long long>(total_delta), static_cast<unsigned long long>(elapsed_ns), ratio);

    // Generous bounds: scheduler tick granularity. The old (buggy) scaling was
    // ~ncpu (e.g. 8), so this fails loudly if the total is not per-CPU.
    if (ratio < 0.75 || ratio > 1.25) {
        printf("PROCSTAT: FAIL\n");
        return 1;
    }

    printf("PROCSTAT: PASS\n");
    return 0;
}

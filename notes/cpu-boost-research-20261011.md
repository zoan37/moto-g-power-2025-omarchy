# CPU boost for scrolling: research (2026-10-11)

Desk research only: no phone access, nothing was loaded or written on the device. The question is how
stock Android on this phone keeps frame-critical threads fast during touch scrolling, and which parts can
be copied to plain Linux (bash PID 1, no cgroups mounted, schedutil, only the vendor_boot module set plus
the GPU set from `gpu-prepare`).

Abbreviations for citations:
- `K/` = `third_party/kernel/kernel-mtk` (`git describe`: `MMI-W1VKS36H.9-12-1`; the shipped modules'
  strings and symbols match the interfaces cited below, e.g. `turbo_pid`, `up_rate_limit_us`, `ux_task`).
- `M/` = `third_party/kernel/motorola-kernel-modules`. `G/` = `third_party/kernel/gki-src/common` (GKI 5.15
  that is running). `D/` = `artifacts/vegas-power-audit/stock-0.dts`.
- `L/` = `artifacts/vegas-linux-bringup/vendor_dlkm/lib/modules` (`modules.dep`, `modules.load`).
- `V/` = files read out of `artifacts/vegas-linux-bringup/super/vendor_b.img` (EROFS) with
  `third_party/erofs-utils/dump/dump.erofs --cat --path=/etc/... <img>`. Line numbers refer to those files.

**Not available locally:** the system partition. `artifacts/vegas-linux-bringup/super/` holds only
`metadata-head.img` plus the `vendor_b` and `vendor_dlkm_b` extents. AOSP's `/system/etc/task_profiles.json`
(defines `MaxPerformance` = join cpu `top-app` etc.), `/system/etc/cgroups.json` and
`/system/etc/init/hw/init.rc` (creates the `/dev/cpuctl` and `/dev/cpuset` groups) would need the
`system_b` extents from super (same liblp read plan as vendor_b). The vendor files are enough for the
values below.

## Summary

Android's scroll performance on this phone comes from cheap, generic knobs rather than from the MediaTek
modules:

1. `sched_pelt_multiplier = 4` at boot (`V/etc/init/hw/init.cgroup.rc:51`): PELT half-life 32 ms → 8 ms
   (`G/kernel/sched/pelt.c:477-516`, `pelt.h:80`), so a thread that starts working hard looks big (and
   migrates and raises frequency) four times sooner.
2. On touch (power HAL `INTERACTION`, held 1.5 s): little ≥ 1.5 GHz, big ≥ 1.7 GHz, top-app
   `cpu.uclamp.min` = 40 %, top-app `cpu.uclamp.latency_sensitive` = 1, DRAM at its top OPP
   (`V/etc/powerscntbl.xml:3-11`).
3. A cpuset layout that keeps background work off the A76s: top-app and foreground `0-7`,
   background and system-background `0-5` (`V/etc/init/hw/init.mt6835.rc:58-63`).
4. `sugov_ext` as governor (`init.cgroup.rc:49-50`), with tunables left at default.

FPSGO (frame-aware boosting) is the part we can't copy, because it needs SurfaceFlinger. Everything in
points 1–3 works in the stock GKI kernel with no vendor module.

## 1. Modules

`modules.load` positions: `thermal_interface` 54, `powerhal_cpu_ctrl` 79, `touch_boost` 80,
`mtk_ioctl_touch_boost` 81, `mtk_perf_common` 117, `task_turbo` 118, `cpufreq_sugov_ext` 119,
`scheduler` 120, `mtk_core_ctl` 121, `cpuqos_v3` 122, `mtk_fpsgo` 131, `fpsgo` 132, `mtk_perf_ioctl` 133.
`moto_sched` is not in `modules.load`. It is loaded by `V/etc/init/hw/init.mmi.overlay.rc:11` at early-init.

Restricted vendor hooks (`register_trace_android_rvh_*`) can't be unregistered (`K/drivers/misc/mediatek/task_turbo/task_turbo.c:1469`),
so none of the scheduler modules can be unloaded. Treat each load as one-way for that boot. A failure
in `init` after hooks are registered would leave dangling hooks, so try them on a boot with a watchdog,
like `gpu-prepare`.

### thermal_interface.ko
- Platform driver for `mediatek,therm_intf`, which maps the thermal CSRAM shared with the thermal firmware
  (`K/drivers/thermal/mediatek/thermal_interface.c:1945-1979`, DT node `D/:402-407`). It exports headroom,
  CPU temperature and `set_cpu_min_opp` (`:369-446`).
- Plain Linux: no scheduling effect on its own. It is required by `cpufreq_sugov_ext`, `scheduler`,
  `mtk_core_ctl`, `task_turbo`, `moto_sched` and the fpsgo modules (`L/modules.dep:38,59,107,174,175,186`).
- Its dependencies (gpueb, mtk_gpufreq_wrapper, mtk_gpu_hal, pbm/mdpm/throttling, tinysys ipi, mbox) are
  all in the GPU module set (`artifacts/vegas-linux-bringup/gpu-stock-probe/manifest.json`). Load it only
  on GPU boots, after `gpu-prepare`.

### cpufreq_sugov_ext.ko (governor `sugov_ext`)
Compared with stock schedutil:
- **Separate up and down rate limits.** Stock schedutil has one `rate_limit_us`. This adds
  `policyN/sugov_ext/up_rate_limit_us` and `down_rate_limit_us` (`K/drivers/misc/mediatek/sched/sugov/cpufreq_sugov_main.c:107-123,741-800,969-971`).
  Both default to `cpufreq_policy_transition_delay_us()` (`:963-964`), the same default as schedutil
  (`G/drivers/cpufreq/cpufreq.c:571-593`, capped at 10 ms). The power HAL can change them
  (`V/etc/powercontable.xml:171-193`), but no scenario in `powerscntbl.xml` does. A long down limit is the
  useful knob: it holds the frequency between frames.
- **Nonlinear util→frequency mapping.** `get_next_freq()` calls `mtk_map_util_freq()`
  (`cpufreq_sugov_main.c:172-191`). That function applies a 20 % margin (`util_scale` = 1024·100/80,
  `K/.../sched/sugov/nonlinear_opp_cap.c:907-930`, the same 1.25× as stock), then looks up an OPP in
  MediaTek's measured per-OPP capacity table instead of using the linear `f = 1.25·util/max·fmax`
  (`:988-1010`).
- There are per-gear `target_margin` and `turn_point` settings (defaults 20 and 0, `:812-813`; sysfs is
  in scheduler.ko, see below). The table is readable at `/proc/mtk_scheduler/pd_capacity_tbl` (`:815`).
  With `CONFIG_NONLINEAR_FREQ_CTL=y` (`K/arch/arm64/configs/mgk_64_k515_defconfig:439`) it also takes over
  the frequency-invariance scale (`cpufreq_sugov_main.c:1112-1128`).
- **uclamp:** `mtk_cpu_util()` applies rq uclamp the same way stock does (`:213-310`). It adds an
  "SBB" multiplier for flagged tasks or groups on busy CPUs (`:261-275`; set via the scheduler.ko ioctl).
  It also passes the rq `uclamp.min` to thermal as a minimum OPP (`:473-525`).
- **util_est:** a global on/off switch (`sysctl_util_est`, `:59-64`), on by default.
- Plain Linux: works as a governor (needs no userspace). Loading late is normal, since governors are
  switched at runtime via `scaling_governor`. Its init reads MediaTek EM/leakage data. If that fails,
  init returns an error before `cpufreq_register_governor` (`:1095-1132`).

### scheduler.ko (MediaTek EAS)
- **Hooks** (`K/drivers/misc/mediatek/sched/eas/sched_main.c:398-532`): `find_energy_efficient_cpu`,
  `find_busiest_group`, `cpu_overutilized`, `tick_entry` (thermal-ceiling-aware capacity from CSRAM),
  `update_cpu_capacity`, `schedule`, `newidle_balance`, RT `select_task_rq`/`find_lowest_rq`,
  big-task rotation and VIP.
- **Wake-up placement** (`K/.../sched/fair.c:830-1100`) follows stock feec: energy-based, with
  `latency_sensitive` → best idle CPU. Differences:
  - With `EAS_PERTASK_LS_SET`, any task with a per-task `uclamp.min > 0` counts as latency-sensitive
    (`:878-882`).
  - There is a per-task "idle prefer" flag (`:885-886`, `eas/eas_plus.c:1365-1381`).
  - A `system_cpumask` excludes CPUs for latency-sensitive tasks (`fair.c:409-438`).
  - VIP-aware selection.
  - `cpu_overutilized` is computed per performance domain, not per CPU (`eas/eas_plus.c:149-180`).
- **Interfaces:**
  - `/sys/devices/system/cpu/sched_ctl/{sched_target_margin,sched_turn_point_freq,sched_util_est_ctrl}`
    (`eas/sched_sys_common.c:41-163`).
  - `/proc/easmgr/eas_ioctl` (`sched_main.c:228-392,474-475`). Commands are in `eas/eas_plus.h:103-128`,
    e.g. `EAS_SET_TASK_IDLE_PREFER` = `_IOW('g',19,uint)` with a TID. Clearing it is broken: `val` is
    unsigned, so the `val < 0` branch never runs (`sched_main.c:234,364-370`).
- **VIP** (`eas/vip.c`): VIP tasks are queued ahead of normal CFS tasks on their rq for a time budget
  (`:513-545`).
  - VIP status comes from the VVIP/basic-VIP setters, group thresholds by cgroup name (`"top-app"`,
    `:297-330`) or moto_sched (`:486-511`).
  - The setters are exported symbols only. There's no userspace path except through moto_sched.
- Plain Linux: no userspace is needed, and placement and VIP are active right after load. `vip_init()`
  initializes existing tasks (`vip.c:943-979`), so a late load is designed for. Dependencies:
  `thermal_interface`, `cpufreq_sugov_ext` (`L/modules.dep:107`).
- Risk: it replaces core scheduler decisions. It's well exercised on Android, but our task and cgroup
  layout is unusual.

### mtk_core_ctl.ko
- Core pausing based on load/thermal. Controls are the module param `policy_enable`, default 0 = off
  (`K/.../sched/core_ctl/core_ctl.c:173-220,298`), and per-cluster sysfs `min_cpus`/`max_cpus`/`enable`
  (`:707-860`).
- With pausing enabled it takes cores away, the opposite of what we want. Load it only because
  `task_turbo` depends on it (`L/modules.dep:174`), and leave `policy_enable` at 0.

### task_turbo.ko
- **Params:** `/sys/module/task_turbo/parameters/feats` accepts only 0, 7 (latency) or 15 (launch, adds
  BIGCORE) (`task_turbo.c:106-109,948-990`). `turbo_pid` / `unset_turbo_pid` take a TID: at most 8, TID
  ≤ 32768, and only while `feats` ≠ 0 (`:111,995-1084`).
- **How threads get turbo:**
  - Through `turbo_pid`.
  - Automatically, only in launch mode (`feats` = 15), for tasks moved into the cpu cgroup whose kernfs
    id is 30 ("top-app" on Android) when they are a process leader or named `RenderThread` (Android HWUI)
    (`:49-52,1191-1208,1323-1339`).
  - Chrome on Linux has no `RenderThread` and we have no such cgroup, so use `turbo_pid`.
  - Android sets `feats`=15 only for launch-type hints (`V/etc/powerscntbl.xml:20,62,72`), not for
    scrolling.
- **Effects:**
  - (a) Renice to the thread's `RLIMIT_NICE` ceiling. If `RLIMIT_NICE` is 0 (the Linux default) it warns
    and keeps the old nice (`:240-266,753-779`). Android sets the rlimit to 40.
  - (b) BIGCORE (feats 15): every wake-up goes to an idle A76, else the A76 with most spare capacity,
    bypassing EAS (`:366-375,522-588`). That's a dynamic `taskset` to the A76s, which made the GPU
    process worse in our bench.
  - (c) Turbo is inherited by rwsem/futex/binder owners (`:280-360,635-870`).
- Plain Linux: usable via `turbo_pid`. Depends on mtk_core_ctl, scheduler, sugov_ext and
  thermal_interface (`L/modules.dep:174`).

### moto_sched.ko (Motorola UX)
- It doesn't match comm names. Threads are labeled by userspace (Motorola's framework daemon) writing
  `/proc/moto_sched/ux_task` as `w <tid> <ux_type bits>` (`M/drivers/moto_sched/msched_sysfs.c:237-353`).
  Bits are in `msched_common.h:58-81`, e.g. `UX_TYPE_TOPAPP` = 16, `UX_TYPE_ANIMATOR` = 8.
- Other files (`msched_sysfs.c:570-624`): `enabled` (hex bitmask; any nonzero value hooks it into
  scheduler.ko, `:96-126`), `ux_scene`, `boost_prio` (default 119), `boost_task_util` (100), `boost_pid`
  (one TID treated as top-app UX), `all_ux_tasks`.
- **Effect:** `task_get_mvp_prio()` maps the type to a VIP priority (TOPAPP → 70, ANIMATOR → 79,
  `msched_common.c:94-133`) with a per-wake runtime budget (TOPAPP 120 ms, `:136-177`), which scheduler.ko
  then uses for VIP queueing (`K/.../sched/eas/vip.c:177-190,486-511`). This shortens run-queue waits. It
  doesn't change frequency or core choice.
- "Top-app" detection uses the cpu-cgroup css id == 4 (`msched_common.h:107-115,211-215`), so Android
  creates foreground, background, top-app in that order.
- TID ≤ 32768 (`msched_sysfs.c:271`). Depends on scheduler.ko (`L/modules.dep:186`).
- Plain Linux: usable by hand. Late load is fine (oem data is zero → no UX type).

### touch_boost.ko + mtk_ioctl_touch_boost.ko + mtk_perf_ioctl.ko
- **Yes, this is an in-kernel input-driven CPU boost.** An input handler on every input device fires on
  `EV_KEY BTN_TOUCH` value 1 (touch-down only) (`K/drivers/misc/mediatek/performance/touch_boost/touch_boost.c:735-806`).
  The ILITEK driver reports `BTN_TOUCH` (`M/drivers/input/touchscreen/ilitek_v3_mmi/ilitek_v3_touch.c:1183`).
- A kthread raises a `freq_qos` minimum on each policy to OPP index `boost_opp_cluster_N` (sorted
  high→low, 0 = max) for `boost_duration` ms from touch-down. Moves don't extend it (`:212-266,1040-1066`).
- **Knobs** in `/proc/touch_boost/`: `enable`, `boost_duration` (default 100), `boost_opp_cluster_{0,1,2}`
  (default −1 = no boost), `active_time`, `boost_ta`, `boost_fg`, `idleprefer_ta`, `idleprefer_fg`,
  `deboost_when_render`, `force_stop_boost` (`:67-76,954-989,1037`).
- The `boost_ta`/`idleprefer` values are only forwarded to userspace: the vendor `touch_boost` daemon
  (`V/etc/init/touch_boost.rc`) blocks on `/proc/perfmgr_touch_boost/ioctl_touch_boost`
  (`K/.../performance/perf_ioctl/ioctl_touch_boost.c:55-125`) and calls the power HAL.
- Android's values (power HAL `InitWrite`, `V/etc/powercontable.xml:407-486`): enable 1, OPP index 2 on
  both clusters, duration 200 ms, boost_ta 80, idleprefer 1.
- Caveat: every boost and unboost `kmalloc`s a command node that only the ioctl reader frees
  (`touch_boost.c:127-156,159-191`). Without a reader the list grows by a small node per touch.
- If `fpsgo` is loaded and active, the boost is skipped (`:245-253`).
- Plain Linux: works without userspace for the frequency part. Our daemon already covers it better
  (hold while touching + 1.5 s), so this module isn't needed.

### powerhal_cpu_ctrl.ko
- `/proc/powerhal_cpu_ctrl/perfserv_freq`: write `min0 max0 min1 max1` (kHz). Each becomes a `freq_qos`
  min/max request per policy (`K/.../performance/powerhal_cpu_ctrl/powerhal_cpu_ctrl.c:246-300,344,400-404`).
- This is the interface libpowerhal uses for `PERF_RES_CPUFREQ_MIN/MAX_CLUSTER_*`, alongside legacy
  `/proc/perfmgr/...` and `scaling_{min,max}_freq` paths (strings in `V/lib64/libpowerhal.so`).
- On Linux, `scaling_min_freq`/`scaling_max_freq` do the same thing. Not needed.

### mtk_perf_common.ko, cpuqos_v3.ko, fpsgo.ko/mtk_fpsgo.ko
- **mtk_perf_common:** a scheduler-tick perf/PMU tracker for tracing
  (`/sys/devices/system/cpu/perf/*`, `K/drivers/misc/mediatek/perf_common/perf_common.c:202-290`,
  `perf_tracker.c:745-769`). No performance effect.
- **cpuqos_v3:** MPAM L3 partitioning that favors the `/top-app` cgroup
  (`K/drivers/misc/mediatek/cache-auditor/cpuqos_v3/mpam_proto.c:62-115,462-500`). It probes only on a
  `mediatek,cpuqos-v3` node (`:808-830`), and the stock DTB has none (`D/` grep), so `plat_enable` stays
  0 and it does nothing on this phone.
- **fpsgo / mtk_fpsgo:** frame-aware boosting fed by BufferQueue queue/dequeue ioctls from Android's
  graphics stack (`K/drivers/misc/mediatek/performance/perf_ioctl/perf_ioctl.c:407-440`). Chrome and
  Hyprland never send them, so it's inert on Linux.

## 2. What Android does during touch/scroll

**Boot and steady state:**
- At boot completion (`V/etc/init/hw/init.cgroup.rc:48-69`):
  - `sugov_ext` on policy0 and policy6.
  - `sched_pelt_multiplier` 4.
  - `sched_util_clamp_min_rt_default` 0 (RT tasks no longer get max frequency).
  - All cpuctl groups' `cpu.uclamp.min` 0 and `latency_sensitive` 0. They are 100/1 during boot
    (`:35-46`); during boot the frequency floor is 2.0 / 2.2 GHz (`:2-5`).
  - Its `/proc/sys/kernel/sched_migration_cost_ns` write is a no-op on GKI 5.15: the knob moved to
    debugfs (`G/kernel/sched/debug.c:323`).
- **cgroup layout:** v1 `cpu` at `/dev/cpuctl`, v1 `cpuset` at `/dev/cpuset`, blkio, memcg; cgroup v2
  only for freezer (`V/etc/cgroups.json:2-44`). cpuset cpus: top-app 0-7, foreground (and
  foreground/boost) 0-7, background 0-5, system-background 0-5 (`V/etc/init/hw/init.mt6835.rc:58-63`).
- Vendor task profiles: `PerfBoost` = SetClamps boost 50 %, `PerfClamp` = clamp 30 %. The aggregate
  `SCHED_SP_*`/`CPUSET_SP_*` profiles map to the system-side profiles (`V/etc/task_profiles.json:100-125,185-230`).

**Power HAL scenarios** (`V/etc/powerscntbl.xml`; resource→file mapping in `V/etc/powercontable.xml`,
e.g. `PERF_RES_SCHED_UCLAMP_MIN_TA` → `/dev/cpuctl/top-app/cpu.uclamp.min` `:101-107`,
`PERF_RES_SCHED_PREFER_IDLE_TA` → `/dev/cpuctl/top-app/cpu.uclamp.latency_sensitive` `:59-65`).
CLUSTER_0 = policy0 (A55), CLUSTER_1 = policy6 (A76). 4000000 = "max".

| Hint | CPU min (A55 / A76) | CPU max | TA uclamp.min | other | hold |
|---|---|---|---|---|---|
| INTERACTION (`:3-11`) | 1.5 / 1.7 GHz | – | 40 | TA latency_sensitive 1, DRAM OPP 0 | 1500 ms |
| UX_SCROLLING (`:159-165`) | 1.0 / 1.1 GHz | 2.0 / 2.2 GHz | 40 | – | while hinted |
| UX_MOVE_SCROLLING (`:154-158`) | – / 0.6 GHz | – / 2.2 GHz | 25 | – | while hinted |
| UX_SCROLLING_COMMON (`:189-194`) | – | – | – | TA latency_sensitive 1, FPSGO rescue off | |
| LAUNCH / PROCESS_CREATE (`:12-21,56-63`) | max / max | – | 100 | task_turbo feats 15 | 6 s |
| APP_TOUCH (`:119-121`) | – | – | – | notifies the touch_boost path | |

- The in-kernel touch_boost adds a 200 ms OPP-index-2 floor on touch-down (section 1).
- Chrome isn't in the per-app list (`V/etc/power_app_cfg.xml`, Chinese apps and games only).
- `INTERACTION` is the AOSP power boost the framework sends on user input (from AOSP behavior; the
  system partition isn't available to cite).
- The DRAM floor is the dvfsrc devfreq `userspace` governor (`K/drivers/devfreq/mtk-dvfsrc-devfreq.c:66-92,157`;
  the boot value 5500000000 is at `init.cgroup.rc:5`).

**Why uclamp 40 % matters here:**
- DT `capacity-dmips-mhz` is 383 (A55) vs 1024 (A76) (`D/:56-133`). With 2.0 vs 2.4 GHz, the A55
  capacity is ≈ 383·2.0/(1024·2.4)·1024 ≈ **319** (verify `/sys/devices/system/cpu/cpu0/cpu_capacity`).
- A boosted task fits a non-max CPU only if `uclamp_min ≤ capacity` (`G/kernel/sched/fair.c:4115-4235`,
  check at `:4232`). So top-app `uclamp.min` 40 % (409) makes top-app threads misfit on the A55s. During
  INTERACTION Android effectively moves the foreground app to the A76s, and sets the frequency floors.
- `latency_sensitive` + boosted makes stock feec pick the highest-capacity idle CPU (`fair.c:7068-7070,7114-7145,7184`).
- A value ≤ ~30 % keeps A55s eligible but drives an A55 running such a task near its top OPP.

## 3. Recipe for plain Linux

**Order: try tier A first. It needs no vendor module.**

### Tier A: stock GKI knobs (safe, reversible)

1. Boot-time, in `native-init`:
   - `echo 4 > /proc/sys/kernel/sched_pelt_multiplier` (accepts 1/2/4, `G/kernel/sched/pelt.c:500-512`).
   - Check `cat /proc/sys/kernel/sched_energy_aware` (EAS needs asymmetric capacity + EM; cpufreq-hw
     registers the EM at `K/drivers/cpufreq/mediatek-cpufreq-hw.c:225`; GKI drops the schedutil
     requirement, `G/kernel/sched/topology.c:355-376`).
   - Read `policy{0,6}/schedutil/rate_limit_us`. If it is several ms, try 1000–2000.
2. cgroup v1, Android layout. v1 matches what the MTK and Motorola modules look up by name and css id,
   and allows per-thread moves:
   ```sh
   mkdir -p /dev/cpuctl /dev/cpuset
   mount -t cgroup -o cpu    none /dev/cpuctl
   mount -t cgroup -o cpuset none /dev/cpuset
   for g in foreground background top-app system-background; do
     mkdir /dev/cpuctl/$g /dev/cpuset/$g    # this order gives top-app cpu css id 4 (moto_sched)
   done
   for g in foreground top-app; do echo 0-7 >/dev/cpuset/$g/cpuset.cpus; done
   for g in background system-background; do echo 0-5 >/dev/cpuset/$g/cpuset.cpus; done
   for g in foreground background top-app system-background; do echo 0 >/dev/cpuset/$g/cpuset.mems; done
   ```
   Android mounts cpuset with `noprefix` (`cpus`/`mems`). Without it the files are `cpuset.cpus`/`cpuset.mems`.
   - Put the session (shell/bar, keyboard, daemons) into `background` / `system-background` (A55 only).
     Put Hyprland and the Chrome launcher into `top-app` before exec. Children inherit, and v1
     `cgroup.procs` moves whole processes.
   - Per-thread alternative: write TIDs to `tasks`, e.g. only the four frame-critical threads into
     top-app, rest of Chrome in foreground.
   - cgroup v2 also works (`cpu.uclamp.latency_sensitive` exists in the v2 cftype,
     `G/kernel/sched/core.c:11125-11129`; no `RT_GROUP_SCHED` in `stock-kernel.config`). Per-thread
     placement then needs threaded mode.
3. Extend `vegas-gpu-boost` (first touch → boost; 1.5 s after the last event → release), matching
   INTERACTION:
   ```sh
   # boost
   echo 1500000 >/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq
   echo 1700000 >/sys/devices/system/cpu/cpufreq/policy6/scaling_min_freq
   echo 40 >/dev/cpuctl/top-app/cpu.uclamp.min          # A/B against 30 (see the capacity note)
   echo 1  >/dev/cpuctl/top-app/cpu.uclamp.latency_sensitive
   # optional DRAM floor: max of /sys/class/devfreq/mtk-dvfsrc-devfreq/available_frequencies
   #   -> .../userspace/set_freq
   # release
   echo 0 >/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq   # as init.cgroup.rc:57-58
   echo 0 >/sys/devices/system/cpu/cpufreq/policy6/scaling_min_freq
   echo 0 >/dev/cpuctl/top-app/cpu.uclamp.min
   echo 0 >/dev/cpuctl/top-app/cpu.uclamp.latency_sensitive
   # echo 0 > .../mtk-dvfsrc-devfreq/userspace/set_freq
   ```
   - A group `uclamp.min` boosts every task in the group (`uclamp_tg_restrict`, `G/kernel/sched/core.c:1458-1478`).
   - With only Chrome + Hyprland in top-app and background work fenced to 0-5, the A76 crowding seen
     with per-thread 614 should be smaller. Measure 40 vs 30 with `scripts/chrome-scroll-bench`.
   - Keep the same 37 °C battery-temperature gate for the frequency/DRAM floors.

### Tier B: optional vendor modules (one-way per boot; GPU boots only, after `gpu-prepare`)

Insmod in dependency order from `L/modules.dep`:
`thermal_interface` → `cpufreq_sugov_ext` → `scheduler` → (`moto_sched`) → (`mtk_core_ctl` → `task_turbo`).

- `sugov_ext`: `echo sugov_ext > policy{0,6}/scaling_governor`. Then try `up_rate_limit_us` 0–500 and
  `down_rate_limit_us` ~10000–20000 to hold frequency between frames. Read
  `/proc/mtk_scheduler/pd_capacity_tbl` to pick uclamp values from real capacities.
- `scheduler`: MTK feec and VIP. Per-thread prefer-idle via `/proc/easmgr/eas_ioctl`
  `EAS_SET_TASK_IDLE_PREFER` (set only).
- `moto_sched`:
  - `echo 1 > /proc/moto_sched/enabled`.
  - For each frame-critical TID: `echo "w <tid> 16" > /proc/moto_sched/ux_task` (VIP queueing, 120 ms
    budget per wake). Clear with `c <tid> 16`.
  - Addresses run-queue latency, not frequency.
- `task_turbo`: `echo 15 > .../feats` (must come first), then
  `echo <tid> > /sys/module/task_turbo/parameters/turbo_pid` (≤ 8 TIDs). Also set
  `prlimit --nice=40` on the process if renice is wanted. BIGCORE equals dynamic A76 pinning, so try
  only on the renderer compositor/main threads.
- Leave alone: `touch_boost` (daemon does it better; node leak), `mtk_ioctl_touch_boost`,
  `mtk_perf_ioctl`, `powerhal_cpu_ctrl`, `mtk_core_ctl` beyond the dependency (keep `policy_enable` 0),
  `cpuqos_v3` (no DT node), `mtk_perf_common`, `fpsgo`/`mtk_fpsgo`.

Chrome Linux thread names for TID lookup (`/proc/<pid>/task/*/comm`): renderer/GPU main = `chrome`,
`Compositor`, `VizCompositorTh`, raster `CompositorTileW` (comm is truncated to 15 characters; confirm on the device).

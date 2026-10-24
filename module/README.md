# Tosya

Makes a hidden uid look like it does not exist:

```
getpriority(PRIO_USER, uid)       -> -ESRCH
ioprio_get(IOPRIO_WHO_USER, uid)  -> -EINVAL
setpriority / ioprio_set          -> same
```

The rules come from the app a process was born from, so an isolated child or anything that called
`setuid()` cannot choose which of them apply. The repository README has the rest.

`customize.sh` picks the `ko/` entry matching `uname -r`, renames it to `ko/tosya.ko` and
removes the rest. With nothing for your kernel it leaves the closest build as
`ko/tosya.ko.try`, prints why another KMI's build will not load (vermagic, and the structures
this module reads), and points at `docs/build.md`. `post-fs-data.sh` loads the module as early
as it can and `service.sh` retries; both prefer `/data/adb/ksud insmod` and use the bundled
`lkmloader` only when ksud is absent, logging its exit code to `state/sync.log`
(`lkmloader ... debug=1` names things in dmesg for 60 seconds). `service.sh` then starts
`sync-tool`, the userspace half: it reads HMA's config, works out the pairs and pushes them,
with the apk records that name an isolated child, over the module's netlink family.

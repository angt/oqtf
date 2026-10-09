# oqtf

`oqtf` runs a command for a fixed time. When the time ends, it stops the
command and everything it spawned: children, grandchildren, daemons.
Nothing escapes, not even with `setsid` or a double fork.

`oqtf` itself is never stopped. With `-s`, the command is never stopped
either. Only its children.

Linux only. Needs cgroups v2.

## Build

```
make
```

Install it with `make install`.

## Use

Give a time, then a command:

```
oqtf 1m make
```

A build can not hang forever. `oqtf` stops it after one minute.

The time is mandatory.

```
oqtf 500ms curl api
oqtf 2m make
oqtf 1h30m make
```

With no command, `oqtf` starts your shell, from the `SHELL` environment
variable:

```
oqtf 2m30s
```

The shell stays. Every program you start from it lives for 2m30s at most.

## Options

```
-s, --shell    The command never stops, only its children
-h, --help     Show this help
```

Options work before or after the time. Use `-s` for shells and
supervisors:

```
oqtf 30m -s tmux
```

`tmux` stays. Its programs get thirty minutes each.

`--` ends the options. Everything after it is the command.

## How it works

`oqtf` makes a new cgroup and moves itself into it. The command starts in
the same cgroup, and everything it spawns is born there too.

Every fraction of a second, `oqtf` reads `cgroup.procs` in that cgroup.
A process older than the time gets `SIGKILL`. The cgroup is the complete
list, so no process can hide.

The command gets `SIGTERM` at the time limit, then `SIGKILL` two seconds
later.

As root, the command runs in its own cgroup namespace. It can not move
itself out.

When the command exits, `oqtf` stops what is left, moves itself out, and
removes the cgroup.

## Permissions

You need write access to the cgroup. This usually means `sudo`:

```
sudo oqtf 1m make
```

Most modern distros give your user session its own cgroups. Then you do
not need `sudo`.

## Exit code

`oqtf` exits with the code of the command. Other cases:

```
124    the time fired
127    the command can not run, like `timeout`
2      bad usage, for example no time
1      `oqtf` itself failed
```

A command stopped by a signal gives 128 plus the signal number: 143 for
`SIGTERM`, 137 for `SIGKILL`.

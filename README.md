# boxer

boxer offers stand-alone Linux containerization with no dependencies.

## Getting Started

To compile and install boxer on your system, run

```shell
make
make install
```

To place the binary in your working directory instead, run

```shell
PREFIX='.' make install
```

Please note that both `make install` calls require root rights
inorder to make the resulting boxer binary a setuid root.

### Usage

Passing any command to boxer will execute the command inside a fresh
container.

```shell
boxer COMMAND...
```

If no command is given, the login shell of the user will be started inside
a container instead.

#### Cgroups

boxer allows you to setup cgroups via command line flags. Flags with the
prefix `cgroup.` followed by the name of a cgroup subsystem and its parameter
cause boxer to activate said cgroup inside the container and pass the option's
value to the cgroup subsystem.

##### Example

The following call

```shell
boxer --cgroup.memory.limit_in_bytes=128m --cgroup.cpu.shares=512
```

activates the *memory* and *cpu* cgroup subsystems inside the container,
with the container's memory limit set to 128 MB and the container's cpu shares
set to 512.

#### Resource Limits

Similar to the cgroup command line flags, boxer supports setting resource
limits via command line flags prefixed with `rlimit.`. To view all
resources limits, run the `prlimit` command or read the `prlimit(2)` manual
page.

##### Example

The following call

```shell
boxer --rlimit.fsize=1m --rlimit.nproc=4k
```

sets the maximum file size inside the container to 1 MB and the
maximum number of processes that can be created inside the container to 4096.

### License

boxer is released under MIT license.
You can find a copy of the MIT License in the [LICENSE](./LICENSE) file.

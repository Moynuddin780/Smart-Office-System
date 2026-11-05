# DXRT runtime, vendored 2026-09-24

Copied from this board's `/usr/local` so the Docker image can be built without
a DEEPX installer inside it:

    include/dxrt/    headers  (dxrt_cxx_api.h and friends)
    lib/libdxrt.so*  DXRT v3.4.0+fad14d6
    lib/libonnxruntime.so*   a link-time dependency of libdxrt

The container does NOT contain an NPU driver. It talks to the host's `dxrtd`
service, which brokers both inference job submission and telemetry. Three things
are needed and each was established empirically by the sibling project:

  /dev/dxrt0                    the device node
  /tmp/dxrt_dynamic_ipc.sock    dxrtd also listens on an abstract-namespace
                                socket, which is scoped to the network namespace
                                and therefore invisible to a container; the
                                filesystem twin is what a container can reach
  DXRT_DYNAMIC_IPC_ENDPOINT     without it the client never looks at the mounted
                                path, burns ~500 ms per engine load retrying the
                                unreachable abstract socket, and silently falls
                                back to a degraded no-service path
  pid: host                     the concrete socket alone was NOT sufficient;
                                dxrtd's protocol depends on seeing real host PIDs

`/dev/shm/dxrt_monitor_v2` is additionally needed for DeviceStatus telemetry
(temperature, clock, per-core utilisation).

Keep this in step with the host: a DXRT upgrade means re-copying these files.
`setup.sh` checks the vendored version against `dxrt-cli -v` and warns on drift.

## Not part of the Git repository

The DEEPX runtime headers and libraries are licensed by DEEPX to its customers, so `.gitignore` leaves them out and only
this note is committed. `./setup.sh` copies them from the host's DXRT installation (`/usr/local/include/dxrt` and
`/usr/local/lib/libdxrt.so*`) into this folder before the Docker image is built.

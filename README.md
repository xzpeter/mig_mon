About
======

`mig_mon` is the short form of `Migration Monitor`.  It's a set of tools for VM
migration testing and debugging.

Features
===========

`mig_mon` provides a few sub-commands to use.

VM Live Migration Network Emulator
----------------------------------------

This sub-tool can be used to emulate live migration TCP streams.

There're two types of live migration: (1) precopy (2) postcopy.  This tool can
emulate (1) or (2) or (1+2) case by specifying different '-t' parameters:

  - Enable precopy only: it emulates a TCP_STREAM workload from src->dst
  - Enable postcopy only: it emulates a TCP_RR workload from dst->src
  - Enable both: it emulates the above TCP_STREAM+TCP_RR on the same socket

For precopy stream, it's the bandwidth that matters.  The bandwidth
information will be dumped per-second on src VM.

For postcopy stream, it's the latency that matters.  The average/maximum
latency value of page requests will be dumped per-second on dst VM.

This sub-command has below parameters:

    ./mig_mon vm [options...]
      -d:    Emulate a dst VM
      -h:    Dump help message
      -H:    Specify dst VM IP (required for -s)
      -s:    Emulate a src VM
      -S:    Specify size of the VM (GB)
      -t:    Specify tests (precopy, postcopy)

Example usage:

To start the (emulated) destination VM, one can run this on dest host:

    ./mig_mon vm -d

Then, to start a src VM emulation and start both live migration streams,
one can run this command on src host:

    ./mig_mon vm -s -H $DEST_IP -t precopy -t postcopy

Specifying both '-t' will just enable both migration streams.

Memory Dirty
--------------

Sub-command "mm_dirty" can generate a constant dirty workload in the system.

    ./mig_mon mm_dirty [options...]
       -m:    memory size in MB (default: 512)
       -r:    dirty rate in MB/s (default: unlimited)
       -p:    work pattern: "sequential", "random", or "once"
              (default: "sequential")
       -P:    page size: "2m" or "1g" for huge pages

To generate a random dirty workload of 500MB/s upon 4GB memory range, we can
use:

    ./mig_mon mm_dirty -m 2000 -r 500 -p random
    
The dirty workload will always dirty pages in 4K page size (even if huge pages
are used) because normally hypervisor will trap dirty in small page size always.

Pre-heat will be done before starting the real workload.

Network Downtime Measurement
---------------------------------

Sub-command "server_rr/client" can be used to measure guest OS network downtime
during migration.  To use it, we can first start the UDP echo server in the
guest using:

    ./mig_mon server_rr
    
Then from outside the guest, we can start the client trying to send a packet
for constant interval (e.g. 50ms) and waiting for a response:

    ./mig_mon client $GUEST_IP 50 $LOG
    
The client side will record the latency of each packet received, recording
spikes into $LOG and also show the maximum latency detected.

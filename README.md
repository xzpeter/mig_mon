About
======

`mig_mon` is the short form of `Migration Monitor`.  It's a set of tools for VM
migration testing and debugging.

Features
===========

`mig_mon` provides a few sub-commands to use.

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

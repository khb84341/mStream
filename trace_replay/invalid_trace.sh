echo 1 > /proc/sys/vm/overcommit_memory 
echo 0 > /sys/kernel/debug/tracing/tracing_on
echo > /sys/kernel/debug/tracing/trace
cat /sys/kernel/debug/tracing/trace_pipe | grep "INVALID\|EARLY\|KMEAN\|STAT\|F2FS-GC\|PBLK-GC\|DB-lifetime" > trace_$1
#cat /sys/kernel/debug/tracing/trace_pipe | grep "INVALID\|EARLY\|first\|afterGC\|KMEAN\|STAT\|F2FS-GC\|PBLK-GC\|DB-lifetime" > trace_$1

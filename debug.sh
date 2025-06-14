echo 0 > /sys/kernel/debug/msm_vidc/debug_output
echo 0xff > /sys/module/videobuf2_core/parameters/debug
echo 0x7fffffff > /sys/kernel/debug/msm_vidc/debug_level
echo 0xff > /sys/devices/platform/soc/aa00000.qcom,vidc/video4linux/video33/dev_debug
echo 6 > /sys/module/drm/parameters/debug
cat /sys/kernel/debug/msm_vidc/core0/*
strace -Tfe trace=ioctl ./v4l2_decode fcamera.hevc -v
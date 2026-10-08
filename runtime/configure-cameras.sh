#!/bin/sh
set -eu

# This topology is the one validated on the source RK3588 board:
# CSI3 -> media4/subdev2/video44; CSI4 -> media5/subdev7/video53.
killall rkaiq_3A_server 2>/dev/null || true
sleep 1

v4l2-ctl -d /dev/v4l-subdev2 \
  --set-subdev-fmt='pad=0,width=3864,height=2192,code=0x300e' >/dev/null 2>&1 || true
v4l2-ctl -d /dev/v4l-subdev7 \
  --set-subdev-fmt='pad=0,width=648,height=640,code=0x300e' >/dev/null 2>&1 || true

media-ctl -d /dev/media4 --set-v4l2 \
  '"rkcif-mipi-lvds2":0[fmt:SGBRG10_1X10/3840x2160]'
media-ctl -d /dev/media4 --set-v4l2 \
  '"rkisp-isp-subdev":0[fmt:SGBRG10_1X10/3840x2160 crop:(0,0)/3840x2160]'
media-ctl -d /dev/media4 --set-v4l2 \
  '"rkisp-isp-subdev":2[fmt:YUYV8_2X8/3840x2160 crop:(0,0)/3840x2160]'

media-ctl -d /dev/media5 --set-v4l2 \
  '"rkcif-mipi-lvds4":0[fmt:SGBRG10_1X10/640x640]'
media-ctl -d /dev/media5 --set-v4l2 \
  '"rkisp-isp-subdev":0[fmt:SGBRG10_1X10/640x640 crop:(0,80)/640x480]'
media-ctl -d /dev/media5 --set-v4l2 \
  '"rkisp-isp-subdev":2[fmt:YUYV8_2X8/640x480 crop:(0,0)/640x480]'

v4l2-ctl -d /dev/video44 --set-fmt-video=width=960,height=540,pixelformat=NV12
v4l2-ctl -d /dev/video53 --set-fmt-video=width=640,height=480,pixelformat=NV12

/usr/bin/rkaiq_3A_server >/tmp/rkaiq_dual_global_roi.log 2>&1 &
sleep 3
echo 'CSI3: 4K60 RAW10 -> ISP NV12 960x540 (/dev/video44)'
echo 'CSI4: sensor ROI 648x640 RAW10 -> ISP NV12 640x480 (/dev/video53)'

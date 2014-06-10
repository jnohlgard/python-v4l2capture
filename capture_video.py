#!/usr/bin/python
#
# python-v4l2capture
#
# This file is an example on how to capture a mjpeg video with
# python-v4l2capture.
#
# 2009, 2010 Fredrik Portstrom
#
# I, the copyright holder of this file, hereby release it into the
# public domain. This applies worldwide. In case this is not legally
# possible: I grant anyone the right to use this work for any
# purpose, without any conditions, unless such conditions are
# required by law.

import Image
import select
import v4l2capture
import time

# Open the video device.
video = v4l2capture.Video_device("/dev/video0")

# Suggest an image size to the device. The device may choose and
# return another size if it doesn't support the suggested one.
size_x, size_y = video.set_format(1280, 1024, fourcc='MJPG')

# Create a buffer to store image data in. This must be done before
# calling 'start' if v4l2capture is compiled with libv4l2. Otherwise
# raises IOError.
video.create_buffers(30)

# Send the buffer to the device. Some devices require this to be done
# before calling 'start'.
video.queue_all_buffers()

# Start the device. This lights the LED if it's a camera that has one.
video.start()

stop_time = time.time() + 10.0
with open('video.mjpg', 'wb') as f:
    while stop_time >= time.time():
        # Wait for the device to fill the buffer.
        select.select((video,), (), ())

        # The rest is easy :-)
        image_data = video.read_and_queue()
        f.write(image_data)
    
video.close()
print "Saved video.mjpg (Size: " + str(size_x) + " x " + str(size_y) + ")"

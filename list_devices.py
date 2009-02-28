#!/usr/bin/python
#
# python-v4l2capture
#
# 2009 Fredrik Portstrom
#
# I, the copyright holder of this file, hereby release it into the
# public domain. This applies worldwide. In case this is not legally
# possible: I grant anyone the right to use this work for any
# purpose, without any conditions, unless such conditions are
# required by law.

import os
import v4l2capture
file_names = [x for x in os.listdir("/dev") if x.startswith("video")]
file_names.sort()
for file_name in file_names:
    path = "/dev/" + file_name
    print path
    try:
        video = v4l2capture.Video_device(path)
        print "    driver:    %s\n    card:      %s\n    bus info:  %s" \
            % video.get_info()
        video.close()
    except IOError, e:
        print "    " + str(e)

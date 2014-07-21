// python-v4l2capture
// Python extension to capture video with video4linux2
//
// 2009, 2010, 2011 Fredrik Portstrom
//
// I, the copyright holder of this file, hereby release it into the
// public domain. This applies worldwide. In case this is not legally
// possible: I grant anyone the right to use this work for any
// purpose, without any conditions, unless such conditions are
// required by law.

#define USE_LIBV4L

#include <Python.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>

#ifdef USE_LIBV4L
#include <libv4l2.h>
#else
#include <sys/ioctl.h>
#define v4l2_close close
#define v4l2_ioctl ioctl
#define v4l2_mmap mmap
#define v4l2_munmap munmap
#define v4l2_open open
#endif

#ifndef Py_TYPE
    #define Py_TYPE(ob) (((PyObject*)(ob))->ob_type)
#endif


#define ASSERT_OPEN if(self->fd < 0)					\
    {									\
      PyErr_SetString(PyExc_ValueError,					\
	  "I/O operation on closed file");				\
      return NULL;							\
    }

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
  void *start;
  size_t length;
};

typedef struct {
  PyObject_HEAD
  int fd;
  struct buffer *buffers;
  int buffer_count;
} Video_device;

struct capability {
  int id;
  const char *name;
};

static struct capability capabilities[] = {
  { V4L2_CAP_ASYNCIO, "asyncio" },
  { V4L2_CAP_AUDIO, "audio" },
  { V4L2_CAP_HW_FREQ_SEEK, "hw_freq_seek" },
  { V4L2_CAP_RADIO, "radio" },
  { V4L2_CAP_RDS_CAPTURE, "rds_capture" },
  { V4L2_CAP_READWRITE, "readwrite" },
  { V4L2_CAP_SLICED_VBI_CAPTURE, "sliced_vbi_capture" },
  { V4L2_CAP_SLICED_VBI_OUTPUT, "sliced_vbi_output" },
  { V4L2_CAP_STREAMING, "streaming" },
  { V4L2_CAP_TUNER, "tuner" },
  { V4L2_CAP_VBI_CAPTURE, "vbi_capture" },
  { V4L2_CAP_VBI_OUTPUT, "vbi_output" },
  { V4L2_CAP_VIDEO_CAPTURE, "video_capture" },
  { V4L2_CAP_VIDEO_OUTPUT, "video_output" },
  { V4L2_CAP_VIDEO_OUTPUT_OVERLAY, "video_output_overlay" },
  { V4L2_CAP_VIDEO_OVERLAY, "video_overlay" }
};

static int my_ioctl(int fd, int request, void *arg)
{
  // Retry ioctl until it returns without being interrupted.

  for(;;)
    {
      int result = v4l2_ioctl(fd, request, arg);

      if(!result)
	{
	  return 0;
	}

      if(errno != EINTR)
	{
	  PyErr_SetFromErrno(PyExc_IOError);
	  return 1;
	}
    }
}

static void Video_device_unmap(Video_device *self)
{
  int i;

  for(i = 0; i < self->buffer_count; i++)
    {
      v4l2_munmap(self->buffers[i].start, self->buffers[i].length);
    }
}

static void Video_device_dealloc(Video_device *self)
{
  if(self->fd >= 0)
    {
      if(self->buffers)
	{
	  Video_device_unmap(self);
	}

      v4l2_close(self->fd);
    }

  Py_TYPE(self)->tp_free((PyObject *)self);
}

static int Video_device_init(Video_device *self, PyObject *args,
    PyObject *kwargs)
{
  const char *device_path;

  if(!PyArg_ParseTuple(args, "s", &device_path))
    {
      return -1;
    }

  int fd = v4l2_open(device_path, O_RDWR | O_NONBLOCK);

  if(fd < 0)
    {
      PyErr_SetFromErrnoWithFilename(PyExc_IOError, (char *)device_path);
      return -1;
    }

  self->fd = fd;
  self->buffers = NULL;
  return 0;
}

static PyObject *Video_device_close(Video_device *self)
{
  if(self->fd >= 0)
    {
      if(self->buffers)
	{
	  Video_device_unmap(self);
	}

      v4l2_close(self->fd);
      self->fd = -1;
    }

  Py_RETURN_NONE;
}

static PyObject *Video_device_fileno(Video_device *self)
{
  ASSERT_OPEN;
#if PY_MAJOR_VERSION < 3
  return PyInt_FromLong(self->fd);
#else
  return PyLong_FromLong(self->fd);
#endif
}

static PyObject *Video_device_get_info(Video_device *self)
{
  ASSERT_OPEN;
  struct v4l2_capability caps;

  if(my_ioctl(self->fd, VIDIOC_QUERYCAP, &caps))
    {
      return NULL;
    }

  PyObject *set = PySet_New(NULL);

  if(!set)
    {
      return NULL;
    }

  struct capability *capability = capabilities;

  while((void *)capability < (void *)capabilities + sizeof(capabilities))
    {
      if(caps.capabilities & capability->id)
	{
#if PY_MAJOR_VERSION < 3
          PyObject *s = PyString_FromString(capability->name);
#else
	  PyObject *s = PyBytes_FromString(capability->name);
#endif

	  if(!s)
	    {
              Py_DECREF(set);
	      return NULL;
	    }

	  PySet_Add(set, s);
	}

      capability++;
    }

  return Py_BuildValue("sssO", caps.driver, caps.card, caps.bus_info, set);
}

static PyObject *Video_device_set_format(Video_device *self, PyObject *args, PyObject *keywds)
{
  int size_x;
  int size_y;
  int yuv420 = 0;
  int fourcc;
  const char *fourcc_str;
  int fourcc_len = 0;
  static char *kwlist [] = {
    "size_x",
    "size_y",
    "yuv420",
    "fourcc",
    NULL
  };

  if (!PyArg_ParseTupleAndKeywords(args, keywds, "ii|is#", kwlist, &size_x, &size_y, &yuv420, &fourcc_str, &fourcc_len))
    {
      return NULL;
    }

  struct v4l2_format format;
  CLEAR(format);
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  /* Get the current format */
  if(my_ioctl(self->fd, VIDIOC_G_FMT, &format))
  {
    return NULL;
  }

#ifdef USE_LIBV4L
  format.fmt.pix.pixelformat =
    yuv420 ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_RGB24;
#else
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
#endif
  format.fmt.pix.field = V4L2_FIELD_INTERLACED;

  if (fourcc_len == 4) {
    fourcc = v4l2_fourcc(fourcc_str[0],
                       fourcc_str[1],
                       fourcc_str[2],
                       fourcc_str[3]);
    format.fmt.pix.pixelformat = fourcc;
    format.fmt.pix.field = V4L2_FIELD_ANY;
  }

  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = size_x;
  format.fmt.pix.height = size_y;
  format.fmt.pix.bytesperline = 0;

  if(my_ioctl(self->fd, VIDIOC_S_FMT, &format))
    {
      return NULL;
    }
 
  return Py_BuildValue("ii", format.fmt.pix.width, format.fmt.pix.height);
}

static PyObject *Video_device_set_fps(Video_device *self, PyObject *args)
{
  int fps;
  if(!PyArg_ParseTuple(args, "i", &fps))
    {
      return NULL;
    }
  struct v4l2_streamparm setfps;
  CLEAR(setfps);
  setfps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  setfps.parm.capture.timeperframe.numerator = 1;
  setfps.parm.capture.timeperframe.denominator = fps;
  if(my_ioctl(self->fd, VIDIOC_S_PARM, &setfps)){
  	return NULL;
  }
  return Py_BuildValue("i",setfps.parm.capture.timeperframe.denominator);
}

static void get_fourcc_str(char *fourcc_str, int fourcc)
{
  if (fourcc_str == NULL)
     return;
  fourcc_str[0] = (char)(fourcc & 0xFF);
  fourcc_str[1] = (char)((fourcc >> 8) & 0xFF);
  fourcc_str[2] = (char)((fourcc >> 16) & 0xFF);
  fourcc_str[3] = (char)((fourcc >> 24) & 0xFF);
  fourcc_str[4] = 0;
}

static PyObject *Video_device_get_format(Video_device *self)
{
  struct v4l2_format format;
  CLEAR(format);
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  /* Get the current format */
  if(my_ioctl(self->fd, VIDIOC_G_FMT, &format))
    {
      return NULL;
    }

  char current_fourcc[5];
  get_fourcc_str(current_fourcc, format.fmt.pix.pixelformat);
  return Py_BuildValue("iis", format.fmt.pix.width, format.fmt.pix.height, current_fourcc);
}

static PyObject *Video_device_get_fourcc(Video_device *self, PyObject *args)
{
  char *fourcc_str;
  int size;
  int fourcc;
  if (!PyArg_ParseTuple(args, "s#", &fourcc_str, &size))
    {
      return NULL;
    }

  if(size < 4)
    {
      return NULL;
    }

  fourcc = v4l2_fourcc(fourcc_str[0],
                       fourcc_str[1],
                       fourcc_str[2],
                       fourcc_str[3]);
  return Py_BuildValue("i", fourcc);
}

static PyObject *Video_device_set_auto_white_balance(Video_device *self, PyObject *args)
{
#ifdef V4L2_CID_AUTO_WHITE_BALANCE
  int autowb;
  if(!PyArg_ParseTuple(args, "i", &autowb))
    {
      return NULL;
    }

  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_AUTO_WHITE_BALANCE;
  ctrl.value = autowb;
  if(my_ioctl(self->fd, VIDIOC_S_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_get_auto_white_balance(Video_device *self)
{
#ifdef V4L2_CID_AUTO_WHITE_BALANCE
  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_AUTO_WHITE_BALANCE;
  if(my_ioctl(self->fd, VIDIOC_G_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_set_white_balance_temperature(Video_device *self, PyObject *args)
{
#ifdef V4L2_CID_WHITE_BALANCE_TEMPERATURE
  int wb;
  if(!PyArg_ParseTuple(args, "i", &wb))
    {
      return NULL;
    }

  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_WHITE_BALANCE_TEMPERATURE;
  ctrl.value = wb;
  if(my_ioctl(self->fd, VIDIOC_S_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_get_white_balance_temperature(Video_device *self)
{
#ifdef V4L2_CID_WHITE_BALANCE_TEMPERATURE
  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_WHITE_BALANCE_TEMPERATURE;
  if(my_ioctl(self->fd, VIDIOC_G_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_set_exposure_absolute(Video_device *self, PyObject *args)
{
#ifdef V4L2_CID_EXPOSURE_ABSOLUTE
  int exposure;
  if(!PyArg_ParseTuple(args, "i", &exposure))
    {
      return NULL;
    }

  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_EXPOSURE_ABSOLUTE;
  ctrl.value = exposure;
  if(my_ioctl(self->fd, VIDIOC_S_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_get_exposure_absolute(Video_device *self)
{
#ifdef V4L2_CID_EXPOSURE_ABSOLUTE
  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_EXPOSURE_ABSOLUTE;
  if(my_ioctl(self->fd, VIDIOC_G_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_set_exposure_auto(Video_device *self, PyObject *args)
{
#ifdef V4L2_CID_EXPOSURE_AUTO
  int autoexposure;
  if(!PyArg_ParseTuple(args, "i", &autoexposure))
    {
      return NULL;
    }

  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
  ctrl.value = autoexposure;
  if(my_ioctl(self->fd, VIDIOC_S_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_get_exposure_auto(Video_device *self)
{
#ifdef V4L2_CID_EXPOSURE_AUTO
  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
  if(my_ioctl(self->fd, VIDIOC_G_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_set_focus_auto(Video_device *self, PyObject *args)
{
#ifdef V4L2_CID_FOCUS_AUTO
  int autofocus;
  if(!PyArg_ParseTuple(args, "i", &autofocus))
    {
      return NULL;
    }

  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_FOCUS_AUTO;
  ctrl.value = autofocus;
  if(my_ioctl(self->fd, VIDIOC_S_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_get_focus_auto(Video_device *self)
{
#ifdef V4L2_CID_FOCUS_AUTO
  struct v4l2_control ctrl;
  CLEAR(ctrl);
  ctrl.id    = V4L2_CID_FOCUS_AUTO;
  if(my_ioctl(self->fd, VIDIOC_G_CTRL, &ctrl)){
  	return NULL;
  }
  return Py_BuildValue("i",ctrl.value);
#else
  return NULL;
#endif
}

static PyObject *Video_device_start(Video_device *self)
{
  ASSERT_OPEN;
  enum v4l2_buf_type type;
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if(my_ioctl(self->fd, VIDIOC_STREAMON, &type))
    {
      return NULL;
    }

  Py_RETURN_NONE;
}

static PyObject *Video_device_stop(Video_device *self)
{
  ASSERT_OPEN;
  enum v4l2_buf_type type;
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if(my_ioctl(self->fd, VIDIOC_STREAMOFF, &type))
    {
      return NULL;
    }

  Py_RETURN_NONE;
}

static PyObject *Video_device_create_buffers(Video_device *self, PyObject *args)
{
  int buffer_count;

  if(!PyArg_ParseTuple(args, "I", &buffer_count))
    {
      return NULL;
    }

  ASSERT_OPEN;

  if(self->buffers)
    {
      PyErr_SetString(PyExc_ValueError, "Buffers are already created");
      return NULL;
    }

  struct v4l2_requestbuffers reqbuf;
  reqbuf.count = buffer_count;
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = V4L2_MEMORY_MMAP;

  if(my_ioctl(self->fd, VIDIOC_REQBUFS, &reqbuf))
    {
      return NULL;
    }

  if(!reqbuf.count)
    {
      PyErr_SetString(PyExc_IOError, "Not enough buffer memory");
      return NULL;
    }

  self->buffers = malloc(reqbuf.count * sizeof(struct buffer));

  if(!self->buffers)
    {
      PyErr_NoMemory();
      return NULL;
    }

  int i;

  for(i = 0; i < reqbuf.count; i++)
    {
      struct v4l2_buffer buffer;
      buffer.index = i;
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;

      if(my_ioctl(self->fd, VIDIOC_QUERYBUF, &buffer))
	{
	  return NULL;
	}

      self->buffers[i].length = buffer.length;
      self->buffers[i].start = v4l2_mmap(NULL, buffer.length,
	  PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, buffer.m.offset);

      if(self->buffers[i].start == MAP_FAILED)
	{
	  PyErr_SetFromErrno(PyExc_IOError);
	  return NULL;
	}
    }

  self->buffer_count = i;
  Py_RETURN_NONE;
}

static PyObject *Video_device_queue_all_buffers(Video_device *self)
{
  if(!self->buffers)
    {
      ASSERT_OPEN;
      PyErr_SetString(PyExc_ValueError, "Buffers have not been created");
      return NULL;
    }

  int i;
  int buffer_count = self->buffer_count;

  for(i = 0; i < buffer_count; i++)
    {
      struct v4l2_buffer buffer;
      buffer.index = i;
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;

      if(my_ioctl(self->fd, VIDIOC_QBUF, &buffer))
	{
	  return NULL;
	}
    }

  Py_RETURN_NONE;
}

static PyObject *Video_device_read_internal(Video_device *self, int queue)
{
  if(!self->buffers)
    {
      ASSERT_OPEN;
      PyErr_SetString(PyExc_ValueError, "Buffers have not been created");
      return NULL;
    }

  struct v4l2_buffer buffer;
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;

  if(my_ioctl(self->fd, VIDIOC_DQBUF, &buffer))
    {
      return NULL;
    }

#ifdef USE_LIBV4L
#if PY_MAJOR_VERSION < 3
  PyObject *result = PyString_FromStringAndSize(
#else
  PyObject *result = PyBytes_FromStringAndSize(
#endif
      self->buffers[buffer.index].start, buffer.bytesused);

  if(!result)
    {
      return NULL;
    }
#else
  // Convert buffer from YUYV to RGB.
  // For the byte order, see: http://v4l2spec.bytesex.org/spec/r4339.htm
  // For the color conversion, see: http://v4l2spec.bytesex.org/spec/x2123.htm
  int length = buffer.bytesused * 6 / 4;
#if PY_MAJOR_VERSION < 3
  PyObject *result = PyString_FromStringAndSize(NULL, length);
#else
  PyObject *result = PyBytes_FromStringAndSize(NULL, length);
#endif

  if(!result)
    {
      return NULL;
    }

  char *rgb = PyString_AS_STRING(result);
  char *rgb_max = rgb + length;
  unsigned char *yuyv = self->buffers[buffer.index].start;

#define CLAMP(c) ((c) <= 0 ? 0 : (c) >= 65025 ? 255 : (c) >> 8)
  while(rgb < rgb_max)
    {
      int u = yuyv[1] - 128;
      int v = yuyv[3] - 128;
      int uv = 100 * u + 208 * v;
      u *= 516;
      v *= 409;

      int y = 298 * (yuyv[0] - 16);
      rgb[0] = CLAMP(y + v);
      rgb[1] = CLAMP(y - uv);
      rgb[2] = CLAMP(y + u);

      y = 298 * (yuyv[2] - 16);
      rgb[3] = CLAMP(y + v);
      rgb[4] = CLAMP(y - uv);
      rgb[5] = CLAMP(y + u);

      rgb += 6;
      yuyv += 4;
    }
#undef CLAMP
#endif

  if(queue && my_ioctl(self->fd, VIDIOC_QBUF, &buffer))
    {
      return NULL;
    }

  return result;
}

static PyObject *Video_device_read(Video_device *self)
{
  return Video_device_read_internal(self, 0);
}

static PyObject *Video_device_read_and_queue(Video_device *self)
{
  return Video_device_read_internal(self, 1);
}

static PyMethodDef Video_device_methods[] = {
  {"close", (PyCFunction)Video_device_close, METH_NOARGS,
       "close()\n\n"
       "Close video device. Subsequent calls to other methods will fail."},
  {"fileno", (PyCFunction)Video_device_fileno, METH_NOARGS,
       "fileno() -> integer \"file descriptor\".\n\n"
       "This enables video devices to be passed select.select for waiting "
       "until a frame is available for reading."},
  {"get_info", (PyCFunction)Video_device_get_info, METH_NOARGS,
       "get_info() -> driver, card, bus_info, capabilities\n\n"
       "Returns three strings with information about the video device, and one "
       "set containing strings identifying the capabilities of the video "
       "device."},
  {"get_fourcc", (PyCFunction)Video_device_get_fourcc, METH_VARARGS,
       "get_fourcc(fourcc_string) -> fourcc_int\n\n"
       "Return the fourcc string encoded as int."},
  {"get_format", (PyCFunction)Video_device_get_format, METH_NOARGS,
       "get_format() -> size_x, size_y, fourcc\n\n"
       "Request the current video format."},
  {"set_format", (PyCFunction)Video_device_set_format, METH_VARARGS|METH_KEYWORDS,
       "set_format(size_x, size_y, yuv420 = 0, fourcc='MJPEG') -> size_x, size_y\n\n"
       "Request the video device to set image size and format. The device may "
       "choose another size than requested and will return its choice. The "
       "image format will be RGB24 if yuv420 is zero (default) or YUV420 if "
       "yuv420 is 1, if fourcc keyword is set that will be the fourcc pixel format used."},
  {"set_fps", (PyCFunction)Video_device_set_fps, METH_VARARGS,
       "set_fps(fps) -> fps \n\n"
       "Request the video device to set frame per seconds.The device may "
       "choose another frame rate than requested and will return its choice. " },
  {"set_auto_white_balance", (PyCFunction)Video_device_set_auto_white_balance, METH_VARARGS,
       "set_auto_white_balance(autowb) -> autowb \n\n"
       "Request the video device to set auto white balance to value. The device may "
       "choose another value than requested and will return its choice. " },
  {"get_auto_white_balance", (PyCFunction)Video_device_get_auto_white_balance, METH_NOARGS,
       "get_auto_white_balance() -> autowb \n\n"
       "Request the video device to get auto white balance value. " },
  {"set_white_balance_temperature", (PyCFunction)Video_device_set_white_balance_temperature, METH_VARARGS,
       "set_white_balance_temperature(temp) -> temp \n\n"
       "Request the video device to set white balance tempature to value. The device may "
       "choose another value than requested and will return its choice. " },
  {"get_white_balance_temperature", (PyCFunction)Video_device_get_white_balance_temperature, METH_NOARGS,
       "get_white_balance_temperature() -> temp \n\n"
       "Request the video device to get white balance temperature value. " },
  {"set_exposure_auto", (PyCFunction)Video_device_set_exposure_auto, METH_VARARGS,
       "set_exposure_auto(autoexp) -> autoexp \n\n"
       "Request the video device to set auto exposure to value. The device may "
       "choose another value than requested and will return its choice. " },
  {"get_exposure_auto", (PyCFunction)Video_device_get_exposure_auto, METH_NOARGS,
       "get_exposure_auto() -> autoexp \n\n"
       "Request the video device to get auto exposure value. " },
  {"set_exposure_absolute", (PyCFunction)Video_device_set_exposure_absolute, METH_VARARGS,
       "set_exposure_absolute(exptime) -> exptime \n\n"
       "Request the video device to set exposure time to value. The device may "
       "choose another value than requested and will return its choice. " },
  {"get_exposure_absolute", (PyCFunction)Video_device_get_exposure_absolute, METH_NOARGS,
       "get_exposure_absolute() -> exptime \n\n"
       "Request the video device to get exposure time value. " },
  {"set_focus_auto", (PyCFunction)Video_device_set_focus_auto, METH_VARARGS,
       "set_auto_focus_auto(autofocus) -> autofocus \n\n"
       "Request the video device to set auto focuse on or off. The device may "
       "choose another value than requested and will return its choice. " },
  {"get_focus_auto", (PyCFunction)Video_device_get_focus_auto, METH_NOARGS,
       "get_focus_auto() -> autofocus \n\n"
       "Request the video device to get auto focus value. " },
  {"start", (PyCFunction)Video_device_start, METH_NOARGS,
       "start()\n\n"
       "Start video capture."},
  {"stop", (PyCFunction)Video_device_stop, METH_NOARGS,
       "stop()\n\n"
       "Stop video capture."},
  {"create_buffers", (PyCFunction)Video_device_create_buffers, METH_VARARGS,
       "create_buffers(count)\n\n"
       "Create buffers used for capturing image data. Can only be called once "
       "for each video device object."},
  {"queue_all_buffers", (PyCFunction)Video_device_queue_all_buffers,
       METH_NOARGS,
       "queue_all_buffers()\n\n"
       "Let the video device fill all buffers created."},
  {"read", (PyCFunction)Video_device_read, METH_NOARGS,
       "read() -> string\n\n"
       "Reads image data from a buffer that has been filled by the video "
       "device. The image data is in RGB och YUV420 format as decided by "
       "'set_format'. The buffer is removed from the queue. Fails if no buffer "
       "is filled. Use select.select to check for filled buffers."},
  {"read_and_queue", (PyCFunction)Video_device_read_and_queue, METH_NOARGS,
       "read_and_queue()\n\n"
       "Same as 'read', but adds the buffer back to the queue so the video "
       "device can fill it again."},
  {NULL}
};

static PyTypeObject Video_device_type = {
#if PY_MAJOR_VERSION < 3
  PyObject_HEAD_INIT(NULL) 0,
#else
  PyVarObject_HEAD_INIT(NULL, 0)
#endif
      "v4l2capture.Video_device", sizeof(Video_device), 0,
      (destructor)Video_device_dealloc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, Py_TPFLAGS_DEFAULT, "Video_device(path)\n\nOpens the video device at "
      "the given path and returns an object that can capture images. The "
      "constructor and all methods except close may raise IOError.", 0, 0, 0,
      0, 0, 0, Video_device_methods, 0, 0, 0, 0, 0, 0, 0,
      (initproc)Video_device_init
};

static PyMethodDef module_methods[] = {
  {NULL}
};

#if PY_MAJOR_VERSION < 3
PyMODINIT_FUNC initv4l2capture(void)
#else
PyMODINIT_FUNC PyInit_v4l2capture(void)
#endif
{
  Video_device_type.tp_new = PyType_GenericNew;

  if(PyType_Ready(&Video_device_type) < 0)
    {
#if PY_MAJOR_VERSION < 3
      return;
#else
      return NULL;
#endif
    }

  PyObject *module;

#if PY_MAJOR_VERSION < 3
  module = Py_InitModule3("v4l2capture", module_methods,
      "Capture video with video4linux2.");
#else
  static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "v4l2capture",
    "Capture video with video4linux2.",
    -1,
    module_methods,
    NULL,
    NULL,
    NULL,
    NULL
  };
  module = PyModule_Create(&moduledef);
#endif

  if(!module)
    {
#if PY_MAJOR_VERSION < 3
      return;
#else
      return NULL;
#endif
    }

  Py_INCREF(&Video_device_type);
  PyModule_AddObject(module, "Video_device", (PyObject *)&Video_device_type);
#if PY_MAJOR_VERSION >= 3
  return module;
#endif
}

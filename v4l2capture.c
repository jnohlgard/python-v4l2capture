// python-v4l2capture
// Python extension to capture video with video4linux2
//
// 2009 Fredrik Portstrom
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

#define ASSERT_OPEN if(self->fd < 0)					\
    {									\
      PyErr_SetString(PyExc_ValueError,					\
	  "I/O operation on closed file");				\
      return NULL;							\
    }

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

  self->ob_type->tp_free((PyObject *)self);
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
  return PyInt_FromLong(self->fd);
}

static PyObject *Video_device_get_info(Video_device *self)
{
  ASSERT_OPEN;
  struct v4l2_capability caps;

  if(my_ioctl(self->fd, VIDIOC_QUERYCAP, &caps))
    {
      return NULL;
    }

  return Py_BuildValue("sss", caps.driver, caps.card, caps.bus_info);
}

static PyObject *Video_device_set_format(Video_device *self, PyObject *args)
{
  int size_x;
  int size_y;

  if(!PyArg_ParseTuple(args, "ii", &size_x, &size_y))
    {
      return NULL;
    }

  struct v4l2_format format;
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = size_x;
  format.fmt.pix.height = size_y;
#ifdef USE_LIBV4L
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
#else
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
#endif
  format.fmt.pix.field = V4L2_FIELD_INTERLACED;
  format.fmt.pix.bytesperline = 0;

  if(my_ioctl(self->fd, VIDIOC_S_FMT, &format))
    {
      return NULL;
    }

  return Py_BuildValue("ii", format.fmt.pix.width, format.fmt.pix.height);
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
  reqbuf.count = 4;
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
  PyObject *result = PyString_FromStringAndSize(
      self->buffers[buffer.index].start, self->buffers[buffer.index].length);

  if(!result)
    {
      return NULL;
    }
#else
  // Convert buffer from YUYV to RGB.
  // For the byte order, see: http://v4l2spec.bytesex.org/spec/r4339.htm
  // For the color conversion, see: http://v4l2spec.bytesex.org/spec/x2123.htm
  int length = self->buffers[buffer.index].length / 4 * 6;
  PyObject *result = PyString_FromStringAndSize(NULL, length);

  if(!result)
    {
      return NULL;
    }

  char *rgb = PyString_AS_STRING(result);
  char *rgb_max = rgb + length;
  unsigned char *yuyv = self->buffers[buffer.index].start;

#define CLAMP (c <= 0 ? 0 : c >= 65025 ? 255 : c >> 8)
  while(rgb < rgb_max)
    {
      int u = yuyv[1] - 128;
      int v = yuyv[3] - 128;
      int uv = 100 * u + 208 * v;
      u *= 516;
      v *= 409;

      int y = 298 * (*yuyv - 16);
      int c = y + v;
      *rgb = CLAMP;
      c = y - uv;
      rgb[1] = CLAMP;
      c = y + u;
      rgb[2] = CLAMP;

      y = 298 * (yuyv[2] - 16);
      c = y + v;
      rgb[3] = CLAMP;
      c = y - uv;
      rgb[4] = CLAMP;
      c = y + u;
      rgb[5] = CLAMP;

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
       "get_info() -> driver, card, bus_info\n\n"
       "Returns three strings with information about the video device."},
  {"set_format", (PyCFunction)Video_device_set_format, METH_VARARGS,
       "set_format(size_x, size_y) -> size_x, size_y\n\n"
       "Request the video device to set image size. The device may choose "
       "another size than requested and will return its choice."},
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
       "Reads RGB image data from a buffer that has been filled by the video "
       "device. The buffer is removed from the queue. Fails if no buffer is "
       "filled. Use select.select to check for filled buffers."},
  {"read_and_queue", (PyCFunction)Video_device_read_and_queue, METH_NOARGS,
       "read_and_queue()\n\n"
       "Same as 'read', but adds the buffer back to the queue so the video "
       "device can fill it again."},
  {NULL}
};

static PyTypeObject Video_device_type = {
  PyObject_HEAD_INIT(NULL)
      0, "v4l2capture.Video_device", sizeof(Video_device), 0,
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

PyMODINIT_FUNC initv4l2capture(void)
{
  Video_device_type.tp_new = PyType_GenericNew;

  if(PyType_Ready(&Video_device_type) < 0)
    {
      return;
    }

  PyObject *module = Py_InitModule3("v4l2capture", module_methods,
      "Capture video with video4linux2.");

  if(!module)
    {
      return;
    }

  Py_INCREF(&Video_device_type);
  PyModule_AddObject(module, "Video_device", (PyObject *)&Video_device_type);
}

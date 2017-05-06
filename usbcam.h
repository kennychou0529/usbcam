// Get the video 4 linux 2 development libraries (v4l2)
//   $ sudo apt-get install libv4l-dev
//   $ sudo apt-get install v4l-utils
//
// Get the turbojpeg library:
//   (See https://github.com/libjpeg-turbo/libjpeg-turbo/blob/master/BUILDING.md)
//   $ git clone https://github.com/libjpeg-turbo/libjpeg-turbo
//   $ cd libjpeg-turbo
//   $ autoreconf -fiv
//   $ mkdir build
//   $ cd build
//   $ sh ../configure
//   $ make
//   $ make install prefix=/usr/local libdir=/usr/local/lib64
//
// Get the turbojpeg documentation
//   http://www.libjpeg-turbo.org/Documentation/Documentation
//
// Compiler flags
//   g++ ... -lv4l2 -lturbojpeg

#include <sys/time.h>
struct usbcam_opt_t
{
    const char *device_name; // e.g. /dev/video0

    // The driver does not overwrite buffers with latest data:
    // Therefore, you should request as many buffers as you expect
    // processing time to take. For example, if you need 100 ms to
    // process one frame and the camera gives one frame every 30 ms,
    // then it will fill up three buffers while you process. If you
    // requested less than three buffers you will not get the latest
    // frame when you ask for the next frame!
    int buffers;

    // Pixel formats specified as codes of four characters, and
    // a predefined list of formats can be found in videodev2.h
    // (http://lxr.free-electrons.com/source/include/uapi/linux/videodev2.h#L616)
    // You can find out what formats your camera supports with
    // $ v4l2-ctl -d /dev/video0 --list-formats-ext
    unsigned int pixel_format; // e.g. V4L2_PIX_FMT_MJPEG
    unsigned int width;
    unsigned int height;
};

void usbcam_cleanup();
void usbcam_init(usbcam_opt_t opt);
void usbcam_lock(unsigned char **data, unsigned int *size, timeval *timestamp=0);
void usbcam_unlock();

//
// Implementation
//

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>      // printf
#include <stdlib.h>     // exit, EXIT_FAILURE
#include <string.h>     // strerror
#include <fcntl.h>      // O_RDWR
#include <errno.h>      // errno
#include <sys/mman.h>   // mmap, munmap
#include <linux/videodev2.h>
#include <libv4l2.h>

#define usbcam_max_buffers 128
#define usbcam_assert(CONDITION, MESSAGE) { if (!(CONDITION)) { printf("[usbcam.h] Error at line %d: %s\n", __LINE__, MESSAGE); exit(EXIT_FAILURE); } }
#ifdef USBCAM_DEBUG
#define usbcam_debug(...) { printf("[usbcam.h] "); printf(__VA_ARGS__); }
#else
#define usbcam_debug(...) { }
#endif

static int          usbcam_has_mmap = 0;
static int          usbcam_has_dqbuf = 0;
static int          usbcam_has_fd = 0;
static int          usbcam_has_stream = 0;
static int          usbcam_fd = 0;
static int          usbcam_buffers = 0;
static void        *usbcam_buffer_start[usbcam_max_buffers] = {0};
static unsigned int usbcam_buffer_length[usbcam_max_buffers] = {0};
static v4l2_buffer  usbcam_dqbuf = {0};

void usbcam_ioctl(int request, void *arg)
{
    usbcam_assert(usbcam_has_fd, "The camera device has not been opened yet!");
    int r;
    do
    {
        r = v4l2_ioctl(usbcam_fd, request, arg);
    } while (r == -1 && ((errno == EINTR) || (errno == EAGAIN)));
    if (r == -1)
    {
        printf("[usbcam.h] USB request failed (%d): %s\n", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

void usbcam_cleanup()
{
    // return any buffers we have dequeued (not sure if this is necessary)
    if (usbcam_has_dqbuf)
    {
        usbcam_debug("Requeuing buffer\n");
        usbcam_ioctl(VIDIOC_QBUF, &usbcam_dqbuf);
        usbcam_has_dqbuf = 0;
    }

    // free buffers
    if (usbcam_has_mmap)
    {
        usbcam_debug("Deallocating mmap\n");
        for (int i = 0; i < usbcam_buffers; i++)
            munmap(usbcam_buffer_start[i], usbcam_buffer_length[i]);
        usbcam_has_mmap = 0;
    }

    // turn off streaming
    if (usbcam_has_stream)
    {
        usbcam_debug("Turning off stream (if this freezes send me a message)\n");
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        usbcam_ioctl(VIDIOC_STREAMOFF, &type);
        usbcam_has_stream = 0;
    }

    if (usbcam_has_fd)
    {
        usbcam_debug("Closing fd\n");
        close(usbcam_fd);
        usbcam_has_fd = 0;
    }
}

void usbcam_init(usbcam_opt_t opt)
{
    usbcam_cleanup();
    usbcam_assert(opt.buffers <= usbcam_max_buffers, "You requested too many buffers");
    usbcam_assert(opt.buffers > 0, "You need atleast one buffer");

    // Open the device
    usbcam_fd = v4l2_open(opt.device_name, O_RDWR, 0);
    usbcam_assert(usbcam_fd >= 0, "Failed to open device");
    usbcam_has_fd = 1;

    // set format
    {
        v4l2_format fmt = {0};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.pixelformat = opt.pixel_format;
        fmt.fmt.pix.width = opt.width;
        fmt.fmt.pix.height = opt.height;
        usbcam_ioctl(VIDIOC_S_FMT, &fmt);

        usbcam_assert(fmt.fmt.pix.pixelformat == opt.pixel_format, "Did not get the requested format");
        usbcam_assert(fmt.fmt.pix.width == opt.width, "Did not get the requested width");
        usbcam_assert(fmt.fmt.pix.height == opt.height, "Did not get the requested height");
    }

    usbcam_debug("Opened device (%s %dx%d)\n", opt.device_name, opt.width, opt.height);

    // tell the driver how many buffers we want
    {
        v4l2_requestbuffers request = {0};
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        request.count = opt.buffers;
        usbcam_ioctl(VIDIOC_REQBUFS, &request);

        usbcam_assert(request.count == opt.buffers, "Did not get the requested number of buffers");
    }

    // allocate buffers
    for (int i = 0; i < opt.buffers; i++)
    {
        v4l2_buffer info = {0};
        info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        info.memory = V4L2_MEMORY_MMAP;
        info.index = i;
        usbcam_ioctl(VIDIOC_QUERYBUF, &info);

        usbcam_buffer_length[i] = info.length;
        usbcam_buffer_start[i] = mmap(
            NULL,
            info.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            usbcam_fd,
            info.m.offset
        );

        usbcam_assert(usbcam_buffer_start[i] != MAP_FAILED, "Failed to allocate memory for buffers");
    }

    usbcam_buffers = opt.buffers;
    usbcam_has_mmap = 1;

    // start streaming
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        usbcam_ioctl(VIDIOC_STREAMON, &type);
    }

    usbcam_has_stream = 1;

    // queue buffers
    for (int i = 0; i < opt.buffers; i++)
    {
        v4l2_buffer info = {0};
        info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        info.memory = V4L2_MEMORY_MMAP;
        info.index = i;
        usbcam_ioctl(VIDIOC_QBUF, &info);
    }
}

void usbcam_unlock()
{
    if (usbcam_has_dqbuf)
    {
        usbcam_ioctl(VIDIOC_QBUF, &usbcam_dqbuf);
        usbcam_has_dqbuf = 0;
    }
}

void usbcam_lock(unsigned char **data, unsigned int *size, timeval *timestamp)
{
    usbcam_assert(!usbcam_has_dqbuf, "You forgot to unlock the previous frame");
    usbcam_assert(usbcam_has_fd, "Camera device not open");
    usbcam_assert(usbcam_has_mmap, "Buffers not allocated");
    usbcam_assert(usbcam_has_stream, "Stream not begun");

    // dequeue all the buffers and select the one with latest data
    v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    {
        // get a buffer
        usbcam_ioctl(VIDIOC_DQBUF, &buf);

        // check if there are more buffers available
        int r = 1;
        while (r == 1)
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(usbcam_fd, &fds);
            timeval tv; // if both fields = 0, select returns immediately
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            r = select(usbcam_fd + 1, &fds, NULL, NULL, &tv); // todo: what if r == -1?
            if (r == 1)
            {
                // queue the previous buffer
                usbcam_ioctl(VIDIOC_QBUF, &buf);

                // get a new buffer
                usbcam_ioctl(VIDIOC_DQBUF, &buf);
            }
        }
    }

    *timestamp = buf.timestamp;
    *data = (unsigned char*)usbcam_buffer_start[buf.index];
    *size = buf.bytesused;

    usbcam_dqbuf = buf;
    usbcam_has_dqbuf = 1;
}
#include <stdio.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

#define VIDEO_DEV "/dev/video10"


struct buffer {
    void* start;
    int length;
    struct v4l2_buffer inner;
    struct v4l2_plane plane;
};

void map(int fd, uint32_t type, struct buffer* buffer) {
    struct v4l2_buffer *inner = &buffer->inner;
    
    memset(inner, 0, sizeof(*inner));
    inner->type = type;
    inner->memory = V4L2_MEMORY_MMAP;
    inner->index = 0;
    inner->length = 1;
    inner->m.planes = &buffer->plane;
    
    ioctl(fd, VIDIOC_QUERYBUF, inner);
    
    buffer->length = inner->m.planes[0].length;
    buffer->start = mmap(NULL, buffer->length, PROT_READ | PROT_WRITE, 
                         MAP_SHARED, fd, inner->m.planes[0].m.mem_offset);
}

struct v4l2_format iformat, oformat;
struct v4l2_requestbuffers buf;
struct buffer output;
struct buffer capture;

int init_v4l2()
{
    int fd;

    fd = open(VIDEO_DEV, O_RDWR);
    if (fd < 0) {
        printf("can't open %s\n", VIDEO_DEV);
        return -1;
    }

    memset(&iformat, 0, sizeof(struct v4l2_format));
    memset(&oformat, 0, sizeof(struct v4l2_format));
    iformat.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    iformat.fmt.pix_mp.width = 640;
    iformat.fmt.pix_mp.height = 480;
    iformat.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    iformat.fmt.pix_mp.num_planes = 1;
    iformat.fmt.pix_mp.plane_fmt[0].sizeimage = 64 * 1024;
    if (ioctl(fd, VIDIOC_S_FMT, &iformat) < 0) {
        printf("VIDIOC_S_FMT failed\n");
        return -1;
    }
    if (ioctl(fd, VIDIOC_G_FMT, &iformat) < 0) {
        printf("VIDIOC_G_FMT failed\n");
        return -1;
    }

    oformat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    //iformat.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YVU420;
    oformat.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_Y12;
    oformat.fmt.pix_mp.width = 640;
    oformat.fmt.pix_mp.height = 480;

    if (ioctl(fd, VIDIOC_S_FMT, &oformat) < 0) {
        printf("oformat: VIDIOC_S_FMT failed\n");
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    buf.memory = V4L2_MEMORY_MMAP;
    buf.count = 1;
    printf("buf count = %d\n", buf.count);

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd, VIDIOC_REQBUFS, &buf) < 0) {
        printf("VIDIOC_REQBUFS failed\n");
        return -1;
    }
    
    map(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &output);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_REQBUFS, &buf) < 0) {
        printf("VIDIOC_REQBUFS failed\n");
        return -1;
    }
    map(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &capture);

    printf("output buffer size: %d\n", output.length);
    printf("capture buffer size: %d\n", capture.length);

    return fd;
}

int first = 1;
uint8_t frame[32768];

int get_frame(FILE *h, uint8_t *buf, uint32_t len)
{
    uint32_t w = 0xffffffff;
    int index = 0;
    int i2 = 4;
    int ret;
    int idx = 0;
            
    *(uint32_t*)buf = 0x01000000;
    *(uint32_t*)buf = 0x01000000;
    idx = 4;

    while(idx < len) {
        w <<= 8;
        if (fread(&w, 1, 1, h) != 1) break;
        index++;
        if ((w & 0x00ffffff) == 0x00000001) {
            if (!first) {
                idx -= 4;
                i2 = index;
                return idx + 1;
            }
            first = 0;
            *(uint32_t*)buf = 0x01000000;
            idx = 4;
            w = 0xffffffff;
        }
        else {
            buf[idx++] = (w & 0xff);
        }
    }

    return -1;
}

int main(int argc, char *argv[])
{
    int ret;
    int type;
    int i;
    int startup;
    // fill buffer
    FILE *h, *h2;
    int fd = 0;

    struct pollfd fds;

    fd = init_v4l2();
    if (fd < 0) return 1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fds.fd = fd;
    fds.events = POLLIN;

    h = fopen("frame.h264", "r");
    if (!h) {
        printf("Can't open frame.h264 file\n");
        return 1;
    }
    h2 = fopen("frame.yuv", "w");
    if (!h2) {
        printf("Can't open frame.yuv file\n");
        return 1;
    }

    ret = ioctl(fd, VIDIOC_QBUF, &capture.inner);
    if (ret < 0 && ret != EAGAIN) {
        printf("capture: VIDIOC_QUERYBUF failed\n");
        return 1;
    }
    if (ioctl(fd, VIDIOC_QBUF, &output.inner) < 0) {
        printf("VIDIOC_QBUF failed\n");
        return 1;
    }
    printf("STREAMON\n");
    /* start streaming */
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("output: VIDIOC_STREAMON failed\n");
        return 1;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("capture: VIDIOC_STREAMON failed\n");
        return 1;
    }
    while(1) {
        ret = poll(&fds, 1, 1000);
        if (ret < 0) {
            perror("poll");
            return 1;
        }
        if (ret == 0) {
            printf("timeout\n");
            startup = 1;
        }
        ret = ioctl(fd, VIDIOC_DQBUF, &capture.inner);
        if (ret < 0 && errno != EAGAIN) {
            printf("capture: VIDIOC_QUERYBUF failed\n");
            return 1;
        }
        if (ret >= 0) {
            printf("save frame of %d bytes\n", capture.length);
            if (ioctl(fd, VIDIOC_QBUF, &capture.inner) < 0) {
                printf("VIDIOC_QBUF failed\n");
                return 1;
            }
        }
        ret = ioctl(fd, VIDIOC_DQBUF, &output.inner);
        if (ret < 0 && errno != EAGAIN) {
            printf("output: VIDIOC_DQBUF failed\n");
            perror("ioctl");
            return 1;
        }
        if (ret >= 0 || startup) {
            startup = 0;
            ret = get_frame(h, frame, 32768);
            if (ret < 0) {
                printf("file error\n");
                return 1;
            }
            if (ret == 0) break;
            memcpy(output.start, frame, ret);
            printf("read %d bytes\n", ret);
            output.plane.bytesused = ret;
            if (ioctl(fd, VIDIOC_QBUF, &output.inner) < 0) {
                printf("VIDIOC_QBUF failed\n");
                return 1;
            }
        }
        
    }

    /* stop streaming */
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        printf("VIDIOC_STREAMOFF failed\n");
        return 1;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        printf("VIDIOC_STREAMOFF failed\n");
        return 1;
    }

exit:
    if (h) fclose(h);
    if (h2) fclose(h2);
    if (fd) close(fd);
    printf("done.\n");
    return 0;
}



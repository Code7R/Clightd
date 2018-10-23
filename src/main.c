/* BEGIN_COMMON_COPYRIGHT_HEADER
 * 
 * clightd: C bus interface for linux to change screen brightness and capture frames from webcam device.
 * https://github.com/FedeDP/Clight/tree/master/clightd
 *
 * Copyright (C) 2017  Federico Di Pierro <nierro92@gmail.com>
 *
 * This file is part of clightd.
 * clightd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * END_COMMON_COPYRIGHT_HEADER */

#include <gamma.h>
#include <dpms.h>
#include <backlight.h>
#include <idle.h>
#include <udev.h>
#include <sensor.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <signal.h>

static int get_version( sd_bus *b, const char *path, const char *interface, const char *property,
                        sd_bus_message *reply, void *userdata, sd_bus_error *error);
static void bus_cb(void);
static void signal_cb(void);
static void set_pollfd(void);
static void main_poll(void);
static void close_mainp(void);

enum poll_idx {
    BUS,
    SIGNAL,
    BRIGHT_SMOOTH,
#ifdef GAMMA_PRESENT
    GAMMA_SMOOTH,
#endif
    WEBCAM_MON,
    ALS_MON,
    POLL_SIZE };
enum quit_codes { LEAVE_W_ERR = -1, SIGNAL_RCV = 1 };

static const char object_path[] = "/org/clightd/backlight";
static const char bus_interface[] = "org.clightd.backlight";
static struct pollfd main_p[POLL_SIZE];
static int quit;

sd_bus *bus;
struct udev *udev;

static const sd_bus_vtable clightd_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "s", get_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("SetBrightness", "d(bdu)s", "b", method_setbrightness, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetBrightness", "s", "a(sd)", method_getbrightness, SD_BUS_VTABLE_UNPRIVILEGED),
#ifdef GAMMA_PRESENT
    SD_BUS_METHOD("SetGamma", "ssi(buu)", "b", method_setgamma, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetGamma", "ss", "i", method_getgamma, SD_BUS_VTABLE_UNPRIVILEGED),
#endif
    /* Webcam related methods */
    SD_BUS_METHOD("CaptureWebcam", "si", "sad", method_capturesensor, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("IsWebcamAvailable", "s", "sb", method_issensoravailable, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("WebcamChanged", "ss", 0),
    /* ALS related methods */
    SD_BUS_METHOD("CaptureAls", "si", "sad", method_capturesensor, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("IsAlsAvailable", "s", "sb", method_issensoravailable, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("AlsChanged", "ss", 0),
    /* Sensors related methods -> ie: generic to use first matching sensor */
    SD_BUS_METHOD("CaptureSensor", "si", "sad", method_capturesensor, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("IsSensorAvailable", "s", "sb", method_issensoravailable, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("SensorChanged", "ss", 0),
#ifdef DPMS_PRESENT
    SD_BUS_METHOD("GetDpms", "ss", "i", method_getdpms, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetDpms", "ssi", "i", method_setdpms, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetDpmsTimeouts", "ss", "iii", method_getdpms_timeouts, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetDpmsTimeouts", "ssiii", "iii", method_setdpms_timeouts, SD_BUS_VTABLE_UNPRIVILEGED),
#endif
#ifdef IDLE_PRESENT
    SD_BUS_METHOD("GetIdleTime", "ss", "i", method_get_idle_time, SD_BUS_VTABLE_UNPRIVILEGED),
#endif
    SD_BUS_VTABLE_END
};

static int get_version( sd_bus *b, const char *path, const char *interface, const char *property,
                        sd_bus_message *reply, void *userdata, sd_bus_error *error) {
    return sd_bus_message_append(reply, "s", VERSION);
}

static void bus_cb(void) {
    int r;
    do {
        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            fprintf(stderr, "Failed to process bus: %s\n", strerror(-r));
            quit = LEAVE_W_ERR;
        }
    } while (r > 0);
}

/*
 * if received an external SIGINT or SIGTERM,
 * just switch the quit flag to 1 and print to stdout.
 */
static void signal_cb(void) {
    struct signalfd_siginfo fdsi;
    ssize_t s;
    
    s = read(main_p[SIGNAL].fd, &fdsi, sizeof(struct signalfd_siginfo));
    if (s != sizeof(struct signalfd_siginfo)) {
        fprintf(stderr, "an error occurred while getting signalfd data.\n");
    }
    printf("Received signal %d. Leaving.\n", fdsi.ssi_signo);
    quit = SIGNAL_RCV;
}

static void set_pollfd(void) {
    int busfd = sd_bus_get_fd(bus);
    main_p[BUS] = (struct pollfd) {
        .fd = busfd,
        .events = POLLIN,
    };
    
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, 0);
    main_p[SIGNAL] = (struct pollfd) {
        .fd = sigfd,
        .events = POLLIN,
    };
    int bright_smooth_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    main_p[BRIGHT_SMOOTH] = (struct pollfd) {
        .fd = bright_smooth_fd,
        .events = POLLIN,
    };
    set_brightness_smooth_fd(bright_smooth_fd);
    
#ifdef GAMMA_PRESENT
    int gamma_smooth_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    main_p[GAMMA_SMOOTH] = (struct pollfd) {
        .fd = gamma_smooth_fd,
        .events = POLLIN,
    };
    set_gamma_smooth_fd(gamma_smooth_fd);
#endif
    main_p[WEBCAM_MON] = (struct pollfd) {
        .fd = sensor_get_monitor(WEBCAM),
        .events = POLLIN,
    };
    main_p[ALS_MON] = (struct pollfd) {
        .fd = sensor_get_monitor(ALS),
        .events = POLLIN,
    };
}

/*
 * Listens on fds
 */
static void main_poll(void) {
    while (!quit) {
        int r = poll(main_p, POLL_SIZE, -1);
        
        if (r == -1 && errno != EINTR) {
            fprintf(stderr, "%s\n", strerror(errno));
            quit = LEAVE_W_ERR;
        }
        
        for (int i = 0; i < POLL_SIZE && !quit && r > 0; i++) {
            if (main_p[i].revents & POLLIN) {
                switch (i) {
                case BUS:
                    bus_cb();
                    break;
                case SIGNAL:
                    signal_cb();
                    break;
                case BRIGHT_SMOOTH:
                    brightness_smooth_cb();
                    break;
#ifdef GAMMA_PRESENT
                case GAMMA_SMOOTH:
                    gamma_smooth_cb();
                    break;
#endif
                case WEBCAM_MON:
                case ALS_MON: {
                    struct udev_device *dev = NULL;
                    sensor_receive_device(i == WEBCAM_MON ? WEBCAM : ALS, &dev);
                    if (dev) {
                        const char *signal = i == WEBCAM_MON ? "WebcamChanged" : "AlsChanged";
                        sd_bus_emit_signal(bus, object_path, bus_interface, signal, "ss", udev_device_get_devnode(dev), udev_device_get_action(dev));
                        /* SensorChanged is emitted for every sensor */
                        sd_bus_emit_signal(bus, object_path, bus_interface, "SensorChanged", "ss", udev_device_get_devnode(dev), udev_device_get_action(dev));
                        udev_device_unref(dev);
                    }
                    break;
                }
                }
                r--;
            }
        }
    }
}

static void close_mainp(void) {
    for (int i = BUS; i < POLL_SIZE; i++) {
        if (main_p[i].fd > 0) {
            close(main_p[i].fd);
        }
    }
}

int main(void) {
    int r;
    udev = udev_new();
    /* Connect to the system bus */
    r = sd_bus_default_system(&bus);
    if (r < 0) {
        fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-r));
        goto finish;
    }
    
    /* Install the object */
    r = sd_bus_add_object_vtable(bus,
                                 NULL,
                                 object_path,
                                 bus_interface,
                                 clightd_vtable,
                                 NULL);
    if (r < 0) {
        fprintf(stderr, "Failed to issue method call: %s\n", strerror(-r));
        goto finish;
    }
    
    r = sd_bus_request_name(bus, bus_interface, 0);
    if (r < 0) {
        fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-r));
        goto finish;
    }
    
    set_pollfd();
   /*
    * Need to parse initial bus messages 
    * or it'll give "Connection timed out" error
    */
    bus_cb();
    main_poll();
    
finish:
    sd_bus_release_name(bus, bus_interface);
    if (bus) {
        sd_bus_flush_close_unref(bus);
    }
    udev_unref(udev);
    close_mainp();
    return quit == LEAVE_W_ERR ? EXIT_FAILURE : EXIT_SUCCESS;
}

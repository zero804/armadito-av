/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#define _GNU_SOURCE

#include <libarmadito/armadito.h>
#include <armadito-config.h>

#include "core/event.h"
#include "core/handle.h"
#include "core/scanconf.h"
#include "core/scanctx.h"

#include "monitor.h"
#include "response.h"
#include "watchdog.h"
#include "modname.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <sys/fanotify.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

struct fanotify_monitor {
	int enable_permission;

	struct access_monitor *monitor;
	struct armadito *armadito;

	struct a6o_scan_conf *scan_conf;

	pid_t my_pid;

	int fanotify_fd;

	GThreadPool *thread_pool;

	struct watchdog *watchdog;
};

static gboolean fanotify_cb(GIOChannel *source, GIOCondition condition, gpointer data);
static void scan_file_thread_fun(gpointer data, gpointer user_data);

struct fanotify_monitor *fanotify_monitor_new(struct access_monitor *m, struct armadito *u)
{
	struct fanotify_monitor *f = malloc(sizeof(struct fanotify_monitor));

	f->enable_permission = 0;
	f->monitor = m;
	f->armadito = u;

	f->scan_conf = a6o_scan_conf_on_access();
	f->my_pid = getpid();

	return f;
}

int fanotify_monitor_enable_permission(struct fanotify_monitor *f, int enable_permission)
{
	f->enable_permission = enable_permission;

	return f->enable_permission;
}

int fanotify_monitor_is_enable_permission(struct fanotify_monitor *f)
{
	return f->enable_permission;
}

static void display_init_error(void)
{
	a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_ERROR, MODULE_LOG_NAME ": fanotify_init failed (%s)", strerror(errno));

	switch(errno) {
	case EPERM:
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": you must be root or have CAP_SYS_ADMIN capability to enable on-access protection");
		break;
	case ENOSYS:
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": this kernel does not implement fanotify_init()");
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": fanotify is available only if the kernel was configured with CONFIG_FANOTIFY");
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": check your running kernel config, for instance with 'grep FANOTIFY /boot/config-$(uname -r)'");
		break;
	}
}

int fanotify_monitor_start(struct fanotify_monitor *f)
{
	unsigned int flags;
	GIOChannel *fanotify_channel;
	GSource *source;

	flags = ((f->enable_permission) ? FAN_CLASS_CONTENT : FAN_CLASS_NOTIF) | FAN_UNLIMITED_QUEUE | FAN_UNLIMITED_MARKS;
	f->fanotify_fd = fanotify_init(flags, O_LARGEFILE | O_RDONLY);

	if (f->fanotify_fd < 0) {
		display_init_error();

		return -1;
	}

	f->watchdog = watchdog_new(f->fanotify_fd);

	f->thread_pool = g_thread_pool_new(scan_file_thread_fun, f, -1, FALSE, NULL);

	/* add the fanotify file desc to the thread loop */
	fanotify_channel = g_io_channel_unix_new(f->fanotify_fd);

	/* g_io_add_watch(fanotify_channel, G_IO_IN, fanotify_cb, f); */
	source = g_io_create_watch(fanotify_channel, G_IO_IN);
	g_source_set_callback(source, (GSourceFunc)fanotify_cb, f, NULL);
	g_source_attach(source, access_monitor_get_main_context(f->monitor));
	g_source_unref(source);

	a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_INFO, MODULE_LOG_NAME ": started Linux on-access protection with fanotify");

	return 0;
}

static char *get_file_path_from_fd(int fd, char *buffer, size_t buffer_size)
{
	ssize_t len;

	if (fd <= 0)
		return NULL;

	snprintf(buffer, buffer_size, "/proc/self/fd/%d", fd);
	if ((len = readlink(buffer, buffer, buffer_size - 1)) < 0)
		return NULL;

	buffer[len] = '\0';

	return buffer;
}

static void fire_detection_event(struct fanotify_monitor *f, struct a6o_report *report)
{
	struct a6o_detection_event detection_ev;
	struct a6o_event *ev;

	detection_ev.context = CONTEXT_REAL_TIME;
	/* should strdup? */
	detection_ev.path = report->path;
	detection_ev.scan_status = report->status;
	detection_ev.scan_action = report->action;
	detection_ev.module_name = report->module_name;
	detection_ev.module_report = report->module_report;

	ev = a6o_event_new(EVENT_DETECTION, &detection_ev);

	a6o_event_source_fire_event(a6o_get_event_source(f->armadito), ev);
	a6o_event_free(ev);
}

static void scan_file_thread_fun(gpointer data, gpointer user_data)
{
	struct fanotify_monitor *f = (struct fanotify_monitor *)user_data;
	struct a6o_scan_context *file_context = (struct a6o_scan_context *)data;
	struct a6o_report report;
	enum a6o_file_status status;

	a6o_report_init(&report, file_context->path);

	status = a6o_scan_context_scan(file_context, &report);

	if (fanotify_monitor_is_enable_permission(f)) {
		__u32 fan_response = (status == A6O_FILE_MALWARE) ? FAN_DENY : FAN_ALLOW;
		if (watchdog_remove(f->watchdog, file_context->fd, NULL))
			response_write(f->fanotify_fd, file_context->fd, fan_response, file_context->path, "scanned");

		file_context->fd = -1; /* this will prevent a6o_scan_context_destroy from closing the file descriptor twice :( */
	} else {
		enum a6o_log_level log_level = (status == A6O_FILE_MALWARE) ? A6O_LOG_LEVEL_WARNING : A6O_LOG_LEVEL_INFO;
		const char *msg =  (status == A6O_FILE_MALWARE) ? "MALWARE" : "CLEAN";

		a6o_log(A6O_LOG_MODULE, log_level, MODULE_LOG_NAME ": fd %3d path %s: %s (scanned)",
			file_context->fd,
			(file_context->path != NULL) ? file_context->path : "null",
			msg);
	}

	a6o_scan_context_destroy(file_context);
	free(file_context);

	if ((status == A6O_FILE_MALWARE || status == A6O_FILE_SUSPICIOUS)
		&& report.path != NULL)
		fire_detection_event(f, &report);

	a6o_report_destroy(&report);
}

static int stat_check(int fd)
{
	struct stat buf;

	/* the 2 following tests could be removed: */
	/* if file descriptor does not refer to a file, read() will fail inside os_mime_type_guess_fd() */
	/* in this case, mime_type will be null, context_status will be error and response will be ALLOW */
	/* BUT: this would give a lot of warning in os_mime_type_guess() */
	/* and the read() in os_mime_type_guess() could be successfull for a device for instance */
	/* so for now I keep the fstat() */

	if (fstat(fd, &buf) < 0)
		return 1;

	if (!S_ISREG(buf.st_mode))
		return 1;

	return 0;
}

static void fanotify_perm_event_process(struct fanotify_monitor *f, struct fanotify_event_metadata *event, const char *path)
{
	struct a6o_scan_context *file_context;

	if (stat_check(event->fd)) {
		if (watchdog_remove(f->watchdog, event->fd, NULL))
			response_write(f->fanotify_fd, event->fd, FAN_ALLOW, path, "stat failed or not a regular file");
		return;
	}

	file_context = malloc(sizeof(struct a6o_scan_context));

	if (a6o_scan_context_get(file_context, event->fd, path, f->scan_conf, NULL)) {   /* means file must not be scanned */
		if (watchdog_remove(f->watchdog, event->fd, NULL))
			response_write(f->fanotify_fd, event->fd, FAN_ALLOW, path, "not scanned");

		/* FIXME */
		/* should same stuff as in scan_file_thread_fun be applied? */
		/* as response_write closes the file descriptor */
		file_context->fd = -1; /* this will prevent a6o_scan_context_destroy from closing the file descriptor twice :( */
		a6o_scan_context_destroy(file_context);
		free(file_context);

		return;
	}

	/* scan in thread pool */
	g_thread_pool_push(f->thread_pool, file_context, NULL);
}

static void fanotify_notify_event_process(struct fanotify_monitor *f, struct fanotify_event_metadata *event, const char *path)
{
	struct a6o_scan_context *file_context;

	if (stat_check(event->fd)) {
		/* log? */
		return;
	}

	file_context = malloc(sizeof(struct a6o_scan_context));

	if (a6o_scan_context_get(file_context, event->fd, path, f->scan_conf, NULL)) {   /* means file must not be scanned */
		/* log? */
		/* response_write(f->fanotify_fd, event->fd, FAN_ALLOW, path, "not scanned"); */

		a6o_scan_context_destroy(file_context);
		free(file_context);

		return;
	}

	/* scan in thread pool */
	g_thread_pool_push(f->thread_pool, file_context, NULL);
}

static void fanotify_pass_1(struct fanotify_monitor *f, struct fanotify_event_metadata *buf, ssize_t len)
{
	struct fanotify_event_metadata *event;

	/* first pass: allow all PERM events from myself, enqueue other PERM events */
	for(event = buf; FAN_EVENT_OK(event, len); event = FAN_EVENT_NEXT(event, len)) {
		if ((event->mask & FAN_OPEN_PERM)) {
			if (event->pid == f->my_pid)
				response_write(f->fanotify_fd, event->fd, FAN_ALLOW, NULL, "PID is myself");
			else
				watchdog_add(f->watchdog, event->fd);
		}
	}
}

static void fanotify_pass_2(struct fanotify_monitor *f, struct fanotify_event_metadata *buf, ssize_t len)
{
	struct fanotify_event_metadata *event;

	/* second pass: process all OPEN_PERM or OPEN events that were not from myself and all other events */
	for(event = buf; FAN_EVENT_OK(event, len); event = FAN_EVENT_NEXT(event, len)) {
		char file_path[PATH_MAX + 1];
		char *p;

		if (event->pid == f->my_pid)
			continue;

		p = get_file_path_from_fd(event->fd, file_path, PATH_MAX);

		if ((event->mask & FAN_OPEN_PERM))
			fanotify_perm_event_process(f, event, p);
		else if ((event->mask & FAN_OPEN))
			fanotify_notify_event_process(f, event, p);
	}
}

/* Size of buffer to use when reading fanotify events */
/* 8192 is recommended by fanotify man page */
#define FANOTIFY_BUFFER_SIZE 8192

static gboolean fanotify_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
	struct fanotify_monitor *f = (struct fanotify_monitor *)data;
	char buf[FANOTIFY_BUFFER_SIZE];
	ssize_t len;

	assert(g_main_context_is_owner(access_monitor_get_main_context(f->monitor)));

	if ((len = read(f->fanotify_fd, buf, FANOTIFY_BUFFER_SIZE)) < 0) {
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_ERROR, MODULE_LOG_NAME ": error reading fanotify event descriptor (%s)", strerror(errno));
		return TRUE;
	}

	if (len) {
		fanotify_pass_1(f, (struct fanotify_event_metadata *)buf, len);
		fanotify_pass_2(f, (struct fanotify_event_metadata *)buf, len);
	}

	return TRUE;
}

static void display_mark_error(const char *path, int enable_permission, const char *adding_or_removing, const char *dir_or_mount)
{
	a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": %s fanotify mark for %s %s failed (%s)", adding_or_removing, dir_or_mount, path, strerror(errno));

	if (enable_permission && errno == EINVAL) {
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": may be this kernel does not support fanotify access permissions?");
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": the fanotify access permissions are available only if the kernel was configured with CONFIG_FANOTIFY_ACCESS_PERMISSIONS");
		a6o_log(A6O_LOG_MODULE, A6O_LOG_LEVEL_WARNING, MODULE_LOG_NAME ": check your running kernel config, for instance with 'grep FANOTIFY /boot/config-$(uname -r)'");
	}
}

int fanotify_monitor_mark_directory(struct fanotify_monitor *f, const char *path)
{
	uint64_t fan_mask;
	int r;

	fan_mask = ((f->enable_permission) ? FAN_OPEN_PERM : FAN_OPEN) | FAN_EVENT_ON_CHILD;

	r = fanotify_mark(f->fanotify_fd, FAN_MARK_ADD, fan_mask, AT_FDCWD, path);

	if (r < 0)
		display_mark_error(path, f->enable_permission, "adding", "directory");

	return r;
}

int fanotify_monitor_unmark_directory(struct fanotify_monitor *f, const char *path)
{
	uint64_t fan_mask;
	int r;

	fan_mask = ((f->enable_permission) ? FAN_OPEN_PERM : FAN_OPEN) | FAN_EVENT_ON_CHILD;

	r = fanotify_mark(f->fanotify_fd, FAN_MARK_REMOVE, fan_mask, AT_FDCWD, path);

	if (r < 0)
		display_mark_error(path, f->enable_permission, "removing", "directory");

	return r;
}

int fanotify_monitor_mark_mount(struct fanotify_monitor *f, const char *path)
{
	uint64_t fan_mask;
	int r;

	fan_mask = (f->enable_permission) ? FAN_OPEN_PERM : FAN_OPEN;

	r = fanotify_mark(f->fanotify_fd, FAN_MARK_ADD | FAN_MARK_MOUNT, fan_mask, AT_FDCWD, path);

	if (r < 0)
		display_mark_error(path, f->enable_permission, "adding", "mount point");

	return r;
}

int fanotify_monitor_unmark_mount(struct fanotify_monitor *f, const char *path)
{
	uint64_t fan_mask;
	int r;

	fan_mask = (f->enable_permission) ? FAN_OPEN_PERM : FAN_OPEN;

	r = fanotify_mark(f->fanotify_fd, FAN_MARK_REMOVE | FAN_MARK_MOUNT, fan_mask, AT_FDCWD, path);

	if (r < 0)
		display_mark_error(path, f->enable_permission, "removing", "mount point");

	return r;
}

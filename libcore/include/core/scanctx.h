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

#ifndef ARMADITO_CORE_FILECTX_H
#define ARMADITO_CORE_FILECTX_H

#include <core/report.h>
#include <core/scanconf.h>

enum a6o_scan_context_status {
	A6O_SC_MUST_SCAN = 0,                     /* !< file must be scanned                              */
	A6O_SC_WHITE_LISTED_DIRECTORY,            /* !< a directory ancestor of path is white listed      */
	A6O_SC_FILE_TOO_BIG,                      /* !< file size is >= maximum file size                 */
	A6O_SC_FILE_CACHED,                       /* !< file path is in path cache                        */
	A6O_SC_FILE_TYPE_NOT_SCANNED,             /* !< file mime type has no associated scan modules     */
	A6O_SC_FILE_OPEN_ERROR                    /* !< error when opening the file                      */
};

const char *a6o_scan_context_status_str(enum a6o_scan_context_status status);

struct a6o_scan_context {
	enum a6o_scan_context_status status;
	int fd;
	const char *path;
	const char *mime_type;
	struct a6o_module **applicable_modules;
};

enum a6o_scan_context_status a6o_scan_context_get(struct a6o_scan_context *ctx, int fd, const char *path, struct a6o_scan_conf *conf, struct a6o_report *report);

enum a6o_file_status a6o_scan_context_scan(struct a6o_scan_context *ctx, struct a6o_report *report);

/* FIXME */
/* should it be merged with _destroy ?*/
void a6o_scan_context_close(struct a6o_scan_context *ctx);

void a6o_scan_context_destroy(struct a6o_scan_context *ctx);

#endif

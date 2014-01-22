/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2013 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include <sys/types.h>   /* open(2), lseek(2), */
#include <sys/stat.h>    /* open(2), */
#include <fcntl.h>       /* open(2), */
#include <unistd.h>      /* read(2), readlink(2), close(2), lseek(2), */
#include <errno.h>       /* errno, EACCES, */
#include <assert.h>      /* assert(3), */
#include <linux/limits.h> /* PATH_MAX, */
#include <string.h>      /* strlen(3), strcmp(3), */
#include <stdbool.h>     /* bool, true, false, */
#include <talloc.h>      /* talloc(3), */
#include <archive.h>     /* archive_*(3), */
#include <archive_entry.h> /* archive_entry*(3), */

#include "extension/care/archive.h"
#include "tracee/tracee.h"
#include "cli/notice.h"

typedef struct {
	int (*set_format)(struct archive *);
	int (*add_filter)(struct archive *);
	int hardlink_resolver_strategy;
	const char *options;
	bool self_extractable;
} Format;

/**
 * Move *@cursor backward -- within in the given @string -- if it
 * reads @suffix once moved.
 */
static bool slurp_suffix(const char *string, const char **cursor, const char *suffix)
{
	size_t length;

	length = strlen(suffix);
	if (*cursor - length < string || strncmp(*cursor - length, suffix, length) != 0)
		return false;

	*cursor -= length;
	return true;
}

/**
 * Detect the expected format for the given @string.  This function
 * always updates the @format structure and @suffix_length with the
 * number of characters that describes the parsed format.
 */
static void parse_suffix(const Tracee* tracee, Format *format,
			const char *string, size_t *suffix_length)
{
	const char *cursor;
	bool found;

	bool no_wrapper_found = false;
	bool no_filter_found  = false;
	bool no_format_found  = false;

	cursor = string + strlen(string);
	bzero(format, sizeof(Format));

/* parse_wrapper: */

	found = slurp_suffix(string, &cursor, ".bin");
	if (found) {
		format->self_extractable = true;
		goto parse_filter;
	}

	no_wrapper_found = true;

parse_filter:

	found = slurp_suffix(string, &cursor, ".gz");
	if (found) {
		format->add_filter = archive_write_add_filter_gzip;
		format->options    = "gzip:compression-level=1";
		goto parse_format;
	}

	found = slurp_suffix(string, &cursor, ".lzo");
	if (found) {
		format->add_filter = archive_write_add_filter_lzop;
		format->options	   = "lzop:compression-level=1";
		goto parse_format;
	}

	found = slurp_suffix(string, &cursor, ".tgz");
	if (found) {
		format->add_filter = archive_write_add_filter_gzip;
		format->options    = "gzip:compression-level=1";
		format->set_format = archive_write_set_format_gnutar;
		format->hardlink_resolver_strategy = ARCHIVE_FORMAT_TAR_GNUTAR;
		goto end;
	}

	found = slurp_suffix(string, &cursor, ".tzo");
	if (found) {
		format->add_filter = archive_write_add_filter_lzop;
		format->options    = "lzop:compression-level=1";
		format->set_format = archive_write_set_format_gnutar;
		format->hardlink_resolver_strategy = ARCHIVE_FORMAT_TAR_GNUTAR;
		goto end;
	}

	no_filter_found = true;

parse_format:

	found = slurp_suffix(string, &cursor, ".cpio");
	if (found) {
		format->set_format = archive_write_set_format_cpio;
		format->hardlink_resolver_strategy = ARCHIVE_FORMAT_CPIO_POSIX;
		goto end;
	}

	found = slurp_suffix(string, &cursor, ".tar");
	if (found) {
		format->set_format = archive_write_set_format_gnutar;
		format->hardlink_resolver_strategy = ARCHIVE_FORMAT_TAR_GNUTAR;
		goto end;
	}

	no_format_found = true;

end:
	if (no_filter_found && no_format_found) {
		format->add_filter = archive_write_add_filter_lzop;
		format->options	  = "lzop:compression-level=1";
		format->set_format = archive_write_set_format_cpio;
		format->hardlink_resolver_strategy = ARCHIVE_FORMAT_CPIO_POSIX;
		format->self_extractable = true;

		if (no_wrapper_found)
			notice(tracee, WARNING, USER,
				"unknown suffix, assuming self-extractable format.");

		no_wrapper_found = false;
		no_filter_found  = false;
		no_format_found  = false;
	}

	if (no_format_found) {
		notice(tracee, WARNING, USER, "unknown format, assuming cpio format.");
		format->set_format = archive_write_set_format_cpio;
		format->hardlink_resolver_strategy = ARCHIVE_FORMAT_CPIO_POSIX;

		no_format_found = false;
	}

	*suffix_length = strlen(cursor);
}

/**
 * Copy "/proc/self/exe" into @destination.  This function returns -1
 * if an error occured, otherwise the file descriptor of the
 * destination.
 */
static int copy_self_exe(const Tracee *tracee, const char *destination)
{
	int output_fd;
	int input_fd;
	int status;

	input_fd = open("/proc/self/exe", O_RDONLY);
	if (input_fd < 0) {
		notice(tracee, ERROR, SYSTEM, "can't open '/proc/self/exe'");
		return -1;
	}

	output_fd = open(destination, O_RDWR|O_CREAT|O_TRUNC, S_IRWXU|S_IRGRP|S_IXGRP);
	if (output_fd < 0) {
		notice(tracee, ERROR, SYSTEM, "can't open/create '%s'", destination);
		status = -1;
		goto end;
	}

	while (1) {
		uint8_t buffer[4 * 1024];
		ssize_t size;

		status = read(input_fd, buffer, sizeof(buffer));
		if (status < 0) {
			notice(tracee, ERROR, SYSTEM, "can't read '/proc/self/exe'");
			goto end;
		}

		if (status == 0)
			break;

		size = status;
		status = write(output_fd, buffer, size);
		if (status < 0) {
			notice(tracee, ERROR, SYSTEM, "can't write '%s'", destination);
			goto end;
		}
		if (status != size)
			notice(tracee, WARNING, INTERNAL,
				"wrote %zd bytes instead of %zd", (size_t) status, size);
	}

end:
	(void) close(input_fd);

	if (status < 0) {
		(void) close(output_fd);
		return -1;
	}

	return output_fd;
}

/**
 * Create a new archive structure (memory allocation attached to
 * @context) for the given @output file.  This function returns NULL
 * on error, otherwise the newly allocated archive structure. See
 * parse_suffix() for the meaning of @suffix_length.
 */
Archive *new_archive(TALLOC_CTX *context, const Tracee* tracee,
		const char *output, size_t *suffix_length)
{
	Format format;
	Archive *archive;
	int status;

	assert(output != NULL);

	parse_suffix(tracee, &format, output, suffix_length);

	archive = talloc_zero(context, Archive);
	if (archive == NULL) {
		notice(tracee, ERROR, INTERNAL, "can't allocate archive structure");
		return NULL;
	}
	archive->fd = -1;

	archive->handle = archive_write_new();
	if (archive->handle == NULL) {
		notice(tracee, WARNING, INTERNAL, "can't initialize archive structure");
		return NULL;
	}

	assert(format.set_format != NULL);
	status = format.set_format(archive->handle);
	if (status != ARCHIVE_OK) {
		notice(tracee, ERROR, INTERNAL, "can't set archive format: %s",
			archive_error_string(archive->handle));
		return NULL;
	}

	if (format.hardlink_resolver_strategy != 0) {
		archive->hardlink_resolver = archive_entry_linkresolver_new();
		if (archive->hardlink_resolver != NULL)
			archive_entry_linkresolver_set_strategy(archive->hardlink_resolver,
								format.hardlink_resolver_strategy);
	}

	if (format.add_filter != NULL) {
		status = format.add_filter(archive->handle);
		if (status != ARCHIVE_OK) {
			notice(tracee, ERROR, INTERNAL, "can't add archive filter: %s",
				archive_error_string(archive->handle));
			return NULL;
		}
	}

	if (format.options != NULL) {
		status = archive_write_set_options(archive->handle, format.options);
		if (status != ARCHIVE_OK) {
			notice(tracee, ERROR, INTERNAL, "can't set archive options: %s",
				archive_error_string(archive->handle));
			return NULL;
		}
	}

	if (format.self_extractable) {
		archive->fd = copy_self_exe(tracee, output);
		if (archive->fd < 0)
			return NULL;

		/* Remember where the CARE binary ends.  */
		archive->offset = lseek(archive->fd, 0, SEEK_CUR);

		status = archive_write_open_fd(archive->handle, archive->fd);
	}
	else
		status = archive_write_open_filename(archive->handle, output);
	if (status != ARCHIVE_OK) {
		notice(tracee, ERROR, INTERNAL, "can't open archive '%s': %s",
			output, archive_error_string(archive->handle));
		return NULL;
	}

	return archive;
}

/**
 * Finalize the given @archive.  This function returns -1 if an error
 * occurred, otherwise 0.
 */
int finalize_archive(Archive *archive)
{
	int status;

	if (archive == NULL || archive->handle == NULL)
		return -1;

	if (archive->hardlink_resolver != NULL)
		archive_entry_linkresolver_free(archive->hardlink_resolver);

	status = archive_write_close(archive->handle);
	if (status != ARCHIVE_OK)
		return -1;

	status = archive_write_free(archive->handle);
	if (status != ARCHIVE_OK)
		return -1;

	return 0;
}

/**
 * Put the content of @path into @archive, with the specified @statl
 * status, at the given @alternate_path (NULL if unchanged).  This
 * function returns -1 if an error occurred, otherwise 0.  Note: this
 * function can be called with @tracee == NULL.
 */
int archive(const Tracee* tracee, Archive *archive,
	const char *path, const char *alternate_path, const struct stat *statl)
{
	struct archive_entry *entry = NULL;
	ssize_t status;
	mode_t type;
	size_t size;
	int fd = -1;

	if (archive == NULL || archive->handle == NULL)
		return -1;

	entry = archive_entry_new();
	if (entry == NULL) {
		notice(tracee, WARNING, INTERNAL, "can't create archive entry for '%s': %s",
			path, archive_error_string(archive->handle));
		status = -1;
		goto end;
	}

	archive_entry_set_pathname(entry, alternate_path ?: path);
	archive_entry_copy_stat(entry, statl);

	if (archive->hardlink_resolver != NULL) {
		struct archive_entry *unused;
		archive_entry_linkify(archive->hardlink_resolver, &entry, &unused);
	}

	/* Get status only once hardlinks were resolved.  */
	size = archive_entry_size(entry);
	type = archive_entry_filetype(entry);

	if (type == AE_IFLNK) {
		char target[PATH_MAX];
		status = readlink(path, target, PATH_MAX);
		if (status >= PATH_MAX) {
			status = -1;
			errno = ENAMETOOLONG;
		}
		if (status < 0) {
			notice(tracee, WARNING, SYSTEM, "can't readlink '%s'", path);
			status = -1;
			goto end;
		}
		target[status] = '\0';

		/* Must be done before archive_write_header().  */
		archive_entry_set_symlink(entry, target);
	}

	status = archive_write_header(archive->handle, entry);
	if (status != ARCHIVE_OK) {
		notice(tracee, WARNING, INTERNAL, "can't write header for '%s': %s",
			path, archive_error_string(archive->handle));
		status = -1;
		goto end;
	}

	/* No content to archive?  */
	if (type != AE_IFREG || size == 0) {
		status = 0;
		goto end;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno != EACCES)
			notice(tracee, WARNING, SYSTEM, "can't open '%s'", path);
		status = -1;
		goto end;
	}

	/* Copy the content from the file into the archive.  */
	do {
		uint8_t buffer[4096];

		status = read(fd, buffer, sizeof(buffer));
		if (status < 0) {
			notice(tracee, WARNING, SYSTEM, "can't read '%s'", path);
			status = -1;
			goto end;
		}

		size = archive_write_data(archive->handle, buffer, status);
		if ((size_t) status != size) {
			notice(tracee, WARNING, INTERNAL, "can't archive '%s' content: %s",
				path, archive_error_string(archive->handle));
			status = -1;
			goto end;
		}
	} while (status > 0);
	status = 0;

end:
	if (fd >= 0)
		(void) close(fd);

	if (entry != NULL)
		archive_entry_free(entry);

	return status;
}

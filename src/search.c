#include "search.h"
#include "scandir.h"

void search_buf(const char *buf, const int buf_len,
                const char *dir_full_path) {
    int binary = -1;  /* 1 = yes, 0 = no, -1 = don't know */
    int buf_offset = 0;

    if (opts.search_stream) {
        binary = 0;
    } else if (!opts.search_binary_files) {
        binary = is_binary((void*) buf, buf_len);
        if (binary) {
            log_debug("File %s is binary. Skipping...", dir_full_path);
            return;
        }
    }

    int matches_len = 0;
    match *matches;
    size_t matches_size;
    size_t matches_spare;

    if (opts.invert_match) {
        /* If we are going to invert the set of matches at the end, we will need
         * one extra match struct, even if there are no matches at all. So make
         * sure we have a nonempty array; and make sure we always have spare
         * capacity for one extra.
         */
        matches_size = 100;
        matches = ag_malloc(matches_size * sizeof(match));
        matches_spare = 1;
    } else {
        matches_size = 0;
        matches = NULL;
        matches_spare = 0;
    }

    if (opts.literal) {
        const char *match_ptr = buf;
        strncmp_fp ag_strnstr_fp = get_strstr(opts);

        while (buf_offset < buf_len) {
            match_ptr = ag_strnstr_fp(match_ptr, opts.query, buf_len - buf_offset, opts.query_len, skip_lookup);
            if (match_ptr == NULL) {
                break;
            }

            if (opts.word_regexp) {
                const char *start = match_ptr;
                const char *end = match_ptr + opts.query_len;

                /* Check whether both start and end of the match lie on a word
                 * boundary
                 */
                if ((start == buf ||
                     is_wordchar(*(start - 1)) != opts.literal_starts_wordchar)
                    &&
                    (end == buf + buf_len ||
                     is_wordchar(*end) != opts.literal_ends_wordchar))
                {
                    /* It's a match */
                } else {
                    /* It's not a match */
                    match_ptr += opts.query_len;
                    buf_offset = end - buf;
                    continue;
                }
            }

            if ((size_t)matches_len + matches_spare >= matches_size) {
                matches_size = matches ? matches_size * 2 : 100;
                log_debug("Too many matches in %s. Reallocating matches to %zu.", dir_full_path, matches_size);
                matches = ag_realloc(matches, matches_size * sizeof(match));
            }

            matches[matches_len].start = match_ptr - buf;
            matches[matches_len].end = matches[matches_len].start + opts.query_len;
            buf_offset = matches[matches_len].end;
            log_debug("Match found. File %s, offset %i bytes.", dir_full_path, matches[matches_len].start);
            matches_len++;
            match_ptr += opts.query_len;

            if (matches_len >= opts.max_matches_per_file) {
                log_err("Too many matches in %s. Skipping the rest of this file.", dir_full_path);
                break;
            }
        }
    } else {
        int rc;
        int offset_vector[3];
        while (buf_offset < buf_len &&
              (rc = pcre_exec(opts.re, opts.re_extra, buf, buf_len, buf_offset, 0, offset_vector, 3)) >= 0) {
            log_debug("Regex match found. File %s, offset %i bytes.", dir_full_path, offset_vector[0]);
            buf_offset = offset_vector[1];

            /* TODO: copy-pasted from above. FIXME */
            if ((size_t)matches_len + matches_spare >= matches_size) {
                matches_size = matches ? matches_size * 2 : 100;
                log_debug("Too many matches in %s. Reallocating matches to %zu.", dir_full_path, matches_size);
                matches = ag_realloc(matches, matches_size * sizeof(match));
            }

            matches[matches_len].start = offset_vector[0];
            matches[matches_len].end = offset_vector[1];
            matches_len++;

            if (matches_len >= opts.max_matches_per_file) {
                log_err("Too many matches in %s. Skipping the rest of this file.", dir_full_path);
                break;
            }
        }
    }

    if (opts.invert_match) {
        matches_len = invert_matches(matches, matches_len, buf_len);
    }

    if (opts.stats) {
        pthread_mutex_lock(&stats_mtx);
        stats.total_bytes += buf_len;
        stats.total_files++;
        stats.total_matches += matches_len;
        pthread_mutex_unlock(&stats_mtx);
    }

    if (matches_len > 0) {
        if (binary == -1 && !opts.print_filename_only) {
            binary = is_binary((void*) buf, buf_len);
        }
        pthread_mutex_lock(&print_mtx);
        if (opts.print_filename_only) {
            print_path(dir_full_path, '\n');
        } else if (binary) {
            print_binary_file_matches(dir_full_path);
        } else {
            print_file_matches(dir_full_path, buf, buf_len, matches, matches_len);
        }
        pthread_mutex_unlock(&print_mtx);
    } else {
        log_debug("No match in %s", dir_full_path);
    }

    if (matches_size > 0) {
        free(matches);
    }
}

/* TODO: this will only match single lines. multi-line regexes silently don't match */
void search_stream(FILE *stream, const char *path) {
    char *line = NULL;
    ssize_t line_len = 0;
    size_t line_cap = 0;

    while ((line_len = getline(&line, &line_cap, stream)) > 0) {
        search_buf(line, line_len, path);
    }

    free(line);
}

void search_file(const char *file_full_path) {
    int fd = -1;
    off_t f_len = 0;
    char *buf = NULL;
    struct stat statbuf;
    int rv = 0;
    FILE *pipe = NULL;

    fd = open(file_full_path, O_RDONLY);
    if (fd < 0) {
        log_err("Error opening file %s: %s Skipping...", file_full_path, strerror(errno));
        goto cleanup;
    }

    rv = fstat(fd, &statbuf);
    if (rv != 0) {
        log_err("Error fstat()ing file %s. Skipping...", file_full_path);
        goto cleanup;
    }

    if ((statbuf.st_mode & S_IFMT) == 0) {
        log_err("%s is not a file. Mode %u. Skipping...", file_full_path, statbuf.st_mode);
        goto cleanup;
    }

    if (statbuf.st_mode & S_IFIFO) {
        log_debug("%s is a named pipe. stream searching", file_full_path);
        pipe = fdopen(fd, "r");
        search_stream(pipe, file_full_path);
        fclose(pipe);
    } else {
        f_len = statbuf.st_size;

        if (f_len == 0) {
            log_debug("File %s is empty, skipping.", file_full_path);
            goto cleanup;
        }

        buf = mmap(0, f_len, PROT_READ, MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED) {
            log_err("File %s failed to load: %s.", file_full_path, strerror(errno));
            goto cleanup;
        }

        search_buf(buf, (int)f_len, file_full_path);
    }

    cleanup:;
    if (fd != -1) {
        munmap(buf, f_len);
        close(fd);
    }
}

void *search_file_worker() {
    work_queue_t *queue_item;

    while (TRUE) {
        pthread_mutex_lock(&work_queue_mtx);
        while (work_queue == NULL) {
            if (done_adding_files) {
                pthread_mutex_unlock(&work_queue_mtx);
                log_debug("Worker finished.");
                pthread_exit(NULL);
            }
            pthread_cond_wait(&files_ready, &work_queue_mtx);
        }
        queue_item = work_queue;
        work_queue = work_queue->next;
        if (work_queue == NULL) {
            work_queue_tail = NULL;
        }
        pthread_mutex_unlock(&work_queue_mtx);

        search_file(queue_item->path);
        free(queue_item->path);
        free(queue_item);
    }
}

/* TODO: Append matches to some data structure instead of just printing them out.
 * Then ag can have sweet summaries of matches/files scanned/time/etc.
 */
void search_dir(ignores *ig, const char *base_path, const char *path, const int depth) {
    struct dirent **dir_list = NULL;
    struct dirent *dir = NULL;
    scandir_baton_t scandir_baton;
    int results = 0;

    char *dir_full_path = NULL;
    const char *ignore_file = NULL;
    int i;

    /* find agignore/gitignore/hgignore/etc files to load ignore patterns from */
    for (i = 0; opts.skip_vcs_ignores ? (i == 0) : (ignore_pattern_files[i] != NULL); i++) {
        ignore_file = ignore_pattern_files[i];
        ag_asprintf(&dir_full_path, "%s/%s", path, ignore_file);
        if (strcmp(SVN_DIR, ignore_file) == 0) {
            load_svn_ignore_patterns(ig, dir_full_path);
        } else {
            load_ignore_patterns(ig, dir_full_path);
        }
        free(dir_full_path);
        dir_full_path = NULL;
    }

    if (opts.path_to_agignore) {
        load_ignore_patterns(ig, opts.path_to_agignore);
    }

    scandir_baton.ig = ig;
    scandir_baton.base_path = base_path;
    results = ag_scandir(path, &dir_list, &filename_filter, &scandir_baton);
    if (results == 0) {
        log_debug("No results found in directory %s", path);
        goto search_dir_cleanup;
    } else if (results == -1) {
        if (errno == ENOTDIR) {
            /* Not a directory. Probably a file. */
            /* If we're only searching one file, don't print the filename header at the top. */
            if (depth == 0 && opts.paths_len == 1) {
                opts.print_heading = -1;
            }
            search_file(path);
        } else {
            log_err("Error opening directory %s: %s", path, strerror(errno));
        }
        goto search_dir_cleanup;
    }

    int offset_vector[3];
    int rc = 0;
    work_queue_t *queue_item;

    for (i = 0; i < results; i++) {
        queue_item = NULL;
        dir = dir_list[i];
        ag_asprintf(&dir_full_path, "%s/%s", path, dir->d_name);

        /* If a link points to a directory then we need to treat it as a directory. */
        if (!opts.follow_symlinks && is_symlink(path, dir)) {
            log_debug("File %s ignored becaused it's a symlink", dir->d_name);
            goto cleanup;
        }

        if (!is_directory(path, dir)) {
            if (opts.file_search_regex) {
                rc = pcre_exec(opts.file_search_regex, NULL, dir_full_path, strlen(dir_full_path),
                               0, 0, offset_vector, 3);
                if (rc < 0) { /* no match */
                    log_debug("Skipping %s due to file_search_regex.", dir_full_path);
                    goto cleanup;
                } else if (opts.match_files) {
                    log_debug("match_files: file_search_regex matched for %s.", dir_full_path);
                    pthread_mutex_lock(&print_mtx);
                    print_path(dir_full_path, '\n');
                    pthread_mutex_unlock(&print_mtx);
                    goto cleanup;
                }
            }

            queue_item = ag_malloc(sizeof(work_queue_t));
            queue_item->path = dir_full_path;
            queue_item->next = NULL;
            pthread_mutex_lock(&work_queue_mtx);
            if (work_queue_tail == NULL) {
                work_queue = queue_item;
            } else {
                work_queue_tail->next = queue_item;
            }
            work_queue_tail = queue_item;
            pthread_mutex_unlock(&work_queue_mtx);
            pthread_cond_signal(&files_ready);
            log_debug("%s added to work queue", dir_full_path);
        } else if (opts.recurse_dirs) {
            if (depth < opts.max_search_depth) {
                log_debug("Searching dir %s", dir_full_path);
                ignores *child_ig = init_ignore(ig);
                search_dir(child_ig, base_path, dir_full_path, depth + 1);
                cleanup_ignore(child_ig);
            } else {
                log_err("Skipping %s. Use the --depth option to search deeper.", dir_full_path);
            }
        }

        cleanup:;
        free(dir);
        dir = NULL;
        if (queue_item == NULL) {
            free(dir_full_path);
            dir_full_path = NULL;
        }
    }

    search_dir_cleanup:;
    free(dir_list);
    dir_list = NULL;
}

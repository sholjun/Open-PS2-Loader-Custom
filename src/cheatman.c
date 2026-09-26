/*
 * Manage cheat codes
 *
 * Copyright (C) 2009-2010 Mathias Lafeldt <misfire@debugon.org>
 * Copyright (C) 2014 doctorxyz
 *
 * This file is part of PS2rd, the PS2 remote debugger.
 *
 * PS2rd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PS2rd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PS2rd.  If not, see <http://www.gnu.org/licenses/>.
 *
 * $Id$
 */

#include <unistd.h>
#include "include/cheatman.h"
#include "include/ioman.h"

static int gEnableCheat; // Enables PS2RD Cheat Engine - 0 for Off, 1 for On
static int gCheatMode;   // Cheat Mode - 0 Enable all cheats, 1 Cheats selected by user

static u32 gCheatList[MAX_CHEATLIST]; // Store hooks/codes addr+val pairs

void InitCheatsConfig(config_set_t *configSet)
{
    config_set_t *configGame = configGetByType(CONFIG_GAME);

    // Default values.
    gCheatSource = 0;
    gEnableCheat = 0;
    gCheatMode = 0;
    memset(gCheatList, 0, sizeof(gCheatList));

    if (configGetInt(configSet, CONFIG_ITEM_CHEATSSOURCE, &gCheatSource)) {
        // Load the rest of the per-game CHEAT configuration if CHEAT is enabled.
        if (configGetInt(configSet, CONFIG_ITEM_ENABLECHEAT, &gEnableCheat) && gEnableCheat) {
            configGetInt(configSet, CONFIG_ITEM_CHEATMODE, &gCheatMode);
        }
    } else {
        if (configGetInt(configGame, CONFIG_ITEM_ENABLECHEAT, &gEnableCheat) && gEnableCheat) {
            configGetInt(configGame, CONFIG_ITEM_CHEATMODE, &gCheatMode);
        }
    }
}

int GetCheatsEnabled(void)
{
    return gEnableCheat;
}

const u32 *GetCheatsList(void)
{
    return gCheatList;
}

/*
 * make_code - Return a code object from string @s.
 */
static code_t make_code(const char *s)
{
    code_t code;
    u32 address;
    u32 value;
    char digits[CODE_DIGITS];
    int i = 0;

    while (*s) {
        if (isxdigit((int)*s))
            digits[i++] = *s;
        s++;
    }

    sscanf(digits, "%08X %08X", &address, &value);

    // Return Code Address and Value
    code.addr = address;
    code.val = value;
    return code;
}


/*
 * is_cheat_code - Return non-zero if @s indicates a cheat code.
 *
 * Example: 10B8DAFA 00003F00
 */
static int is_cheat_code(const char *s)
{
    int i = 0;

    while (*s) {
        if (isxdigit((int)*s)) {
            if (++i > CODE_DIGITS)
                return 0;
        } else if (!isspace((int)*s)) {
            return 0;
        }
        s++;
    }

    return (i == CODE_DIGITS);
}

/*
 * parse_line - Parse the current line and return a filled (code found) or zeroed (code not found) object.
 */
static code_t parse_line(const char *line, int linenumber)
{
    code_t code;
    code.addr = 0;
    code.val = 0;
    int ret;
    LOG("%4i  %s\n", linenumber, line);
    ret = is_cheat_code(line);
    if (ret) {
        /* Process actual code and add it to the list. */
        code = make_code(line);
    }
    return code;
}

/*
 * is_cmt_str - Return non-zero if @s indicates a comment.
 */
static inline int is_cmt_str(const char *s)
{
    return (strlen(s) >= 2 && !strncmp(s, "//", 2)) || (*s == '#');
}

/*
 * chr_idx - Returns the index within @s of the first occurrence of the
 * specified char @c.  If no such char occurs in @s, then (-1) is returned.
 */
static size_t chr_idx(const char *s, char c)
{
    size_t i = 0;

    while (s[i] && (s[i] != c))
        i++;

    return (s[i] == c) ? i : -1;
}

/*
 * term_str - Terminate string @s where the callback functions returns non-zero.
 */
static char *term_str(char *s, int (*callback)(const char *))
{
    if (callback != NULL) {
        while (*s) {
            if (callback(s)) {
                *s = NUL;
                break;
            }
            s++;
        }
    }

    return s;
}

/*
 * is_empty_str - Returns 1 if @s contains no printable chars other than white
 * space.  Otherwise, 0 is returned.
 */
static int is_empty_str(const char *s)
{
    size_t slen = strlen(s);

    while (slen--) {
        if (isgraph((int)*s++))
            return 0;
    }

    return 1;
}

/*
 * trim_str - Removes white space from both ends of the string @s.
 */
static int trim_str(char *s)
{
    size_t first = 0;
    size_t last;
    size_t slen;
    char *t = s;

    /* Return if string is empty */
    if (is_empty_str(s))
        return -1;

    /* Get first non-space char */
    while (isspace((int)*t++))
        first++;

    /* Get last non-space char */
    last = strlen(s) - 1;
    t = &s[last];
    while (isspace((int)*t--))
        last--;

    /* Kill leading/trailing spaces */
    slen = last - first + 1;
    memmove(s, s + first, slen);
    s[slen] = NUL;

    return slen;
}

/*
 * is_empty_substr - Returns 1 if the first @count chars of @s are not printable
 * (apart from white space).  Otherwise, 0 is returned.
 */
static int is_empty_substr(const char *s, size_t count)
{
    while (count--) {
        if (isgraph((int)*s++))
            return 0;
    }

    return 1;
}

/* Max line length to parse */
#define CHEAT_LINE_MAX 255

/**
 * parse_buf - Parse a text buffer for cheats.
 * @buf: buffer holding text (must be NUL-terminated!)
 * @return: 0: success, -1: error
 */
static int parse_buf(const char *buf)
{
    code_t code;
    char line[CHEAT_LINE_MAX + 1];
    int linenumber = 1;

    if (buf == NULL)
        return -1;

    int i = 0;

    while (*buf) {
        /* Scanner */
        int len = chr_idx(buf, LF);
        if (len < 0)
            len = strlen(line);
        else if (len > CHEAT_LINE_MAX)
            len = CHEAT_LINE_MAX;

        if (!is_empty_substr(buf, len)) {
            strncpy(line, buf, len);
            line[len] = NUL;

            /* Screener */
            term_str(line, is_cmt_str);
            trim_str(line);

            /* Parser */
            code = parse_line(line, linenumber);
            if (!((code.addr == 0) && (code.val == 0))) {
                gCheatList[i] = code.addr;
                i++;
                gCheatList[i] = code.val;
                i++;
            }
        }
        linenumber++;
        buf += len + 1;
    }

    gCheatList[i] = 0;
    i++;
    gCheatList[i] = 0;

    return 0;
}

/**
 * read_text_file - Reads text from a file into a buffer.
 * @filename: name of text file
 * @maxsize: max file size (0: arbitrary size)
 * @return: ptr to NULL-terminated text buffer, or NULL if an error occured
 */
static inline char *read_text_file(const char *filename, int maxsize)
{
    char *buf = NULL;
    int fd, filesize;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        LOG("%s: Can't open text file %s\n", __FUNCTION__, filename);
        return NULL;
    }

    filesize = lseek(fd, 0, SEEK_END);
    if (maxsize && filesize > maxsize) {
        LOG("%s: Text file too large: %i bytes, max: %i bytes\n", __FUNCTION__, filesize, maxsize);
        goto end;
    }

    buf = malloc(filesize + 1);
    if (buf == NULL) {
        LOG("%s: Unable to allocate %i bytes\n", __FUNCTION__, filesize + 1);
        goto end;
    }

    if (filesize > 0) {
        lseek(fd, 0, SEEK_SET);
        if (read(fd, buf, filesize) != filesize) {
            LOG("%s: Can't read from text file %s\n", __FUNCTION__, filename);
            free(buf);
            buf = NULL;
            goto end;
        }
    }

    buf[filesize] = '\0';
end:
    close(fd);
    return buf;
}

/*
 * Load cheats from text file.
 */
int load_cheats(const char *cheatfile)
{
    char *buf = NULL;
    int ret;

    memset(gCheatList, 0, sizeof(gCheatList));

    LOG("%s: Reading cheat file '%s'...", __FUNCTION__, cheatfile);
    buf = read_text_file(cheatfile, 0);
    if (buf == NULL) {
        LOG("\n%s: Could not read cheats file '%s'\n", __FUNCTION__, cheatfile);
        return -1;
    }
    LOG("Ok!\n");
    ret = parse_buf(buf);
    free(buf);
    if (ret < 0)
        return -1;
    else
        return 0;
}

static int contains_ignore_case(const char *haystack, const char *needle)
{
    if (!haystack || !needle)
        return 0;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return 1;
        haystack++;
    }
    return 0;
}

static int is_raw_or_commented_code(const char *s, int *is_commented, char *raw_code, int raw_max)
{
    char tmp[CHEAT_LINE_MAX + 1];
    strncpy(tmp, s, CHEAT_LINE_MAX);
    tmp[CHEAT_LINE_MAX] = NUL;
    trim_str(tmp);

    int commented = 0;
    char *p = tmp;
    if (strncmp(p, "//", 2) == 0) {
        commented = 1;
        p += 2;
        trim_str(p);
    } else if (*p == '#') {
        commented = 1;
        p += 1;
        trim_str(p);
    }

    if (is_cheat_code(p)) {
        if (is_commented)
            *is_commented = commented;
        if (raw_code && raw_max > 0) {
            strncpy(raw_code, p, raw_max - 1);
            raw_code[raw_max - 1] = NUL;
        }
        return 1;
    }
    return 0;
}

static void clean_cheat_title(const char *line, char *out_title, int max_len, int *out_is_off)
{
    char tmp[CHEAT_LINE_MAX + 1];
    strncpy(tmp, line, CHEAT_LINE_MAX);
    tmp[CHEAT_LINE_MAX] = NUL;
    trim_str(tmp);

    int is_off = 0;
    char *p = tmp;

    if (strncasecmp(p, "// [OFF]", 8) == 0 || strncasecmp(p, "//[OFF]", 7) == 0) {
        is_off = 1;
        p += (p[7] == ' ' ? 8 : 7);
    } else if (strncasecmp(p, "// [ON]", 7) == 0 || strncasecmp(p, "//[ON]", 6) == 0) {
        is_off = 0;
        p += (p[6] == ' ' ? 7 : 6);
    } else if (strncmp(p, "//", 2) == 0) {
        p += 2;
    } else if (*p == '#') {
        p += 1;
    }

    trim_str(p);
    strncpy(out_title, p, max_len - 1);
    out_title[max_len - 1] = NUL;
    if (out_is_off)
        *out_is_off = is_off;
}

static int has_alpha_or_digit(const char *s)
{
    while (*s) {
        if (isalnum((unsigned char)*s))
            return 1;
        s++;
    }
    return 0;
}

int ParseCheatFileItems(const char *cheatfile, cheat_file_t *out_cheats)
{
    char *buf = NULL;
    char line[CHEAT_LINE_MAX + 1];

    if (!out_cheats)
        return -1;

    memset(out_cheats, 0, sizeof(cheat_file_t));

    buf = read_text_file(cheatfile, 0);
    if (!buf)
        return -1;

    char candidate_title[MAX_CHEAT_NAME_LEN];
    candidate_title[0] = NUL;
    int candidate_is_off = 0;
    int in_cheat_block = 0;

    const char *p = buf;
    while (*p && out_cheats->count < MAX_CHEAT_ITEMS) {
        int len = chr_idx(p, LF);
        if (len < 0)
            len = strlen(p);
        else if (len > CHEAT_LINE_MAX)
            len = CHEAT_LINE_MAX;

        if (!is_empty_substr(p, len)) {
            strncpy(line, p, len);
            line[len] = NUL;
            trim_str(line);

            // Skip game title quotes header e.g. "Game Title /ID ..."
            if (line[0] == '"') {
                candidate_title[0] = NUL;
                in_cheat_block = 0;
            } else {
                int is_commented = 0;
                char raw_code[CHEAT_LINE_MAX + 1];
                if (is_raw_or_commented_code(line, &is_commented, raw_code, sizeof(raw_code))) {
                    if (candidate_title[0] != NUL) {
                        int idx = out_cheats->count;
                        if (idx < MAX_CHEAT_ITEMS) {
                            strncpy(out_cheats->items[idx].name, candidate_title, MAX_CHEAT_NAME_LEN - 1);
                            out_cheats->items[idx].name[MAX_CHEAT_NAME_LEN - 1] = NUL;
                            out_cheats->items[idx].enabled = (!candidate_is_off && !is_commented) ? 1 : 0;
                            out_cheats->items[idx].is_mastercode = contains_ignore_case(candidate_title, "master");
                            out_cheats->count++;
                        }
                        candidate_title[0] = NUL;
                        in_cheat_block = 1;
                    } else if (in_cheat_block) {
                        if (!is_commented && out_cheats->count > 0)
                            out_cheats->items[out_cheats->count - 1].enabled = 1;
                    }
                } else {
                    char clean[MAX_CHEAT_NAME_LEN];
                    int is_off = 0;
                    clean_cheat_title(line, clean, sizeof(clean), &is_off);
                    if (has_alpha_or_digit(clean)) {
                        strncpy(candidate_title, clean, sizeof(candidate_title) - 1);
                        candidate_title[sizeof(candidate_title) - 1] = NUL;
                        candidate_is_off = is_off;
                        in_cheat_block = 0;
                    }
                }
            }
        }

        p += len + 1;
    }

    free(buf);
    return out_cheats->count;
}

int SaveCheatFileItems(const char *cheatfile, const cheat_file_t *in_cheats)
{
    char *buf = NULL;
    char line[CHEAT_LINE_MAX + 1];

    if (!cheatfile || !in_cheats)
        return -1;

    buf = read_text_file(cheatfile, 0);
    if (!buf)
        return -1;

    int orig_size = strlen(buf);
    int max_out_size = orig_size + 16384;
    char *out_buf = malloc(max_out_size);
    if (!out_buf) {
        free(buf);
        return -1;
    }
    out_buf[0] = NUL;
    int out_len = 0;

    int current_cheat_idx = -1;
    const char *p = buf;

    while (*p) {
        int len = chr_idx(p, LF);
        int has_lf = (len >= 0);
        if (len < 0)
            len = strlen(p);
        else if (len > CHEAT_LINE_MAX)
            len = CHEAT_LINE_MAX;

        strncpy(line, p, len);
        line[len] = NUL;

        char trimmed[CHEAT_LINE_MAX + 1];
        strncpy(trimmed, line, len);
        trimmed[len] = NUL;
        trim_str(trimmed);

        if (trimmed[0] == '"') {
            current_cheat_idx = -1;
            out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", line);
        } else if (is_empty_str(trimmed)) {
            out_len += snprintf(out_buf + out_len, max_out_size - out_len, "\n");
        } else {
            int is_commented = 0;
            char raw_code[CHEAT_LINE_MAX + 1];
            if (is_raw_or_commented_code(trimmed, &is_commented, raw_code, sizeof(raw_code))) {
                if (current_cheat_idx >= 0 && current_cheat_idx < in_cheats->count) {
                    if (in_cheats->items[current_cheat_idx].enabled) {
                        out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", raw_code);
                    } else {
                        out_len += snprintf(out_buf + out_len, max_out_size - out_len, "// %s\n", raw_code);
                    }
                } else {
                    out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", line);
                }
            } else {
                char clean[MAX_CHEAT_NAME_LEN];
                int is_off = 0;
                clean_cheat_title(trimmed, clean, sizeof(clean), &is_off);

                int matched_idx = -1;
                if (has_alpha_or_digit(clean)) {
                    for (int i = 0; i < in_cheats->count; ++i) {
                        if (strcasecmp(in_cheats->items[i].name, clean) == 0) {
                            matched_idx = i;
                            break;
                        }
                    }
                }

                if (matched_idx >= 0) {
                    current_cheat_idx = matched_idx;
                    if (in_cheats->items[matched_idx].is_mastercode) {
                        out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", in_cheats->items[matched_idx].name);
                    } else if (in_cheats->items[matched_idx].enabled) {
                        out_len += snprintf(out_buf + out_len, max_out_size - out_len, "// %s\n", in_cheats->items[matched_idx].name);
                    } else {
                        out_len += snprintf(out_buf + out_len, max_out_size - out_len, "// [OFF] %s\n", in_cheats->items[matched_idx].name);
                    }
                } else {
                    // Non-cheat comment or metadata (like //ELF CRC=...)
                    out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", line);
                }
            }
        }

        if (has_lf)
            p += len + 1;
        else
            break;
    }

    free(buf);

    int fd = open(cheatfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        free(out_buf);
        return -1;
    }

    write(fd, out_buf, out_len);
    close(fd);
    free(out_buf);

    return 0;
}


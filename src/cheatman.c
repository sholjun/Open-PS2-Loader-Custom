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
                p += len + 1;
                continue;
            }

            // Check if this is a disabled cheat title: e.g. "// [OFF] Infinite Health"
            if (strncmp(line, "// [OFF]", 8) == 0 || strncmp(line, "//[OFF]", 7) == 0) {
                char *name = line + (line[7] == ' ' ? 8 : 7);
                trim_str(name);
                if (strlen(name) > 0) {
                    int idx = out_cheats->count;
                    strncpy(out_cheats->items[idx].name, name, MAX_CHEAT_NAME_LEN - 1);
                    out_cheats->items[idx].name[MAX_CHEAT_NAME_LEN - 1] = NUL;
                    out_cheats->items[idx].enabled = 0;
                    out_cheats->items[idx].is_mastercode = contains_ignore_case(name, "master");
                    out_cheats->count++;
                }
            }
            // Check if this is a raw hex cheat code: "XXXXXXXX YYYYYYYY"
            else if (is_cheat_code(line)) {
                // Code line, skip
            }
            // Check if general comment
            else if (is_cmt_str(line)) {
                // Section comment, skip
            }
            // Otherwise, it's an ACTIVE cheat title!
            else {
                if (strlen(line) > 1) {
                    int idx = out_cheats->count;
                    strncpy(out_cheats->items[idx].name, line, MAX_CHEAT_NAME_LEN - 1);
                    out_cheats->items[idx].name[MAX_CHEAT_NAME_LEN - 1] = NUL;
                    out_cheats->items[idx].enabled = 1;
                    out_cheats->items[idx].is_mastercode = contains_ignore_case(line, "master");
                    out_cheats->count++;
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
    int max_out_size = orig_size + 8192;
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
            int found_cheat = -1;

            if (strncmp(trimmed, "// [OFF]", 8) == 0 || strncmp(trimmed, "//[OFF]", 7) == 0) {
                char *name = trimmed + (trimmed[7] == ' ' ? 8 : 7);
                trim_str(name);
                for (int i = 0; i < in_cheats->count; ++i) {
                    if (strcasecmp(in_cheats->items[i].name, name) == 0) {
                        found_cheat = i;
                        break;
                    }
                }
            } else if (!is_cmt_str(trimmed) && !is_cheat_code(trimmed)) {
                for (int i = 0; i < in_cheats->count; ++i) {
                    if (strcasecmp(in_cheats->items[i].name, trimmed) == 0) {
                        found_cheat = i;
                        break;
                    }
                }
            }

            if (found_cheat >= 0) {
                current_cheat_idx = found_cheat;
                if (in_cheats->items[found_cheat].enabled) {
                    out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", in_cheats->items[found_cheat].name);
                } else {
                    out_len += snprintf(out_buf + out_len, max_out_size - out_len, "// [OFF] %s\n", in_cheats->items[found_cheat].name);
                }
            } else if (current_cheat_idx >= 0) {
                char clean_code[CHEAT_LINE_MAX + 1];
                strncpy(clean_code, trimmed, CHEAT_LINE_MAX);
                clean_code[CHEAT_LINE_MAX] = NUL;

                char *code_ptr = clean_code;
                if (strncmp(code_ptr, "//", 2) == 0) {
                    code_ptr += 2;
                    trim_str(code_ptr);
                }

                if (is_cheat_code(code_ptr)) {
                    if (in_cheats->items[current_cheat_idx].enabled) {
                        out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", code_ptr);
                    } else {
                        out_len += snprintf(out_buf + out_len, max_out_size - out_len, "// %s\n", code_ptr);
                    }
                } else {
                    out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", line);
                }
            } else {
                out_len += snprintf(out_buf + out_len, max_out_size - out_len, "%s\n", line);
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


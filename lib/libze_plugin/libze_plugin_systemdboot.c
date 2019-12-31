#include "libze_plugin/libze_plugin_systemdboot.h"

#include "libze/libze_util.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REGEX_BUFLEN 512
#define REGEX_MAX_MATCHES 25
#define SYSTEMDBOOT_ENTRY_PREFIX "org.zectl"

#define NUM_SYSTEMDBOOT_PROPERTY_VALUES 2
#define NUM_SYSTEMDBOOT_PROPERTIES 2
char const *systemdboot_properties[NUM_SYSTEMDBOOT_PROPERTIES][NUM_SYSTEMDBOOT_PROPERTY_VALUES] = {
    {"efi", "/efi"}, {"boot", "/boot"}};

struct replace_matched_data {
    char const *be_name;
    char const *active_be;
};

struct replace_fstab_data {
    char const *be_name;
    char const *active_be;
    char const *boot_mountpoint;
    char const *efi_mountpoint;
};

typedef libze_error (*line_replace_fn)(libze_handle *lzeh, void *data,
                                       char const line_buf[LIBZE_MAX_PATH_LEN],
                                       char replace_line_buf[LIBZE_MAX_PATH_LEN]);

libze_error
libze_plugin_systemdboot_defaults(libze_handle *lzeh, nvlist_t **default_properties) {
    libze_error ret = LIBZE_ERROR_SUCCESS;
    nvlist_t *properties = NULL;

    properties = fnvlist_alloc();
    if (properties == NULL) {
        return libze_error_nomem(lzeh);
    }
    char namespace_buf[ZFS_MAXPROPLEN];
    if (libze_plugin_form_namespace(PLUGIN_SYSTEMDBOOT, namespace_buf) != LIBZE_ERROR_SUCCESS) {
        ret = libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN, "Exceeded max property name length.\n");
        goto err;
    }
    for (int i = 0; i < NUM_SYSTEMDBOOT_PROPERTIES; i++) {
        if (libze_default_prop_add(&properties, systemdboot_properties[i][0],
                                   systemdboot_properties[i][1], namespace_buf) != 0) {
            ret = libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN,
                                  "Failed to add %s property to systemdboot nvlist.\n",
                                  systemdboot_properties[i][0]);
            goto err;
        }
    }

    *default_properties = properties;
    return ret;
err:
    (void) libze_list_free(properties);
    return ret;
}

static libze_error
add_default_properties(libze_handle *lzeh) {
    libze_error ret = LIBZE_ERROR_SUCCESS;

    nvlist_t *defaults_nvl = NULL;
    if ((ret = libze_plugin_systemdboot_defaults(lzeh, &defaults_nvl)) != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    // Add defaults
    nvpair_t *default_pair = NULL;
    for (default_pair = nvlist_next_nvpair(defaults_nvl, NULL); default_pair != NULL;
         default_pair = nvlist_next_nvpair(defaults_nvl, default_pair)) {
        char const *default_nvp_name = nvpair_name(default_pair);

        // If nvlist already in properties, don't add default
        if (nvlist_exists(lzeh->ze_props, default_nvp_name)) {
            continue;
        }

        nvlist_t *nvl = NULL;
        if (nvpair_value_nvlist(default_pair, &nvl) != 0) {
            ret = libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, "Failed to access nvlist %s.\n",
                                  default_nvp_name);
            goto err;
        }
        nvlist_t *nvl_copy = NULL;
        if (nvlist_dup(nvl, &nvl_copy, 0) != 0) {
            ret = libze_error_nomem(lzeh);
            goto err;
        }
        if (nvlist_add_nvlist(lzeh->ze_props, default_nvp_name, nvl_copy) != 0) {
            ret = libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, "Failed adding default property %s.\n",
                                  default_nvp_name);
            goto err;
        }
    }

err:
    if (defaults_nvl != NULL) {
        (void) libze_list_free(defaults_nvl);
    }
    return ret;
}

/**
 * @brief
 * @param lzeh
 * @pre lzeh->ze_props is allocated
 * @return
 */
libze_error
libze_plugin_systemdboot_init(libze_handle *lzeh) {
    libze_error ret = LIBZE_ERROR_SUCCESS;

    if ((ret = add_default_properties(lzeh)) != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    return ret;
}

libze_error
libze_plugin_systemdboot_pre_activate(libze_handle *lzeh) {
    return 0;
}

static libze_error
form_fstab_regex(regex_t *re, char const boot_mountpoint[LIBZE_MAX_PATH_LEN],
                 char const efi_mountpoint[LIBZE_MAX_PATH_LEN],
                 char const be_name[LIBZE_MAX_PATH_LEN], char reg_buf[REGEX_BUFLEN]) {

    libze_error ret = LIBZE_ERROR_SUCCESS;

    char be_no_dots[ZFS_MAX_DATASET_NAME_LEN];
    char ns_no_dots[ZFS_MAX_DATASET_NAME_LEN];
    char efi_no_dots[ZFS_MAX_DATASET_NAME_LEN];
    char boot_no_dots[ZFS_MAX_DATASET_NAME_LEN];

    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, be_name,
                                    ZFS_MAX_DATASET_NAME_LEN, be_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, ZE_PROP_NAMESPACE,
                                    ZFS_MAX_DATASET_NAME_LEN, ns_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, efi_mountpoint,
                                    ZFS_MAX_DATASET_NAME_LEN, efi_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, boot_mountpoint,
                                    ZFS_MAX_DATASET_NAME_LEN, boot_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    int intret = snprintf(reg_buf, REGEX_BUFLEN, "\\(^[\t ]*%s/env/%s-\\)\\(%s\\)\\([\t ]*%s.*$\\)",
                          efi_no_dots, ns_no_dots, be_no_dots, boot_no_dots);

    if (intret >= REGEX_BUFLEN) {
        return LIBZE_ERROR_MAXPATHLEN;
    }

    if (regcomp(re, reg_buf, 0) != 0) {
        return LIBZE_ERROR_UNKNOWN;
    }

    return LIBZE_ERROR_SUCCESS;
}

static libze_error
form_loader_path(char const efi_mountpoint[LIBZE_MAX_PATH_LEN],
                 char const middle_dir[LIBZE_MAX_PATH_LEN], char const be_name[LIBZE_MAX_PATH_LEN],
                 char loader_buf[LIBZE_MAX_PATH_LEN]) {

    int ret = snprintf(loader_buf, LIBZE_MAX_PATH_LEN, "%s/%s/%s-%s", efi_mountpoint, middle_dir,
                       SYSTEMDBOOT_ENTRY_PREFIX, be_name);

    if (ret >= REGEX_BUFLEN) {
        return LIBZE_ERROR_MAXPATHLEN;
    }

    return LIBZE_ERROR_SUCCESS;
}

static libze_error
form_loader_config(char const efi_mountpoint[LIBZE_MAX_PATH_LEN],
                   char const be_name[LIBZE_MAX_PATH_LEN], char loader_buf[LIBZE_MAX_PATH_LEN]) {

    libze_error ret = form_loader_path(efi_mountpoint, "loader/entries", be_name, loader_buf);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    if (strlcat(loader_buf, ".conf", LIBZE_MAX_PATH_LEN) >= LIBZE_MAX_PATH_LEN) {
        return LIBZE_ERROR_MAXPATHLEN;
    }

    return LIBZE_ERROR_SUCCESS;
}

static libze_error
file_accessible(libze_handle *lzeh, char const unit_buf[LIBZE_MAX_PATH_LEN]) {
    /* Check if boot.mount is r/w */
    errno = 0;
    if (access(unit_buf, R_OK | W_OK) != 0) {
        switch (errno) {
            case EACCES:
                return libze_error_set(lzeh, LIBZE_ERROR_EPERM,
                                       "Boot mountpoint unit %s in not in read/write mode.\n",
                                       unit_buf);
            case ENAMETOOLONG:
                return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                                       "Boot mountpoint unit path exceeds max path length.\n");
            default:
                return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN,
                                       "Boot mountpoint unit %s could not be accessed.\n",
                                       unit_buf);
        }
    }

    return LIBZE_ERROR_SUCCESS;
}

static libze_error
form_title_regex(regex_t *re, char const be_name[ZFS_MAX_DATASET_NAME_LEN]) {

    char reg_buf[REGEX_BUFLEN];
    libze_error ret = LIBZE_ERROR_SUCCESS;

    ret = libze_util_concat("\\(title.*\\)\\(", be_name, "\\)\\(.*\\)", REGEX_BUFLEN, reg_buf);

    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    if (regcomp(re, reg_buf, 0) != 0) {
        return LIBZE_ERROR_UNKNOWN;
    }

    return ret;
}

static libze_error
form_linux_regex(regex_t *re, char const be_name[ZFS_MAX_DATASET_NAME_LEN]) {

    char reg_buf[REGEX_BUFLEN];

    char be_no_dots[ZFS_MAX_DATASET_NAME_LEN] = "";
    char ns_no_dots[ZFS_MAX_DATASET_NAME_LEN] = "";
    libze_error ret = LIBZE_ERROR_SUCCESS;
    int iret = 0;

    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, be_name,
                                    ZFS_MAX_DATASET_NAME_LEN, be_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, ZE_PROP_NAMESPACE,
                                    ZFS_MAX_DATASET_NAME_LEN, ns_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    iret = snprintf(reg_buf, REGEX_BUFLEN, "\\(linux\\|initrd\\)\\(.*\\)\\(%s-\\)\\(%s\\)\\(/.*\\)",
                    ns_no_dots, be_no_dots);

    if (iret >= REGEX_BUFLEN) {
        return LIBZE_ERROR_MAXPATHLEN;
    }

    if (regcomp(re, reg_buf, 0) != 0) {
        return LIBZE_ERROR_UNKNOWN;
    }

    return LIBZE_ERROR_SUCCESS;
}

static libze_error
form_dataset_regex(regex_t *re, char const be_root[LIBZE_MAX_PATH_LEN],
                   char const be_name[ZFS_MAX_DATASET_NAME_LEN]) {

    char reg_buf[REGEX_BUFLEN];

    char be_no_dots[ZFS_MAX_DATASET_NAME_LEN] = "";
    char be_root_no_dots[ZFS_MAX_DATASET_NAME_LEN] = "";
    libze_error ret = LIBZE_ERROR_SUCCESS;

    int iret = 0;

    /* Dots must be replaced with \. */
    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, be_name,
                                    ZFS_MAX_DATASET_NAME_LEN, be_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    ret = libze_util_replace_string(".", "\\.", ZFS_MAX_DATASET_NAME_LEN, be_root,
                                    ZFS_MAX_DATASET_NAME_LEN, be_root_no_dots);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    iret = snprintf(reg_buf, REGEX_BUFLEN, "\\(options.*zfs=%s/\\)\\(%s\\)\\(.*\\)",
                    be_root_no_dots, be_no_dots);

    if (iret >= REGEX_BUFLEN) {
        return LIBZE_ERROR_MAXPATHLEN;
    }

    if (regcomp(re, reg_buf, 0) != 0) {
        return LIBZE_ERROR_UNKNOWN;
    }

    return LIBZE_ERROR_SUCCESS;
}

static libze_error
get_cfg_line_from_regex(libze_handle *lzeh, void *data, char const line[LIBZE_MAX_PATH_LEN],
                        char replace_line_buf[LIBZE_MAX_PATH_LEN]) {
    // TODO: Add errors via lzeh
    libze_error ret = LIBZE_ERROR_SUCCESS;

    struct replace_matched_data *rmd = data;

    regmatch_t pmatch;

    regex_t re_title_buf, re_linux_buf, re_dataset_buf;
    regex_t *re_title_p = NULL, *re_linux_p = NULL, *re_dataset_p = NULL;

    ret = form_linux_regex(&re_linux_buf, rmd->active_be);
    if (ret != LIBZE_ERROR_SUCCESS) {
        goto done;
    }
    re_linux_p = &re_linux_buf;

    ret = form_title_regex(&re_title_buf, rmd->active_be);
    if (ret != LIBZE_ERROR_SUCCESS) {
        goto done;
    }
    re_title_p = &re_title_buf;

    ret = form_dataset_regex(&re_dataset_buf, lzeh->env_root, rmd->active_be);
    if (ret != LIBZE_ERROR_SUCCESS) {
        goto done;
    }
    re_dataset_p = &re_dataset_buf;

    char replace_two[LIBZE_MAX_PATH_LEN] = "";
    char replace_four[LIBZE_MAX_PATH_LEN] = "";

    ret = libze_util_concat("\\1", rmd->be_name, "\\3", LIBZE_MAX_PATH_LEN, replace_two);
    if (ret == LIBZE_ERROR_SUCCESS) {
        ret = libze_util_concat("\\1\\2\\3", rmd->be_name, "\\5", LIBZE_MAX_PATH_LEN, replace_four);
    }
    if (ret != LIBZE_ERROR_SUCCESS) {
        goto done;
    }

    if (regexec(re_title_p, line, 0, &pmatch, 0) == 0) {
        ret = libze_util_regex_subexpr_replace(re_title_p, LIBZE_MAX_PATH_LEN, replace_two,
                                               LIBZE_MAX_PATH_LEN, line, LIBZE_MAX_PATH_LEN,
                                               replace_line_buf);
        goto done;
    }

    if (regexec(re_linux_p, line, 0, &pmatch, 0) == 0) {
        ret = libze_util_regex_subexpr_replace(re_linux_p, LIBZE_MAX_PATH_LEN, replace_four,
                                               LIBZE_MAX_PATH_LEN, line, LIBZE_MAX_PATH_LEN,
                                               replace_line_buf);
        goto done;
    }

    if (regexec(re_dataset_p, line, 0, &pmatch, 0) == 0) {
        ret = libze_util_regex_subexpr_replace(re_dataset_p, LIBZE_MAX_PATH_LEN, replace_two,
                                               LIBZE_MAX_PATH_LEN, line, LIBZE_MAX_PATH_LEN,
                                               replace_line_buf);
        goto done;
    }

    if (strlcpy(replace_line_buf, line, LIBZE_MAX_PATH_LEN) >= LIBZE_MAX_PATH_LEN) {
        ret = LIBZE_ERROR_MAXPATHLEN;
    }

done:
    if (re_title_p != NULL) {
        regfree(re_title_p);
    }
    if (re_linux_p != NULL) {
        regfree(re_linux_p);
    }
    if (re_dataset_p != NULL) {
        regfree(re_dataset_p);
    }

    return ret;
}

static libze_error
get_fstab_line_from_regex(libze_handle *lzeh, void *data, char const line[LIBZE_MAX_PATH_LEN],
                          char replace_line_buf[LIBZE_MAX_PATH_LEN]) {
    // TODO: Add errors via lzeh
    libze_error ret = LIBZE_ERROR_SUCCESS;

    struct replace_fstab_data *rmd = data;

    regmatch_t pmatch;

    regex_t re_boot_buf;
    regex_t *re_boot_p = NULL;

    char fstab_re_buf[LIBZE_MAX_PATH_LEN] = "";
    ret = form_fstab_regex(&re_boot_buf, rmd->boot_mountpoint, rmd->efi_mountpoint, rmd->active_be,
                           fstab_re_buf);
    if (ret != LIBZE_ERROR_SUCCESS) {
        goto done;
    }
    re_boot_p = &re_boot_buf;

    char replace_two[LIBZE_MAX_PATH_LEN] = "";
    char replace_four[LIBZE_MAX_PATH_LEN] = "";

    ret = libze_util_concat("\\1", rmd->be_name, "\\3", LIBZE_MAX_PATH_LEN, replace_two);
    if (ret == LIBZE_ERROR_SUCCESS) {
        ret = libze_util_concat("\\1\\2\\3", rmd->be_name, "\\5", LIBZE_MAX_PATH_LEN, replace_four);
    }
    if (ret != LIBZE_ERROR_SUCCESS) {
        goto done;
    }

    if (regexec(re_boot_p, line, 0, &pmatch, 0) == 0) {
        ret = libze_util_regex_subexpr_replace(re_boot_p, LIBZE_MAX_PATH_LEN, replace_two,
                                               LIBZE_MAX_PATH_LEN, line, LIBZE_MAX_PATH_LEN,
                                               replace_line_buf);
        goto done;
    }

    if (strlcpy(replace_line_buf, line, LIBZE_MAX_PATH_LEN) >= LIBZE_MAX_PATH_LEN) {
        ret = LIBZE_ERROR_MAXPATHLEN;
    }

done:
    if (re_boot_p != NULL) {
        regfree(re_boot_p);
    }

    return ret;
}

/**
 * @brief Loop over each line of a file replacing each line according to a regular expression.
 *        The file at @p filename_new will be erased upon opening.
 *
 * @param lzeh            libze handle
 * @param be_name         New boot environment
 * @param be_name_active  Active boot environment
 * @param filename        Old configuration file
 * @param new_filename    New configuration file
 * @return @p LIBZE_ERROR_SUCCESS on success,
 *         @p LIBZE_ERROR_MAXPATHLEN on buffer length being exceeded
 */
static libze_error
replace_matched(libze_handle *lzeh, char const filename[LIBZE_MAX_PATH_LEN],
                char const filename_new[LIBZE_MAX_PATH_LEN], line_replace_fn replace_fn,
                void *data) {

    libze_error ret = LIBZE_ERROR_SUCCESS;

    char *open_err = "Failed to open %s.\n";

    FILE *file_new = fopen(filename_new, "w+");
    if (file_new == NULL) {
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, open_err, file_new);
    }

    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        fclose(file_new);
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, open_err, filename);
    }

    char line_buf[LIBZE_MAX_PATH_LEN];
    char replace_line_buf[LIBZE_MAX_PATH_LEN] = "";
    while (fgets(line_buf, LIBZE_MAX_PATH_LEN, file)) {
        /* Get the new line to be written out according to a regular expression
           If there's no match, original line will be in replace_line_buf */
        ret = replace_fn(lzeh, data, line_buf, replace_line_buf);
        if (ret != LIBZE_ERROR_SUCCESS) {
            goto err;
        }
        fwrite(replace_line_buf, 1, strlen(replace_line_buf), file_new);
    }

    fflush(file_new);

err:
    fclose(file);
    fclose(file_new);

    return LIBZE_ERROR_SUCCESS;
}

/**
 * @brief Replace a boot environment name in an old configuration file writing out the
 *        changes to a new configuration file.
 *
 * @param lzeh         libze handle
 * @param be_name      New boot environment
 * @param active_be    Active boot environment
 * @param filename     Old configuration file
 * @param new_filename New configuration file
 *
 * @return @p LIBZE_ERROR_SUCCESS on success,
 *         @p LIBZE_ERROR_MAXPATHLEN on buffer being exceeded,
 *         @p LIBZE_ERROR_EPERM upon missing permissions to open file,
 *         @p LIBZE_ERROR_UNKNOWN upon file could not be accessed
 */
static libze_error
replace_be_name(libze_handle *lzeh, char const be_name[ZFS_MAX_DATASET_NAME_LEN],
                char const active_be[ZFS_MAX_DATASET_NAME_LEN],
                char const filename[LIBZE_MAX_PATH_LEN],
                char const new_filename[LIBZE_MAX_PATH_LEN]) {

    libze_error ret = LIBZE_ERROR_SUCCESS;

    if ((ret = file_accessible(lzeh, filename)) != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    /* Setup regular expression */
    char reg_buf[REGEX_BUFLEN];
    if (strlcpy(reg_buf, active_be, ZFS_MAX_DATASET_NAME_LEN) >= ZFS_MAX_DATASET_NAME_LEN) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN, "Regex exceeds max path length.\n");
    }

    struct replace_matched_data data = {.be_name = be_name, .active_be = active_be};

    ret = replace_matched(lzeh, filename, new_filename, get_cfg_line_from_regex, &data);

    return ret;
}

/**
 * @brief Using boot mountpoint update fstab
 *
 * @param lzeh               libze handle
 * @param boot_mountpoint    Mountpoint of boot partition
 * @param be_mountpoint      BE mountpoint
 * @param efi_mountpoint      EFI mountpoint
 * @return                   @p LIBZE_ERROR_UNKNOWN Boot mountpoint is not set
 *                           @p LIBZE_ERROR_MAXPATHLEN Max path length exceeded
 *                           @p LIBZE_ERROR_SUCCESS On success
 */
static libze_error
update_fstab(libze_handle *lzeh, libze_activate_data *activate_data,
             char const boot_mountpoint[LIBZE_MAX_PATH_LEN],
             char const efi_mountpoint[LIBZE_MAX_PATH_LEN]) {

    libze_error ret = LIBZE_ERROR_SUCCESS;
    int interr = 0;

    char active_be[ZFS_MAX_DATASET_NAME_LEN];
    if (libze_boot_env_name(lzeh->env_activated_path, ZFS_MAX_DATASET_NAME_LEN, active_be) != 0) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN, "Bootfs exceeds max path length.\n");
    }

    /* Get path to fstab */
    char fstab_buf[LIBZE_MAX_PATH_LEN] = "";
    if ((strlcpy(fstab_buf, activate_data->be_mountpoint, LIBZE_MAX_PATH_LEN) >=
         LIBZE_MAX_PATH_LEN) ||
        (strlcat(fstab_buf, "/etc/fstab", LIBZE_MAX_PATH_LEN) >= LIBZE_MAX_PATH_LEN)) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                               "fstab exceeds max path length (%s).\n", LIBZE_MAX_PATH_LEN);
    }

    if ((ret = file_accessible(lzeh, fstab_buf)) != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    char new_filename[LIBZE_MAX_PATH_LEN];
    int err = libze_util_concat(fstab_buf, ".", "bak", LIBZE_MAX_PATH_LEN, new_filename);
    if (err != 0) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                               "Backup fstab exceeds max path length.\n");
    }

    /* backup unit */
    if ((ret = libze_util_copy_file(fstab_buf, new_filename)) != 0) {
        return ret;
    }

    /* Create a tempfile to manipulate before replacing original */
    char tmpfile[LIBZE_MAX_PATH_LEN];
    interr = libze_util_concat(fstab_buf, ".", ".zectl-sdboot.XXXXXX", LIBZE_MAX_PATH_LEN, tmpfile);

    if (interr != 0) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                               "Temporary boot mount unit exceeds max path length.\n");
    }

    int fd = mkstemp(tmpfile);
    if (fd == -1) {
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, "Failed to create temporary file\n");
    }

    struct replace_fstab_data data = {.active_be = active_be,
                                      .be_name = activate_data->be_name,
                                      .boot_mountpoint = boot_mountpoint,
                                      .efi_mountpoint = efi_mountpoint};

    ret = replace_matched(lzeh, fstab_buf, tmpfile, get_fstab_line_from_regex, &data);
    if (ret != LIBZE_ERROR_SUCCESS) {
        remove(tmpfile);
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, "Failed to replace lines in %s.\n",
                               tmpfile);
    }

    errno = 0;
    /* Use rename for atomicity */
    interr = rename(tmpfile, fstab_buf);
    if (interr != 0) {
        // TODO: Better error message from errno
        remove(tmpfile);
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, "Failed to replace %s.\n", fstab_buf);
    }

    return ret;
}

/**
 * @brief Run mid-activate hook
 * @param lzeh Initialized libze handle
 * @param be_mountpoint
 * @param be_name New be
 * @return Non-zero on failure
 */
libze_error
libze_plugin_systemdboot_mid_activate(libze_handle *lzeh, libze_activate_data *activate_data) {

    libze_error ret = LIBZE_ERROR_SUCCESS;

    char boot_mountpoint[ZFS_MAXPROPLEN];
    char efi_mountpoint[ZFS_MAXPROPLEN];

    char namespace_buf[ZFS_MAXPROPLEN];
    if (libze_plugin_form_namespace(PLUGIN_SYSTEMDBOOT, namespace_buf) !=
        LIBZE_PLUGIN_MANAGER_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                               "Exceeded max property name length.\n");
    }

    ret = libze_be_prop_get(lzeh, boot_mountpoint, "boot", namespace_buf);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN,
                               "Couldn't access systemdboot:boot property.\n");
    }
    ret = libze_be_prop_get(lzeh, efi_mountpoint, "efi", namespace_buf);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN,
                               "Couldn't access systemdboot:efi property.\n");
    }

    ret = update_fstab(lzeh, activate_data, boot_mountpoint, efi_mountpoint);

    return ret;
}

/**
 * @brief Run mid-activate hook
 * @param lzeh Initialized libze handle
 * @return Non @p LIBZE_ERROR_SUCCESS on failure
 */
libze_error
libze_plugin_systemdboot_post_activate(libze_handle *lzeh, char const be_name[LIBZE_MAX_PATH_LEN]) {
    /*
     * Steps:
     *   - Copy <esp>/loader/entries/<prefix>-<oldbe>.conf -> <prefix>-<be>.conf
     *   - Modify esp/loader/entries/org.zectl-<be>.conf
     *       - Replace <oldbe> with <newbe>
     *   - Copy old kernels to esp/env/org.zectl-<be>/
     *   - Modify loader.conf
     */

    libze_error ret = LIBZE_ERROR_SUCCESS;
    int iret = 0;

    char active_be[ZFS_MAX_DATASET_NAME_LEN];
    if (libze_boot_env_name(lzeh->env_activated_path, ZFS_MAX_DATASET_NAME_LEN, active_be) != 0) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN, "Bootfs exceeds max path length.\n");
    }

    char boot_mountpoint[ZFS_MAXPROPLEN];
    char efi_mountpoint[ZFS_MAXPROPLEN];

    char namespace_buf[ZFS_MAXPROPLEN];
    if (libze_plugin_form_namespace(PLUGIN_SYSTEMDBOOT, namespace_buf) !=
        LIBZE_PLUGIN_MANAGER_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                               "Exceeded max property name length.\n");
    }

    ret = libze_be_prop_get(lzeh, boot_mountpoint, "boot", namespace_buf);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN,
                               "Couldn't access systemdboot:boot property.\n");
    }
    ret = libze_be_prop_get(lzeh, efi_mountpoint, "efi", namespace_buf);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN,
                               "Couldn't access systemdboot:efi property.\n");
    }

    /* Copy <esp>/loader/entries/<prefix>-<oldbe>.conf -> <prefix>-<be>.conf */
    char loader_buf[LIBZE_MAX_PATH_LEN];
    char new_loader_buf[LIBZE_MAX_PATH_LEN];

    ret = form_loader_config(efi_mountpoint, active_be, loader_buf);
    if (ret == LIBZE_ERROR_SUCCESS) {
        ret = form_loader_config(efi_mountpoint, be_name, new_loader_buf);
    }

    if (ret != LIBZE_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                               "BE loader path exceeds max path length.\n");
    }

    ret = replace_be_name(lzeh, be_name, active_be, loader_buf, new_loader_buf);
    if (ret != LIBZE_ERROR_SUCCESS) {
        return libze_error_set(lzeh, LIBZE_ERROR_MAXPATHLEN,
                               "Failed to replace '%s' in '%s' with '%s'.\n", active_be,
                               new_loader_buf, be_name);
    }

    ret = form_loader_path(efi_mountpoint, "env", active_be, loader_buf);
    if (ret == LIBZE_ERROR_SUCCESS) {
        ret = form_loader_path(efi_mountpoint, "env", be_name, new_loader_buf);
    }
    if (ret != LIBZE_ERROR_SUCCESS) {
        return ret;
    }

    iret = libze_util_copydir(loader_buf, new_loader_buf);
    if (iret != 0) {
        // TODO: Check error, return better message
        return libze_error_set(lzeh, LIBZE_ERROR_UNKNOWN, "Failed to copy %s -> %s.\n", loader_buf,
                               new_loader_buf);
    }

    return ret;
}

libze_error
libze_plugin_systemdboot_post_destroy(libze_handle *lzeh, char const be_name[LIBZE_MAX_PATH_LEN]) {
    puts("sd_post_destroy");

    return 0;
}